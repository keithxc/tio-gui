#!/usr/bin/env python3
"""Test a Linux AppImage without the host Nix store, using a private X display."""
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import time

image = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='tio-appimage-') as directory:
    root = Path(directory)
    # NixOS may intercept AppImages through binfmt_misc and invoke its own
    # /nix-based helper. Clear only the ELF padding magic in a temporary copy
    # so the kernel executes the bundled static runtime, as on ordinary Linux.
    if Path('/proc/sys/fs/binfmt_misc/appimage_type_2').exists():
        direct = root / 'direct.AppImage'
        shutil.copy2(image, direct)
        direct.chmod(0o755)
        with direct.open('r+b') as binary:
            binary.seek(8); assert binary.read(3) == b'AI\x02'
            binary.seek(8); binary.write(b'\x00' * 3)
        image = str(direct)
    (root / 'runtime').mkdir(mode=0o700)
    (root / 'config/tio-gui').mkdir(parents=True)
    (root / 'config/tio-gui/config.ini').write_text(
        '[general]\nlanguage=en\n[defaults]\nlogging=true\n'
        f'log-directory={directory}\nlog-file=received.log\n')
    # Bubblewrap starts before /nix is hidden; the AppImage must then bootstrap
    # using its own runtime and bundled libraries, including the tio child.
    launch = ['bwrap', '--ro-bind', '/', '/', '--tmpfs', '/nix',
              '--dev-bind', '/dev', '/dev', '--proc', '/proc', '--bind', '/tmp', '/tmp',
              '--setenv', 'PATH', '/usr/bin:/bin',
              '--setenv', 'XDG_CONFIG_HOME', str(root / 'config'),
              '--setenv', 'XDG_CACHE_HOME', str(root / 'cache'),
              '--setenv', 'XDG_RUNTIME_DIR', str(root / 'runtime'),
              '--setenv', 'TIO_GUI_NON_UNIQUE', '1',
              image, '--appimage-extract-and-run']
    result = subprocess.check_output(launch + ['--version'], text=True, timeout=60)
    version = (Path(__file__).resolve().parents[1] / 'VERSION').read_text().strip()
    assert f'tio-gui {version}' in result, result
    master, slave = pty.openpty()
    device = os.ttyname(slave)
    with (root / 'startup.log').open('w+') as log:
        app = subprocess.Popen(launch + ['--device', device, '--baud', '115200'],
                               stdout=log, stderr=log)
        try:
            window = subprocess.check_output(
                ['xdotool', 'search', '--sync', '--onlyvisible', '--name', '^tio-gui'],
                text=True, timeout=60).splitlines()[-1]
            time.sleep(1)
            subprocess.run(['xdotool', 'windowfocus', '--sync', window,
                            'mousemove', '--window', window, '200', '240', 'click', '1'], check=True)
            subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '30', 'release-036'], check=True)
            subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Return'], check=True)
            received = bytearray()
            deadline = time.monotonic() + 8
            while b'release-036\r' not in received and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    received.extend(os.read(master, 65536))
            assert b'release-036\r' in received, received
            os.write(master, b'PACKAGED_RX_036\r\n')
            time.sleep(.5)
            subprocess.run(['xdotool', 'key', '--clearmodifiers', 'F6'], check=True)
            time.sleep(.5)
            # A per-tab suffix may be added to the requested log filename.
            logs = [p for p in root.glob('received*') if p.is_file()]
            assert any(b'PACKAGED_RX_036' in p.read_bytes() for p in logs), logs
            # Close the tab, then confirm the open-log dialog on this private
            # X display (no window manager: the dialog is placed at the origin).
            subprocess.run(['xdotool', 'mousemove', '--window', window,
                            '325', '18', 'click', '1'], check=True)
            time.sleep(.5)
            subprocess.run(['xdotool', 'mousemove', '183', '86', 'click', '1'], check=True)
            app.wait(timeout=15)
            assert app.returncode == 0, app.returncode
            log.seek(0)
            output = log.read()
            assert 'CRITICAL' not in output and 'ERROR' not in output, output
            assert 'Fontconfig error' not in output, output
        finally:
            if app.poll() is None:
                app.terminate()
                try:
                    app.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    app.kill(); app.wait()
            os.close(slave); os.close(master)
print('AppImage without host /nix: version, GUI, bundled tio PTY TX/RX/logging, and normal close passed.')
