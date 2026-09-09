#!/usr/bin/env python3
"""Real Ctrl+wheel into the GTK console, on tests/headless.py's private display."""
import os
import subprocess
import sys
import threading

process = subprocess.Popen(
    [sys.argv[1], '-p', '/console/search-zoom'],
    env={**os.environ, 'TIO_TEST_ZOOM': '1'},
    stdout=subprocess.PIPE, text=True,
)
watchdog = threading.Timer(30, process.kill)
watchdog.start()
try:
    for line in process.stdout:
        print(line, end='', flush=True)
        if 'ZOOM_READY ' in line:
            x, y = line.split('ZOOM_READY ', 1)[1].split()
            window = subprocess.check_output(
                ['xdotool', 'search', '--name', '^tio-gui zoom acceptance$'], text=True
            ).splitlines()[-1]
            subprocess.run(['xdotool', 'mousemove', '--window', window, x, y,
                            'keydown', 'ctrl', 'click', '--repeat', '2', '--delay', '100', '4',
                            'keyup', 'ctrl'], check=True)
    sys.exit(process.wait())
finally:
    watchdog.cancel()
    if process.poll() is None:
        process.kill()
        process.wait()
