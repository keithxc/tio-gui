#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Explicitly started, single-controller HTTP(S) bridge to tio-serial-agent."""
import argparse
import collections
import hmac
import ipaddress
import json
from pathlib import Path
import queue
import re
import secrets
import socket
import ssl
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BODY_LIMIT = 96 * 1024
EVENT_LIMIT = 4 * 1024 * 1024
EVENT_COUNT = 8192
AGENT_LINE_LIMIT = 128 * 1024
STATIC = Path(__file__).resolve().parent


class Failure(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


class Agent:
    """A bounded writer and reader keep a stalled subprocess off HTTP threads."""
    def __init__(self, executable):
        self.callback = lambda event: None
        self.process = subprocess.Popen([str(Path(executable).resolve())],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.condition = threading.Condition()
        self.call_lock = threading.Lock()
        self.writes = queue.Queue(maxsize=2)
        self.pending = None
        self.reply = None
        self.dead = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.writer = threading.Thread(target=self._write, daemon=True)
        self.reader.start()
        self.writer.start()

    def _failed(self):
        with self.condition:
            first = not self.dead
            self.dead = True
            self.condition.notify_all()
        if first:
            self.callback({"event": "gateway-error", "message": "Serial agent stopped; session closed"})

    def _read(self):
        try:
            while True:
                line = self.process.stdout.readline(AGENT_LINE_LIMIT + 1)
                if not line or len(line) > AGENT_LINE_LIMIT or not line.endswith(b"\n"):
                    break
                value = json.loads(line)
                if not isinstance(value, dict):
                    break
                if "event" in value:
                    self.callback(value)
                else:
                    with self.condition:
                        if value.get("id") == self.pending:
                            self.reply = value
                            self.condition.notify_all()
        except (OSError, ValueError):
            pass
        finally:
            self._failed()
            if self.process.poll() is None:
                self.process.terminate()

    def _write(self):
        try:
            while not self.dead:
                try:
                    data = self.writes.get(timeout=0.5)
                except queue.Empty:
                    continue
                view = memoryview(data)
                while view:
                    count = self.process.stdin.write(view)
                    if not count:
                        raise OSError("Agent pipe closed")
                    view = view[count:]
                self.process.stdin.flush()
        except (OSError, ValueError):
            self._failed()

    def call(self, request, timeout=10):
        if not self.call_lock.acquire(timeout=timeout):
            raise Failure(503, "Serial agent busy")
        try:
            with self.condition:
                if self.dead:
                    raise Failure(503, "Serial agent unavailable; restart the gateway")
                self.pending = secrets.token_hex(16)
                self.reply = None
                data = dict(request, id=self.pending)
                self.writes.put_nowait(json.dumps(data, separators=(",", ":")).encode() + b"\n")
                self.condition.wait_for(lambda: self.reply is not None or self.dead, timeout)
                reply = self.reply
                self.pending = None
            if reply is None:
                self.stop()
                raise Failure(503, "Serial agent did not respond; session closed")
            reply.pop("id", None)
            return reply
        finally:
            self.call_lock.release()

    def stop(self):
        self._failed()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        for worker in (self.reader, self.writer):
            if worker is not threading.current_thread():
                worker.join(timeout=1)
        for stream in (self.process.stdin, self.process.stdout):
            try:
                stream.close()
            except OSError:
                pass


class Gateway:
    def __init__(self, agent, token=None, idle_timeout=45, event_limit=EVENT_LIMIT):
        self.agent = agent
        self.token = token or secrets.token_urlsafe(32)
        self.idle_timeout = idle_timeout
        self.event_limit = event_limit
        self.condition = threading.Condition()
        self.operation = threading.Lock()
        self.owner = None
        self.last_seen = 0
        self.events = collections.deque()
        self.event_bytes = 0
        self.sequence = self.acknowledged = self.delivered = 0
        self.polling = False
        self.lease = 0
        self.fault = None
        self.needs_close = False
        self.stopping = threading.Event()
        self.agent.callback = self.event
        self.supervisor = threading.Thread(target=self._supervise, daemon=True)
        self.supervisor.start()

    def event(self, value):
        with self.condition:
            if not self.owner or self.fault:
                return
            size = len(json.dumps(value).encode())
            if value.get("event") == "gateway-error":
                self.fault = value.get("message", "Serial agent failed")
            elif self.event_bytes + size > self.event_limit or len(self.events) >= EVENT_COUNT:
                self.fault = "Receive buffer overflow: data was lost and the serial session was closed. Release control before reconnecting."
                self.needs_close = True
            else:
                self.sequence += 1
                self.events.append((self.sequence, value, size))
                self.event_bytes += size
            self.condition.notify_all()

    def _check_owner(self, client):
        if self.owner != client:
            raise Failure(409, "Another browser owns control, or your control lease expired")
        self.last_seen = time.monotonic()

    def _reset(self):
        self.lease += 1
        self.polling = False
        self.owner = None
        self.events.clear()
        self.event_bytes = self.sequence = self.acknowledged = self.delivered = 0
        self.fault = None
        self.needs_close = False
        self.condition.notify_all()

    def perform(self, client, path, body):
        if path == "/api/events":
            return self._poll(client, body)
        if not self.operation.acquire(timeout=1):
            raise Failure(429, "Another serial operation is still running")
        try:
            if path == "/api/claim":
                if body:
                    raise Failure(400, "Claim does not accept options")
                with self.condition:
                    if self.owner and self.owner != client:
                        raise Failure(409, "Another browser already owns this gateway")
                    previous = self.owner is not None
                if previous:
                    # A manual re-entry starts fresh; it never resumes unknown queued writes.
                    self.agent.call({"op": "close"})
                with self.condition:
                    self._reset()
                    self.owner = client
                    self.last_seen = time.monotonic()
                    return {"ok": True, "idleTimeout": self.idle_timeout}
            with self.condition:
                self._check_owner(client)
            if path == "/api/release":
                if body:
                    raise Failure(400, "Release does not accept options")
                try:
                    result = self.agent.call({"op": "close"})
                finally:
                    with self.condition:
                        self._reset()
                return result
            if path != "/api/request":
                raise Failure(404, "Unknown endpoint")
            allowed = {
                "devices": {"op"}, "close": {"op"}, "send": {"op", "data"},
                "line": {"op", "line", "high"},
                "open": {"op", "device", "baud", "bits", "stops", "parity", "flow", "reconnect"},
            }
            op = body.get("op")
            if not isinstance(op, str) or op not in allowed or set(body) - allowed[op]:
                raise Failure(400, "Unknown operation or fields")
            with self.condition:
                if self.fault and op not in ("close", "devices"):
                    raise Failure(409, self.fault)
            return self.agent.call(body)
        finally:
            self.operation.release()

    def _poll(self, client, body):
        after = body.get("after", 0)
        if set(body) - {"after"} or type(after) is not int:
            raise Failure(400, "Expected an integer event cursor")
        with self.condition:
            self._check_owner(client)
            if self.polling:
                raise Failure(409, "Only one event poll is allowed")
            if not self.acknowledged <= after <= self.delivered:
                raise Failure(400, "Invalid event cursor")
            self.acknowledged = after
            while self.events and self.events[0][0] <= after:
                self.event_bytes -= self.events.popleft()[2]
            self.polling = True
            lease = self.lease
            try:
                self.condition.wait_for(lambda: self.events or self.fault or self.lease != lease,
                                        timeout=min(10, self.idle_timeout / 3))
                if self.lease != lease:
                    raise Failure(409, "Control lease ended or restarted")
                self._check_owner(client)
                values, size = [], 0
                for seq, value, count in self.events:
                    if len(values) >= 256 or size + count > 256 * 1024:
                        break
                    values.append(dict(value, seq=seq))
                    size += count
                if values:
                    self.delivered = values[-1]["seq"]
                return {"ok": True, "events": values, "fault": self.fault}
            finally:
                if self.lease == lease:
                    self.polling = False

    def _supervise(self):
        while not self.stopping.wait(0.5):
            if not self.operation.acquire(timeout=0.1):
                continue
            try:
                with self.condition:
                    expired = self.owner and time.monotonic() - self.last_seen > self.idle_timeout
                    close = self.needs_close or expired
                    self.needs_close = False
                if close:
                    try:
                        self.agent.call({"op": "close"})
                    except Failure:
                        pass
                    if expired:
                        with self.condition:
                            self._reset()
            finally:
                self.operation.release()

    def stop(self):
        self.stopping.set()
        self.agent.stop()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 16

    def __init__(self, address, gateway, tls=False):
        self.gateway = gateway
        self.tls = tls
        self.slots = threading.BoundedSemaphore(16)
        self.rate_lock = threading.Lock()
        self.rate = collections.deque()
        super().__init__(address, Handler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def allow_request(self):
        with self.rate_lock:
            now = time.monotonic()
            while self.rate and self.rate[0] <= now - 60:
                self.rate.popleft()
            if len(self.rate) >= 600:
                return False
            self.rate.append(now)
            return True


class Handler(BaseHTTPRequestHandler):
    server_version = "tio-gui-remote"
    sys_version = ""

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, format, *args):
        # Never log URLs, headers, tokens, or device data.
        pass

    def _headers(self, status, content_type, size):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(size))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def _json(self, status, value):
        payload = json.dumps(value, separators=(",", ":")).encode()
        self._headers(status, "application/json; charset=utf-8", len(payload))
        self.wfile.write(payload)

    def _same_origin(self):
        hosts = self.headers.get_all("Host", [])
        if len(hosts) != 1 or not re.fullmatch(r"[A-Za-z0-9.\-\[\]:]+", hosts[0]):
            raise Failure(400, "Invalid Host")
        origins = self.headers.get_all("Origin", [])
        expected = ("https" if self.server.tls else "http") + "://" + hosts[0]
        if origins and (len(origins) != 1 or origins[0] != expected):
            raise Failure(403, "Cross-origin requests are not allowed")
        if self.headers.get("Sec-Fetch-Site") not in (None, "same-origin", "none"):
            raise Failure(403, "Cross-site requests are not allowed")

    def do_GET(self):
        try:
            self._same_origin()
            entry = {"/": ("index.html", "text/html; charset=utf-8"),
                     "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                     "/style.css": ("style.css", "text/css; charset=utf-8")}.get(self.path)
            if not entry:
                raise Failure(404, "Not found")
            payload = (STATIC / entry[0]).read_bytes()
            self._headers(200, entry[1], len(payload))
            self.wfile.write(payload)
        except Failure as error:
            self._json(error.status, {"ok": False, "error": str(error)})

    def do_POST(self):
        try:
            self._same_origin()
            if not self.server.allow_request():
                raise Failure(429, "Gateway request limit reached; retry later")
            auth = self.headers.get_all("Authorization", [])
            if len(auth) != 1 or not hmac.compare_digest(auth[0].encode("utf-8"), ("Bearer " + self.server.gateway.token).encode("utf-8")):
                raise Failure(401, "Enter the gateway access token")
            client = self.headers.get("X-Tio-Client", "")
            if not re.fullmatch(r"[a-f0-9]{64}", client):
                raise Failure(400, "Expected a random 256-bit client identifier")
            if self.headers.get("Transfer-Encoding"):
                raise Failure(400, "Chunked requests are not supported")
            lengths = self.headers.get_all("Content-Length", [])
            if len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,10}", lengths[0]):
                raise Failure(411, "Content-Length required")
            length = int(lengths[0])
            if not 0 < length <= BODY_LIMIT:
                raise Failure(413, "Request body exceeds 96 KiB")
            if self.headers.get("Content-Type", "").split(";")[0].strip() != "application/json":
                raise Failure(415, "Expected application/json")
            payload = self.rfile.read(length)
            if len(payload) != length:
                raise Failure(400, "Incomplete request")
            try:
                body = json.loads(payload)
            except (ValueError, UnicodeError, RecursionError):
                raise Failure(400, "Invalid JSON") from None
            if not isinstance(body, dict):
                raise Failure(400, "Expected a JSON object")
            result = self.server.gateway.perform(client, self.path, body)
            self._json(200, result)
        except Failure as error:
            self._json(error.status, {"ok": False, "error": str(error)})
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            # The lease supervisor closes abandoned serial sessions.
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--agent", required=True, help="Path to tio-serial-agent")
    parser.add_argument("--host", default="127.0.0.1", help="Numeric bind address; default: 127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--cert", help="PEM TLS certificate (include intermediate chain)")
    parser.add_argument("--key", help="PEM TLS private key")
    parser.add_argument("--idle-timeout", type=int, default=45, help="Control lease timeout in seconds (15-300)")
    args = parser.parse_args()
    try:
        address = ipaddress.ip_address(args.host)
    except ValueError:
        parser.error("--host must be a numeric IP address")
    if not 0 <= args.port <= 65535 or not 15 <= args.idle_timeout <= 300:
        parser.error("Invalid port or idle timeout")
    if bool(args.cert) != bool(args.key):
        parser.error("Supply both --cert and --key")
    if not address.is_loopback and not args.cert:
        parser.error("Non-loopback access requires --cert and --key")
    if address.version != 4:
        parser.error("This preview currently binds IPv4 addresses only")
    if not Path(args.agent).is_file():
        parser.error("--agent must name an existing executable file")
    context = None
    if args.cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
    gateway = Gateway(Agent(args.agent), idle_timeout=args.idle_timeout)
    try:
        server = Server((args.host, args.port), gateway, tls=bool(context))
        if context:
            server.socket = context.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
        print(f"Remote serial gateway: {'https' if context else 'http'}://{args.host}:{server.server_port}", flush=True)
        print(f"Access token (enter in the page; never append to the URL): {gateway.token}", flush=True)
        print("One browser controls this agent. Ctrl-C closes the gateway and its serial session.", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
    finally:
        gateway.stop()


if __name__ == "__main__":
    main()
