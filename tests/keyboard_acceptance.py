#!/usr/bin/env python3
"""Drive real GTK key events on the private display and verify VTE PTY bytes."""
import os
import select
import subprocess
import sys
import time

if os.environ.get("GDK_BACKEND") != "x11":
    raise SystemExit("Run through tests/headless.py to isolate keyboard focus")
app = subprocess.Popen([sys.argv[1]], stdout=subprocess.PIPE)
try:
    window = subprocess.check_output(
        ["xdotool", "search", "--sync", "--onlyvisible", "--name", "^tio-gui isolated keyboard test$"],
        text=True, timeout=8,
    ).splitlines()[0]
    subprocess.run(["xdotool", "windowfocus", "--sync", window], check=True, timeout=5)
    time.sleep(.2)
    subprocess.run(["xdotool", "type", "--clearmodifiers", "--delay", "30", "ABC"], check=True)
    subprocess.run(["xdotool", "key", "--clearmodifiers", "--delay", "50",
                    "Return", "Tab", "Escape", "Up", "ctrl+c"], check=True)
    expected = b"ABC\r\t\x1b\x1b[A\x03"
    actual = bytearray()
    pending = b""
    deadline = time.monotonic() + 5
    while len(actual) < len(expected) and time.monotonic() < deadline:
        if select.select([app.stdout], [], [], .1)[0]:
            pending += os.read(app.stdout.fileno(), 4096)
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                if line.startswith(b"RX:"):
                    actual.extend(bytes.fromhex(line[3:].decode()))
    assert actual == expected, (actual.hex(), expected.hex())
    print("PASS real GTK keyboard → hidden VTE → PTY: text, Enter, Tab, Escape, arrow, Ctrl-C")
finally:
    app.terminate()
    try:
        app.wait(timeout=5)
    except subprocess.TimeoutExpired:
        app.kill()
        app.wait()
