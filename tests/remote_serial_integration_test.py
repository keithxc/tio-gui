#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Real HTTP -> gateway -> agent -> PTY, using synthetic data only."""
import base64
import http.client
import importlib.util
import json
import os
from pathlib import Path
import secrets
import sys
import threading
import time
import unittest

AGENT = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gateway", ROOT / "remote/gateway.py")
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)


@unittest.skipIf(os.name == "nt", "Windows requires a real or virtual COM pair")
class RemoteTransportTests(unittest.TestCase):
    def setUp(self):
        import pty
        self.master, self.slave = pty.openpty()
        self.backend = gateway.Agent(AGENT)
        self.bridge = gateway.Gateway(self.backend)
        self.server = gateway.Server(("127.0.0.1", 0), self.bridge)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.client = secrets.token_hex(32)
        self.cursor = 0
        self.buffer = []

    def tearDown(self):
        self.bridge.stop()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        os.close(self.master)
        os.close(self.slave)

    def post(self, path, body, client=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=12)
        conn.request("POST", path, json.dumps(body), {
            "Authorization": "Bearer " + self.bridge.token,
            "Content-Type": "application/json",
            "X-Tio-Client": client or self.client,
            "Origin": f"http://127.0.0.1:{self.server.server_port}",
        })
        response = conn.getresponse()
        status, result = response.status, json.loads(response.read())
        conn.close()
        return status, result

    def request(self, op, **fields):
        status, result = self.post("/api/request", dict(op=op, **fields))
        self.assertEqual(status, 200, result)
        self.assertTrue(result["ok"], result)
        return result

    def event(self, kind):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            for i, value in enumerate(self.buffer):
                if value["event"] == kind:
                    return self.buffer.pop(i)
            status, result = self.post("/api/events", {"after": self.cursor})
            self.assertEqual(status, 200, result)
            self.assertIsNone(result["fault"], result)
            self.buffer.extend(result["events"])
            if result["events"]:
                self.cursor = result["events"][-1]["seq"]
        self.fail("Timed out waiting for " + kind)

    def test_remote_roundtrip_and_control_isolation(self):
        import select
        self.assertEqual(self.post("/api/claim", {})[0], 200)
        self.request("open", device=os.ttyname(self.slave))
        self.assertTrue(self.event("status")["connected"])
        other = secrets.token_hex(32)
        self.assertEqual(self.post("/api/claim", {}, client=other)[0], 409)
        self.assertEqual(self.post("/api/events", {"after": 0}, client=other)[0], 409)
        self.assertEqual(self.post("/api/request", {"op": "close"}, client=other)[0], 409)
        raw = bytes(range(256)) * 8
        os.write(self.master, raw)
        rx = b""
        while len(rx) < len(raw):
            rx += base64.b64decode(self.event("rx")["data"], validate=True)
        self.assertEqual(rx, raw)
        self.request("send", data=base64.b64encode(raw).decode())
        tx = b""
        while len(tx) < len(raw):
            self.assertTrue(select.select([self.master], [], [], 4)[0])
            tx += os.read(self.master, 4096)
        self.assertEqual(tx, raw)
        confirmed = b""
        while len(confirmed) < len(raw):
            confirmed += base64.b64decode(self.event("tx")["data"], validate=True)
        self.assertEqual(confirmed, raw)
        self.assertEqual(self.post("/api/release", {})[0], 200)
        self.assertEqual(self.post("/api/events", {"after": self.cursor})[0], 409)
        self.assertEqual(self.post("/api/claim", {}, client=other)[0], 200)
        # A new owner sees a new sequence, never the previous controller's log.
        self.client, self.cursor, self.buffer = other, 0, []
        self.request("open", device=os.ttyname(self.slave))
        status = self.event("status")
        self.assertTrue(status["connected"])
        self.assertEqual(status["seq"], 1)


if __name__ == "__main__":
    unittest.main()
