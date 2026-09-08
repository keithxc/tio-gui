#!/usr/bin/env python3
"""Unplug/replug an isolated PTY and verify the GUI's actual connection observer."""
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
with tempfile.TemporaryDirectory(prefix="tio-reconnect-test-") as root:
    path = Path(root) / "serial"
    path.symlink_to(os.ttyname(slave))
    process = subprocess.Popen(["tio", str(path)], stdin=terminal, stdout=terminal,
                               stderr=terminal, env={**os.environ, "XDG_CONFIG_HOME": root})
    helper = None
    try:
        helper = subprocess.Popen([str(build / "quick-editor-test"), "-p", "/connection/real-reconnect"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env={**os.environ, "TIO_TEST_TIO_PID": str(process.pid), "TIO_TEST_TIO_DEVICE": str(path)})
        def wait_stage(stage):
            output = b""
            until = time.monotonic() + 10
            while f"WATCH_{stage}".encode() not in output:
                assert time.monotonic() < until, repr(output)
                ready, _, _ = select.select([helper.stdout, control], [], [], .1)
                if control in ready:
                    os.read(control, 8192)
                if helper.stdout in ready:
                    block = os.read(helper.stdout.fileno(), 8192)
                    assert block, repr(output)
                    output += block
            print(output.decode(), end="")
        wait_stage(0)
        path.unlink()
        os.close(serial); os.close(slave)
        serial = slave = -1
        wait_stage(1)
        serial, slave = pty.openpty()
        tty.setraw(slave)
        path.symlink_to(os.ttyname(slave))
        wait_stage(2)
        assert helper.wait(timeout=3) == 0
        print("PASS: real tio unplug/replug, GUI input gating, disconnect reason, reconnect count = 1")
    finally:
        if helper and helper.poll() is None:
            helper.kill(); helper.wait()
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
        for fd in (serial, slave, control, terminal):
            if fd >= 0: os.close(fd)
