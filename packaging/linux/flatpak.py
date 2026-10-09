#!/usr/bin/env python3
"""Export the verified portable runtime as a sideloadable Flatpak bundle."""
import argparse
from pathlib import Path
import shutil
import subprocess

APP_ID = 'io.github.keithxc.tio_gui'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('appdir', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('version')
args = parser.parse_args()
output = args.output.resolve()
stage = output / 'flatpak-stage'
if stage.exists():
    raise SystemExit('Choose a fresh output directory; Flatpak stage already exists.')
files = stage / 'files'
shutil.copytree(args.appdir, files / 'opt/tio-gui')
(files / 'bin').mkdir()
(files / 'bin/tio-gui').write_text('#!/bin/sh\nexec /app/opt/tio-gui/AppRun "$@"\n')
(files / 'bin/tio-gui').chmod(0o755)
for directory in ['applications', 'metainfo']:
    shutil.copytree(args.appdir / 'usr/share' / directory, files / 'share' / directory)
shutil.copytree(args.appdir / 'usr/share/icons/hicolor', files / 'share/icons/hicolor',
                ignore=shutil.ignore_patterns('index.theme', 'icon-theme.cache'))
(stage / 'metadata').write_text(f'''[Application]
name={APP_ID}
runtime=org.freedesktop.Platform/x86_64/25.08
command=tio-gui
''')
subprocess.run(['flatpak', 'build-finish', '--command=tio-gui', '--share=ipc', '--share=network',
                '--socket=wayland', '--socket=fallback-x11', '--device=all',
                '--system-talk-name=org.bluez', '--filesystem=xdg-documents', str(stage)], check=True)
repo = output / 'flatpak-repo'
subprocess.run(['flatpak', 'build-export', '--arch=x86_64', str(repo), str(stage), 'stable'], check=True)
subprocess.run(['flatpak', 'build-bundle', '--arch=x86_64',
                '--runtime-repo=https://flathub.org/repo/flathub.flatpakrepo', str(repo),
                str(output / f'tio-gui-{args.version}-linux-x86_64.flatpak'), APP_ID, 'stable'], check=True)
