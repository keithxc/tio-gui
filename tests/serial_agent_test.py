#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the real headless protocol; POSIX adds byte-exact PTY transport."""
import base64
from contextlib import contextmanager
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import unittest

AGENT = str(Path(sys.argv.pop(1)).resolve())


@contextmanager
def peer_agent():
    """A separate process for ownership checks, with bounded event waits."""
    proc = subprocess.Popen([AGENT], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    messages = queue.Queue()
    def read():
        for line in proc.stdout:
            messages.put(json.loads(line))
    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    try:
        yield proc, messages
    finally:
        proc.stdin.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            raise AssertionError("Peer agent did not exit on stdin EOF")
        finally:
            reader.join(timeout=2)
            diagnostics = proc.stderr.read().decode(errors="replace")
            proc.stdout.close()
            proc.stderr.close()
        if proc.returncode != 0 or "CRITICAL" in diagnostics:
            raise AssertionError(f"Peer agent exited {proc.returncode}: {diagnostics}")


class AgentTests(unittest.TestCase):
    def setUp(self):
        self.proc = subprocess.Popen([AGENT], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.messages = queue.Queue()
        self.events = []
        self.counter = 0
        def read():
            for line in self.proc.stdout:
                self.messages.put(json.loads(line))
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def tearDown(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            self.fail("Agent did not release its serial session on stdin EOF")
        self.reader.join(timeout=2)
        diagnostics = self.proc.stderr.read().decode(errors="replace")
        self.proc.stdout.close()
        self.proc.stderr.close()
        self.assertEqual(self.proc.returncode, 0, diagnostics)
        self.assertNotIn("CRITICAL", diagnostics)

    def request(self, op, **fields):
        self.counter += 1
        rid = str(self.counter)
        message = dict(id=rid, op=op, **fields)
        self.proc.stdin.write(json.dumps(message).encode() + b"\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            item = self.messages.get(timeout=5)
            if item.get("id") == rid:
                return item
            self.events.append(item)
        self.fail("No agent response")

    def event(self, kind):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            for i, e in enumerate(self.events):
                if e.get("event") == kind:
                    return self.events.pop(i)
            self.events.append(self.messages.get(timeout=5))
        self.fail(f"No {kind} event")

    def test_devices_and_bad_operations(self):
        devices = self.request("devices")
        self.assertTrue(devices["ok"])
        self.assertIsInstance(devices["devices"], list)
        self.assertFalse(self.request("shell", command="anything")["ok"])
        self.assertFalse(self.request("send", data="YWJj")["ok"])
        self.assertTrue(self.request("close")["ok"])

    def test_types_and_limits(self):
        for overrides in [{"device": []}, {"baud": -1}, {"baud": True},
                          {"baud": 115200.5}, {"bits": 9}, {"stops": 0},
                          {"flow": 3}, {"parity": 8}, {"reconnect": "yes"},
                          {"device": "x" * 4097}]:
            fields = {"device": "COM9999"}
            fields.update(overrides)
            with self.subTest(overrides=overrides):
                self.assertFalse(self.request("open", **fields)["ok"])
        for raw in [b"null\n", b"[]\n", b'{"id":1,"op":"devices"}\n',
                    b'{"id":"a","op":null}\n', b'{"id":"a","op":"devices\\u0000other"}\n',
                    b'{"id":"a\\u0000b","op":"devices"}\n',
                    b'{"id":"a","op":"devices"}\x00ignored\n',
                    b"x" * (128 * 1024 + 1) + b"\n"]:
            self.proc.stdin.write(raw)
            self.proc.stdin.flush()
            self.assertFalse(self.messages.get(timeout=5)["ok"])
        self.assertTrue(self.request("devices")["ok"])

    def test_absent_device_reports_status(self):
        path = "COM9999" if os.name == "nt" else "/nonexistent-tio-agent-test"
        self.assertTrue(self.request("open", device=path)["ok"])
        self.assertFalse(self.event("status")["connected"])
        self.event("done")
        self.assertTrue(self.request("close")["ok"])

    @unittest.skipIf(os.name == "nt", "PTY available on POSIX; COM hardware is a separate gate")
    def test_cross_process_device_ownership(self):
        import pty
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        device = os.ttyname(slave)
        self.assertTrue(self.request("open", device=device)["ok"])
        self.assertTrue(self.event("status")["connected"])
        with peer_agent() as (peer, messages):
            peer.stdin.write(json.dumps(dict(id="peer", op="open", device=device)).encode() + b"\n")
            peer.stdin.flush()
            events = []
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                events.append(messages.get(timeout=max(0.01, deadline - time.monotonic())))
                if events[-1].get("event") == "done":
                    break
            self.assertTrue(any(item.get("event") == "done" for item in events), events)
            statuses = [item for item in events if item.get("event") == "status"]
            self.assertTrue(statuses, events)
            self.assertFalse(any(item["connected"] for item in statuses), events)

    @unittest.skipIf(os.name == "nt", "PTY available on POSIX; COM hardware is a separate gate")
    def test_stdin_eof_releases_device(self):
        import pty
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        device = os.ttyname(slave)
        self.assertTrue(self.request("open", device=device)["ok"])
        self.assertTrue(self.event("status")["connected"])
        self.proc.stdin.close()
        self.assertEqual(self.proc.wait(timeout=5), 0)
        with peer_agent() as (peer, messages):
            peer.stdin.write(json.dumps(dict(id="peer", op="open", device=device)).encode() + b"\n")
            peer.stdin.flush()
            while True:
                item = messages.get(timeout=5)
                if item.get("event") == "status":
                    self.assertTrue(item["connected"], item)
                    break

    def test_stdin_eof_stops_missing_device_reconnect(self):
        device = "COM9999999" if os.name == "nt" else "/nonexistent-tio-agent-test"
        self.assertTrue(self.request("open", device=device, reconnect=True)["ok"])
        self.assertFalse(self.event("status")["connected"])
        self.proc.stdin.close()
        self.assertEqual(self.proc.wait(timeout=5), 0)

    @unittest.skipIf(os.name == "nt", "PTY available on POSIX; COM hardware is a separate gate")
    def test_binary_roundtrip_ownership_and_close(self):
        import pty
        import select
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        self.assertTrue(self.request("open", device=os.ttyname(slave))["ok"])
        self.assertTrue(self.event("status")["connected"])
        self.assertFalse(self.request("open", device=os.ttyname(slave))["ok"])
        for bad in ["?", "YQ", "YQ==\n", "", "YQ===", "YR=="]:
            self.assertFalse(self.request("send", data=bad)["ok"])
        self.assertFalse(self.request("line", line=-1)["ok"])
        self.assertFalse(self.request("line")["ok"])
        self.assertFalse(self.request("line", line=0, high="true")["ok"])
        data = bytes(range(256)) * 4
        os.write(master, data)
        received = b""
        while len(received) < len(data):
            received += base64.b64decode(self.event("rx")["data"], validate=True)
        self.assertEqual(received, data)
        self.assertTrue(self.request("send", data=base64.b64encode(data).decode())["ok"])
        received = b""
        while len(received) < len(data):
            ready, _, _ = select.select([master], [], [], 3)
            self.assertTrue(ready, "PTY did not receive transmitted bytes")
            received += os.read(master, 4096)
        self.assertEqual(received, data)
        transmitted = b""
        while len(transmitted) < len(data):
            transmitted += base64.b64decode(self.event("tx")["data"])
        self.assertEqual(transmitted, data)
        self.assertTrue(self.request("close")["ok"])
        self.event("done")
        self.assertFalse(self.request("send", data="YQ==")["ok"])
        # Closing releases the lock and allows another session in this process.
        self.assertTrue(self.request("open", device=os.ttyname(slave))["ok"])
        while not self.event("status")["connected"]:
            pass
        self.assertTrue(self.request("close")["ok"])


if __name__ == "__main__":
    unittest.main()
