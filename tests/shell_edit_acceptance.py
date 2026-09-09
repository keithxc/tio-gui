#!/usr/bin/env python3
"""Replay a real Bash/Readline PTY stream through the highlight renderer."""
import os
from pathlib import Path
import pty
import select
import signal
import sys
import tempfile
import time
import shutil
import subprocess

binary = str(Path(sys.argv[1]).resolve())
shell = sys.argv[2] if len(sys.argv) > 2 else shutil.which('bash')
if not shell:
    raise SystemExit('A Readline-enabled Bash is required (optional second argument)')
with tempfile.TemporaryDirectory(prefix='tio-shell-edit-') as temporary:
    pid, master = pty.fork()
    if pid == 0:
        os.execve(shell, [shell, '--noprofile', '--norc', '-i'], {
            'PATH': os.environ['PATH'], 'TERM': 'xterm-256color',
            'HOME': temporary, 'INPUTRC': '/dev/null', 'LC_ALL': 'C', 'PS1': 'test> ',
        })
    stream = bytearray()
    def drain():
        received = bytearray()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and select.select([master], [], [], .2)[0]:
            received.extend(os.read(master, 65536))
        stream.extend(received)
        return bytes(received)
    try:
        # Startup can need longer than a single inter-byte quiet period.
        deadline = time.monotonic() + 5
        while b'test> ' not in stream and time.monotonic() < deadline:
            drain()
        assert b'test> ' in stream, bytes(stream)
        steps = [
            (b'abcdef', 'test> abcdef', 12),
            (b'\x7f', 'test> abcde', 11),
            (b'\x1b[D\x1b[D', 'test> abcde', 9),
            (b'\x7f', 'test> abde', 8),
            (b'\x1b[3~', 'test> abe', 8),
            (b'Z', 'test> abZe', 9),
            (b'\x15', 'test> e', 6),
            (b'\x0b', 'test>', 6),
        ]
        for keys, expected, cursor in steps:
            os.write(master, keys)
            received = drain()
            print(f'keys={keys!r} response={received!r}', flush=True)
            capture = Path(temporary) / 'capture.bin'
            capture.write_bytes(stream)
            subprocess.run([binary, '-p', '/highlighter/real-shell-editing'], check=True, env={
                **os.environ, 'TIO_TEST_SHELL_CAPTURE': str(capture),
                'TIO_TEST_SHELL_EXPECTED': expected, 'TIO_TEST_SHELL_CURSOR': str(cursor),
            })
    finally:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        os.close(master)
print('Real Bash end/middle Backspace, Delete, insertion, Ctrl-U and Ctrl-K passed.')
