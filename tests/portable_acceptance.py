#!/usr/bin/env python3
"""Exercise a portable Linux launcher on a private display and PTY.

Run through tests/headless.py. The caller supplies a task-owned work root and
command, and can additionally isolate networking and development libraries.
"""
import argparse
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--work-root', type=Path, required=True)
parser.add_argument('--screenshot', type=Path)
parser.add_argument('--language', default='en')
parser.add_argument('--debugger', action='store_true')
parser.add_argument('command', nargs=argparse.REMAINDER)
args = parser.parse_args()
command = args.command[1:] if args.command[:1] == ['--'] else args.command
root = args.work_root.resolve()
root.mkdir(parents=True, exist_ok=True)
if command[0].endswith('.AppImage'):
    # NixOS's kernel binfmt registration also affects containers. Exercise
    # the image's own static runtime instead of the host's /nix helper, by
    # clearing only the AI2 padding bytes in a disposable copy.
    direct = root / 'direct-runtime.AppImage'
    shutil.copy2(command[0], direct)
    direct.chmod(0o755)
    with direct.open('r+b') as stream:
        stream.seek(8)
        assert stream.read(3) == b'AI\x02'
        stream.seek(8)
        stream.write(b'\x00' * 3)
    command[0] = str(direct)
(root / 'runtime').mkdir(mode=0o700, exist_ok=True)
(root / 'config/tio-gui').mkdir(parents=True, exist_ok=True)
(root / 'config/tio-gui/config.ini').write_text(
    f'[general]\nlanguage={args.language}\n[defaults]\nlogging=true\n'
    f'log-directory={root}\nlog-file=received.log\n')
env = {**os.environ, 'TIO_GUI_NON_UNIQUE': '1', 'XDG_CONFIG_HOME': str(root / 'config'),
       'XDG_CACHE_HOME': str(root / 'cache'), 'XDG_RUNTIME_DIR': str(root / 'runtime'),
       'LANG': 'C', 'LC_ALL': 'C', 'LANGUAGE': 'en', 'GTK_A11Y': 'none'}
version = subprocess.check_output(command + ['--version'], env=env, text=True, timeout=30)
expected_version = (Path(__file__).resolve().parents[1] / 'VERSION').read_text().strip()
assert f'tio-gui {expected_version}' in version, version
master, slave = pty.openpty()
device = os.ttyname(slave)
app = None
try:
    with (root / 'startup.log').open('w+') as log:
        started = time.monotonic()
        gui_command = (['gdb', '--batch', '--return-child-result', '-ex', 'run', '-ex', 'bt', '--args']
                       if args.debugger else []) + command
        app = subprocess.Popen(gui_command + ['--device', device, '--baud', '115200'],
                               env=env, stdout=log, stderr=log, start_new_session=True)
        window = subprocess.check_output(['xdotool', 'search', '--sync', '--onlyvisible',
                                          '--name', '^tio-gui'], text=True, timeout=30).splitlines()[-1]
        elapsed = time.monotonic() - started
        time.sleep(1)
        subprocess.run(['xdotool', 'windowfocus', '--sync', window, 'mousemove', '--window',
                        window, '200', '240', 'click', '1'], check=True)
        subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '30', 'release-043'], check=True)
        subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Return'], check=True)
        received = bytearray()
        deadline = time.monotonic() + 8
        while b'release-043\r' not in received and time.monotonic() < deadline:
            if select.select([master], [], [], .1)[0]:
                received.extend(os.read(master, 65536))
        assert b'release-043\r' in received, received
        payload = b'PACKAGED_RX_043\r\n'
        os.write(master, payload)
        time.sleep(.5)
        if args.screenshot:
            args.screenshot.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['import', '-window', window, str(args.screenshot)], check=True)
        subprocess.run(['xdotool', 'key', '--clearmodifiers', 'F6'], check=True)
        time.sleep(.5)
        logs = [p for p in root.glob('received*') if p.is_file()]
        # tio's default input mapping normalizes CRLF to LF in the log.
        assert any(b'PACKAGED_RX_043' in p.read_bytes() for p in logs), logs
        subprocess.run(['xdotool', 'key', '--clearmodifiers', 'ctrl+w'], check=True)
        time.sleep(.5)
        if app.poll() is None:
            # GTK's close-session dialog defaults to Cancel; Tab selects Close.
            subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Tab', 'Return'], check=True)
        app.wait(timeout=15)
        assert app.returncode == 0, app.returncode
        log.seek(0)
        output = log.read()
        assert not any(x in output for x in ['CRITICAL', 'ERROR', 'Fontconfig error',
                                            'Locale is unavailable', 'Failed to load icon']), output
        print(f'PASS: version, {args.language} X11 window in {elapsed:.2f}s, '
              'bundled tio PTY TX/RX/logging and normal close')
finally:
    if app is not None and app.poll() is None:
        os.killpg(app.pid, signal.SIGTERM)
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(app.pid, signal.SIGKILL)
            app.wait()
    os.close(slave)
    os.close(master)
