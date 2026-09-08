#!/usr/bin/env python3
"""Exercise real tio and GUI controls with a test-only virtual ioctl adapter."""
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import time
import tty

build = Path(sys.argv[1]).resolve()
serial, slave = pty.openpty()
control, terminal = pty.openpty()
tty.setraw(slave)
with tempfile.TemporaryDirectory(prefix="tio-lines-test-") as root:
    log = Path(root) / "ioctl.log"
    process = subprocess.Popen([
        "tio", "--no-reconnect", "--rs-485", "--rs-485-config",
        "RTS_ON_SEND=1,RTS_AFTER_SEND=0,RTS_DELAY_BEFORE_SEND=3,RTS_DELAY_AFTER_SEND=7,RX_DURING_TX",
        "--line-pulse-duration", "DTR=20,RTS=20", os.ttyname(slave),
    ], stdin=terminal, stdout=terminal, stderr=terminal, env={
        **os.environ, "XDG_CONFIG_HOME": root,
        "LD_PRELOAD": str(build / "libserial-ioctl-shim.so"), "TIO_TEST_IOCTL_LOG": str(log),
    })
    try:
        output = b""
        until = time.monotonic() + 5
        while b"Connected" not in output:
            assert process.poll() is None, repr(output)
            assert time.monotonic() < until, repr(output)
            if select.select([control], [], [], .1)[0]:
                output += os.read(control, 8192)
        subprocess.run([str(build / "quick-editor-test"), "-p", "/serial-lines/real-tio"],
            env={**os.environ, "TIO_TEST_CONTROL_FD": str(control)}, pass_fds=(control,),
            check=True, timeout=15)
        rows = [tuple(map(int, line.split())) for line in log.read_text().splitlines()]
        states = [row[1] for row in rows if row[0] == 0x5418]  # TIOCMSET
        # tio describes electrical levels: HIGH clears the active-low modem bit.
        assert states == [0, 2, 2, 6, 4, 6, 2, 6], states
        assert any(row[2:] == (0x13, 3, 7) for row in rows), rows  # enabled, RTS, RX_DURING_TX
        assert not select.select([serial], [], [], .1)[0], "Control commands leaked to serial data"
        print("PASS: GUI → real tio → virtual driver: DTR/RTS high/low/pulse, RS-485 flags/delays; no data leakage")
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        for fd in (serial, slave, control, terminal):
            os.close(fd)
