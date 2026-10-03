#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""No serial hardware: exercise the actual HTTP gateway with a mock agent."""
import base64
import http.client
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gateway", ROOT / "remote/gateway.py")
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)


class MockAgent:
    def __init__(self):
        self.callback = lambda event: None
        self.calls = []
        self.opened = False

    def call(self, request):
        self.calls.append(dict(request))
        op = request["op"]
        if op == "devices":
            return {"ok": True, "devices": ["/mock/serial"]}
        if op == "open":
            self.opened = True
            self.callback({"event": "status", "connected": True, "device": "/mock/serial", "message": "Connected"})
        if op == "close":
            self.opened = False
            self.callback({"event": "done"})
        if op == "send":
            self.callback({"event": "tx", "data": request["data"]})
            self.callback({"event": "rx", "data": request["data"]})
        return {"ok": True}

    def stop(self):
        self.opened = False


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.agent = MockAgent()
        self.gateway = gateway.Gateway(self.agent, idle_timeout=45)
        self.server = gateway.Server(("127.0.0.1", 0), self.gateway)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.client = secrets.token_hex(32)
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.gateway.stop()
        self.thread.join(timeout=2)

    def request(self, path, body=None, headers=None, method="POST"):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        fields = {"Authorization": "Bearer " + self.gateway.token, "X-Tio-Client": self.client,
                  "Content-Type": "application/json", "Origin": self.base}
        fields.update(headers or {})
        payload = json.dumps(body if body is not None else {}).encode()
        connection.request(method, path, body=payload if method == "POST" else None, headers=fields)
        response = connection.getresponse()
        raw = response.read()
        result = json.loads(raw) if response.getheader("Content-Type", "").startswith("application/json") else raw
        status, response_headers = response.status, dict(response.getheaders())
        connection.close()
        return status, result, response_headers

    def claim(self):
        self.assertEqual(self.request("/api/claim")[0], 200)

    def test_static_and_no_token_disclosure(self):
        status, body, headers = self.request("/", method="GET")
        self.assertEqual(status, 200)
        self.assertNotIn(self.gateway.token.encode(), body)
        self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
        self.assertEqual(headers["Cache-Control"], "no-store")
        for path in ("/../LICENSE", "/?token=secret", "/api/devices"):
            self.assertEqual(self.request(path, method="GET")[0], 404)

    def test_auth_origin_and_client(self):
        for auth in ("", "Bearer wrong", "Bearer café"):
            self.assertEqual(self.request("/api/claim", headers={"Authorization": auth})[0], 401)
        self.assertEqual(self.request("/api/claim", headers={"Origin": "https://evil.example"})[0], 403)
        self.assertEqual(self.request("/api/claim", headers={"Origin": "null"})[0], 403)
        self.assertEqual(self.request("/api/claim", headers={"Sec-Fetch-Site": "cross-site"})[0], 403)
        self.assertEqual(self.request("/api/claim", headers={"X-Tio-Client": "short"})[0], 400)
        self.assertEqual(self.agent.calls, [])

    def test_single_owner_and_release(self):
        self.claim()
        other = {"X-Tio-Client": secrets.token_hex(32)}
        self.assertEqual(self.request("/api/claim", headers=other)[0], 409)
        self.assertEqual(self.request("/api/request", {"op": "open", "device": "/mock/serial"}, other)[0], 409)
        self.assertEqual(self.request("/api/request", {"op": "open", "device": "/mock/serial"})[0], 200)
        self.assertTrue(self.agent.opened)
        self.assertEqual(self.request("/api/release")[0], 200)
        self.assertFalse(self.agent.opened)
        self.assertEqual(self.request("/api/claim", headers=other)[0], 200)

    def test_binary_events_acknowledge_without_loss(self):
        self.claim()
        encoded = base64.b64encode(bytes(range(256))).decode()
        self.request("/api/request", {"op": "send", "data": encoded})
        status, first, _ = self.request("/api/events", {"after": 0})
        self.assertEqual(status, 200)
        self.assertEqual([e["event"] for e in first["events"]], ["tx", "rx"])
        self.assertEqual(first["events"][1]["data"], encoded)
        self.assertEqual(self.request("/api/events", {"after": 0})[1], first)
        self.agent.callback({"event": "status", "connected": False, "message": "gone"})
        second = self.request("/api/events", {"after": first["events"][-1]["seq"]})[1]
        self.assertEqual([e["event"] for e in second["events"]], ["status"])
        self.assertEqual(self.request("/api/events", {"after": 999})[0], 400)
        self.assertEqual(self.request("/api/events", {"after": True})[0], 400)

    def test_manual_reclaim_starts_a_fresh_session(self):
        self.claim()
        self.request("/api/request", {"op": "open", "device": "/mock/serial"})
        first = self.request("/api/events", {"after": 0})[1]
        self.agent.callback({"event": "rx", "data": "AA=="})
        self.request("/api/events", {"after": first["events"][-1]["seq"]})
        self.claim()
        self.assertFalse(self.agent.opened)
        self.assertEqual(self.gateway.acknowledged, 0)
        self.assertEqual(list(self.gateway.events), [])

    def test_body_commands_and_rate_limits(self):
        self.claim()
        for body in ({"op": "shell", "command": "touch /tmp/should-not-run"},
                     {"op": "devices", "command": "anything"}, {"op": ["devices"]}):
            self.assertEqual(self.request("/api/request", body)[0], 400)
        self.assertEqual(self.request("/api/request", {"op": "send", "data": "x" * gateway.BODY_LIMIT})[0], 413)
        self.assertEqual(self.request("/api/request", [], {"Content-Type": "text/plain"})[0], 415)
        self.assertEqual(self.request("/api/request", [1])[0], 400)
        with self.server.rate_lock:
            self.server.rate.extend([time.monotonic()] * 600)
        self.assertEqual(self.request("/api/request", {"op": "devices"})[0], 429)

    def test_overflow_is_explicit_and_closes_session(self):
        self.claim()
        self.request("/api/request", {"op": "open", "device": "/mock/serial"})
        self.gateway.event_limit = 128
        self.agent.callback({"event": "rx", "data": "A" * 1024})
        result = self.request("/api/events", {"after": 0})[1]
        self.assertIn("data was lost", result["fault"])
        self.assertEqual(self.request("/api/request", {"op": "send", "data": "AA=="})[0], 409)
        deadline = time.monotonic() + 2
        while self.agent.opened and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertFalse(self.agent.opened)
        self.assertIsNotNone(self.gateway.fault)

    def test_abandoned_browser_expires_and_releases_port(self):
        self.claim()
        self.request("/api/request", {"op": "open", "device": "/mock/serial"})
        with self.gateway.condition:
            self.gateway.last_seen = time.monotonic() - 60
        deadline = time.monotonic() + 2
        while self.gateway.owner and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertIsNone(self.gateway.owner)
        self.assertFalse(self.agent.opened)
        self.assertEqual(self.request("/api/request", {"op": "send", "data": "AA=="})[0], 409)

    def test_second_event_poll_is_rejected(self):
        self.claim()
        result = []
        reader = threading.Thread(target=lambda: result.append(self.request("/api/events", {"after": 0})))
        reader.start()
        deadline = time.monotonic() + 1
        while not self.gateway.polling and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(self.request("/api/events", {"after": 0})[0], 409)
        self.agent.callback({"event": "done"})
        reader.join(timeout=2)
        self.assertEqual(result[0][0], 200)

    def test_reclaim_cancels_old_poll_without_affecting_new_lease(self):
        self.claim()
        result = []
        reader = threading.Thread(target=lambda: result.append(self.request("/api/events", {"after": 0})))
        reader.start()
        deadline = time.monotonic() + 1
        while not self.gateway.polling and time.monotonic() < deadline:
            time.sleep(.01)
        self.claim()
        reader.join(timeout=2)
        self.assertEqual(result[0][0], 409)
        self.agent.callback({"event": "rx", "data": "AA=="})
        self.assertEqual(self.request("/api/events", {"after": 0})[0], 200)

    def test_cli_refuses_remote_plaintext_before_starting_agent(self):
        result = subprocess.run([sys.executable, str(ROOT / "remote/gateway.py"), "--agent", "/missing",
                                 "--host", "0.0.0.0"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires --cert and --key", result.stderr)


class AgentProtocolTests(unittest.TestCase):
    @unittest.skipIf(sys.platform == "win32", "Executable script fixture uses a POSIX shebang")
    def test_process_pipe_framing_and_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "mock-agent"
            path.write_text(f"#!{sys.executable}\n" +
                            "import sys,json\nfor line in sys.stdin:\n r=json.loads(line)\n print(json.dumps({'event':'status','connected':False,'message':'mock'}),flush=True)\n print(json.dumps({'id':r['id'],'ok':True,'devices':['mock']}),flush=True)\n")
            path.chmod(0o700)
            agent = gateway.Agent(path)
            events = []
            agent.callback = events.append
            try:
                self.assertEqual(agent.call({"op": "devices"})["devices"], ["mock"])
                self.assertEqual(events[0]["event"], "status")
            finally:
                agent.stop()
            self.assertIsNotNone(agent.process.poll())

    @unittest.skipUnless(shutil.which("openssl"), "TLS regression needs openssl to generate a temporary test certificate")
    def test_silent_tls_peer_does_not_block_accept(self):
        with tempfile.TemporaryDirectory() as directory:
            cert, key = Path(directory) / "cert.pem", Path(directory) / "key.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                            "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost"],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            bridge = gateway.Gateway(MockAgent())
            server = gateway.Server(("127.0.0.1", 0), bridge, tls=True)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            server.socket = context.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            silent = socket.create_connection(("127.0.0.1", server.server_port), timeout=2)
            try:
                # Trust only this test certificate; production clients must also verify TLS.
                client_context = ssl.create_default_context(cafile=str(cert))
                connection = http.client.HTTPSConnection("localhost", server.server_port, timeout=2, context=client_context)
                connection.request("GET", "/")
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                response.read()
                connection.close()
            finally:
                silent.close()
                server.shutdown()
                server.server_close()
                bridge.stop()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
