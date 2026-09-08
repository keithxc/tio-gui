#!/usr/bin/env python3
"""Check tio raw socket TX/RX against an isolated PTY, including every byte."""
import os
import pty
import select
import socket
import subprocess
import sys
import tempfile
import time
import tty


def read_exact(fd, length, timeout=5):
    result = bytearray()
    until = time.monotonic() + timeout
    while len(result) < length:
        left = until - time.monotonic()
        assert left > 0, f"Timed out: {len(result)}/{length} bytes"
        ready, _, _ = select.select([fd], [], [], left)
        assert ready, f"Timed out: {len(result)}/{length} bytes"
        block = os.read(fd, length - len(result))
        assert block, "Unexpected EOF"
        result.extend(block)
    return bytes(result)


def main():
    serial, slave = pty.openpty()
    control, terminal = pty.openpty()
    tty.setraw(slave)
    with tempfile.TemporaryDirectory(prefix="tio-gui-test-") as root:
        path = root + "/serial.sock"
        process = subprocess.Popen(
            ["tio", "--no-reconnect", "--socket", "unix:" + path, os.ttyname(slave)],
            stdin=terminal, stdout=terminal, stderr=terminal,
            env={**os.environ, "XDG_CONFIG_HOME": root},
        )
        try:
            until = time.monotonic() + 5
            while not os.path.exists(path):
                assert process.poll() is None, "tio exited before creating socket"
                assert time.monotonic() < until, "Socket did not appear"
                time.sleep(0.02)
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(5)
                client.connect(path)
                # Wait for tio's device-open confirmation before sending.
                output = b""
                while b"Connected" not in output:
                    assert time.monotonic() < until, repr(output)
                    if select.select([control], [], [], .1)[0]:
                        output += os.read(control, 8192)
                script = root + "/line-control-test.lua"
                with open(script, "w") as handle:
                    handle.write('print("TIO_GUI_SCRIPT_ACCEPTED")\n')
                os.write(control, b"\x14r")
                output = b""
                until = time.monotonic() + 5
                while b"Enter file name:" not in output:
                    assert time.monotonic() < until, repr(output)
                    if select.select([control], [], [], .1)[0]:
                        output += os.read(control, 8192)
                os.write(control, script.encode() + b"\r")
                output = b""
                while b"TIO_GUI_SCRIPT_ACCEPTED" not in output:
                    assert time.monotonic() < until, repr(output)
                    if select.select([control], [], [], .1)[0]:
                        output += os.read(control, 8192)
                assert not select.select([serial], [], [], .05)[0], "Command prompt leaked onto serial"
                print("PASS: tio runtime script prompt accepts request/response handshake without serial leakage")
                payload = bytes(range(256))
                client.sendall(payload)
                actual = read_exact(serial, len(payload))
                assert actual == payload, (actual.hex(), payload.hex())
                os.write(serial, payload)
                actual = read_exact(client.fileno(), len(payload))
                assert actual == payload, (actual.hex(), payload.hex())
                print("PASS: all 256 byte values preserved in both directions through tio socket")
                if len(sys.argv) > 1:
                    helper = subprocess.Popen(
                        [sys.argv[1], "-p", "/sequences/real-tio"],
                        env={**os.environ, "TIO_TEST_SOCKET": path},
                    )
                    try:
                        first = read_exact(serial, 3)
                        started = time.monotonic()
                        second = read_exact(serial, 8)
                        elapsed = time.monotonic() - started
                        assert first == bytes.fromhex("00 14 FF"), first.hex()
                        assert second == bytes.fromhex("01 03 00 00 00 0A C5 CD"), second.hex()
                        assert elapsed >= 0.08, f"Delay too short: {elapsed}"
                        assert helper.wait(timeout=5) == 0
                        print(f"PASS: GUI sequence runner → tio → PTY, binary + CRC, delay {elapsed:.3f}s")
                    finally:
                        if helper.poll() is None:
                            helper.kill()
                            helper.wait()

        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            for fd in (serial, slave, control, terminal):
                os.close(fd)


if __name__ == "__main__":
    main()
