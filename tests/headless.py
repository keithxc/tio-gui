#!/usr/bin/env python3
"""Run GUI acceptance on a private X server, independent of desktop focus."""
import os
import select
import subprocess
import sys

server = subprocess.Popen(
    ["Xvfb", "-displayfd", "1", "-screen", "0", "1280x900x24", "-nolisten", "tcp"],
    stdout=subprocess.PIPE, text=True,
)
try:
    if not select.select([server.stdout], [], [], 10)[0]:
        raise RuntimeError("Private X server did not start")
    display = server.stdout.readline().strip()
    if not display.isdigit():
        raise RuntimeError("Private X server returned no display number")
    env = {**os.environ, "DISPLAY": ":" + display, "GDK_BACKEND": "x11",
           "GTK_A11Y": "none", "GTK_IM_MODULE": "simple", "GSK_RENDERER": "cairo"}
    sys.exit(subprocess.run(sys.argv[1:], env=env).returncode)
finally:
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
