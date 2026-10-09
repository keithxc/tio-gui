#!/usr/bin/env python3
"""Build a relocatable GTK runtime and native packages in Ubuntu 24.04.

Each process uses sharun's private loader, not LD_LIBRARY_PATH injected into
host tools. All downloaded inputs are checksum-locked; Ubuntu package/source
versions and copyright notices are recorded with the shipped runtime.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

SOURCE = Path(__file__).resolve().parents[2]
APP_ID = 'io.github.keithxc.tio_gui'


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def fetch(cache):
    cache.mkdir(parents=True, exist_ok=True)
    for name, item in json.loads((SOURCE / 'packaging/linux/tools.json').read_text()).items():
        target = cache / name
        if not target.exists():
            failures = []
            for url in dict.fromkeys([item['url'], item.get('mirror', item['url'])]):
                try:
                    with tempfile.TemporaryDirectory(dir=cache, prefix='download-') as directory:
                        downloaded = Path(directory) / name
                        run('curl', '-fL', '--retry', '3', '--connect-timeout', '20',
                            '--max-time', '180', '-o', str(downloaded), url)
                        with downloaded.open('rb') as stream:
                            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                        if digest != item['sha256']:
                            raise RuntimeError(f'{url}: unexpected checksum {digest}')
                        downloaded.replace(target)
                    break
                except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
                    failures.append(str(error))
            if not target.exists():
                raise RuntimeError('\n'.join(failures))
        with target.open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual != item['sha256']:
            raise RuntimeError(f'Checksum mismatch for {target}: {actual}')


def elf(path):
    try:
        with path.open('rb') as stream:
            return stream.read(4) == b'\x7fELF'
    except (OSError, IsADirectoryError):
        return False


def static_notices(root, cache):
    fetch(cache)
    destination = root / 'usr/share/doc/tio-gui/runtime-licenses'
    for name in ['musl.COPYRIGHT', 'mimalloc.LICENSE', 'zlib.LICENSE', 'zstd.LICENSE']:
        shutil.copy2(cache / name, destination / name)


def bundle(root, sharun):
    fetch(sharun.parent)
    prefix = root / 'usr'
    libraries = prefix / 'shared/lib'
    libraries.mkdir(parents=True)
    binaries = prefix / 'shared/bin'
    binaries.mkdir(parents=True)
    notices = prefix / 'share/doc/tio-gui/runtime-licenses'
    notices.mkdir(parents=True)
    packages = {}
    queue = []

    def provenance(path):
        candidates = list(dict.fromkeys([str(path.resolve()), str(path)]))
        candidates += [p.removeprefix('/usr') for p in candidates if p.startswith('/usr/lib/')]
        candidates += ['/usr' + p for p in candidates if p.startswith('/lib/')]
        owners = set()
        for candidate in candidates:
            result = subprocess.run(['dpkg-query', '-S', candidate], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            owners = {owner for line in result.stdout.splitlines()
                      for owner in line.rsplit(': ', 1)[0].split(', ')
                      if re.fullmatch(r'[a-z0-9][a-z0-9+.-]*(?::[a-z0-9]+)?', owner)}
            if owners:
                break
        if not owners:
            raise RuntimeError(f'No package owner for {path}')
        for package in owners:
            if package in packages:
                continue
            fields = subprocess.check_output(['dpkg-query', '-W',
                '-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}',
                package], text=True).split('\t')
            packages[package] = dict(zip(['binary', 'version', 'source', 'source_version'], fields))
            shutil.copy2(Path('/usr/share/doc') / package.split(':')[0] / 'copyright',
                         notices / (package.replace(':', '_') + '.copyright'))

    def copy_library(path):
        target = libraries / path.name
        if target.exists():
            return
        shutil.copy2(path, target, follow_symlinks=True)
        provenance(path)
        queue.append(target)

    # Public tools are launched through the same private loader as the GUI.
    for name in ['sz', 'rz', 'lua5.4', 'bwrap', 'gdk-pixbuf-query-loaders']:
        tool = shutil.which(name)
        if not tool and name == 'gdk-pixbuf-query-loaders':
            tool = '/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders'
        path = Path(tool)
        shutil.copy2(path, prefix / 'bin' / name, follow_symlinks=True)
        provenance(path)
    (prefix / 'bin/bwrap').rename(prefix / 'bin/bwrap-real')
    shutil.copy2(Path('/work/bwrap-runtime'), prefix / 'bin/bwrap')
    (prefix / 'bin/lua').symlink_to('lua5.4')
    for path in (prefix / 'bin').iterdir():
        if not path.is_symlink() and elf(path):
            queue.append(path)
    system = Path('/usr/lib/x86_64-linux-gnu')
    for name in ['ld-linux-x86-64.so.2', 'libc.so.6', 'libm.so.6', 'libdl.so.2',
                 'libpthread.so.0', 'librt.so.1', 'libresolv.so.2', 'libnss_dns.so.2',
                 'libnss_files.so.2']:
        copy_library(system / name)
    for source, relative in [(system / 'gconv', 'shared/lib/gconv'),
                             (system / 'gio/modules', 'shared/lib/gio/modules'),
                             (system / 'gdk-pixbuf-2.0/2.10.0/loaders', 'lib/gdk-pixbuf/loaders'),
                             (Path('/usr/share/glib-2.0/schemas'), 'share/glib-2.0/schemas'),
                             (Path('/usr/share/mime'), 'share/mime'),
                             (Path('/usr/share/icons/Adwaita'), 'share/icons/Adwaita'),
                             (Path('/usr/share/fonts/truetype/dejavu'), 'share/fonts/dejavu'),
                             (Path('/usr/share/fonts/opentype/noto'), 'share/fonts/noto')]:
        target = prefix / relative
        shutil.copytree(source, target)
        provenance(Path(str(source) + '/*'))
        queue.extend(p for p in target.rglob('*') if elf(p))
    seen = set()
    while queue:
        path = queue.pop()
        if path in seen:
            continue
        seen.add(path)
        result = subprocess.run(['ldd', str(path)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if 'not found' in result.stdout:
            raise RuntimeError(f'Unresolved dependency {path}:\n{result.stdout}')
        for match in re.findall(r'(?:=>\s+|^\s*)(/\S+)\s+\(', result.stdout, re.M):
            dependency = Path(match)
            if not dependency.is_relative_to(root):
                copy_library(dependency)
    (libraries / 'lib.path').write_text('+\n')
    (notices / 'packages.json').write_text(json.dumps(packages, indent=2) + '\n')
    for name, path in [('tio', Path('/work/tio-source/LICENSE')),
                       ('quickjs', Path('/work/quickjs-source/LICENSE'))]:
        shutil.copy2(path, notices / (name + '.LICENSE'))
    shutil.copy2(SOURCE / 'LICENSE', prefix / 'share/doc/tio-gui/LICENSE')
    shutil.copy2(sharun, prefix / 'sharun')
    (prefix / 'sharun').chmod(0o755)
    shutil.copy2(SOURCE / 'packaging/linux/SHARUN-LICENSE', notices / 'sharun.LICENSE')
    for filename, license_name in [('type2-runtime.tar.gz', 'LICENSE'),
                                   ('libfuse.tar.xz', 'LGPL2.txt'),
                                   ('squashfuse.tar.gz', 'LICENSE')]:
        with tarfile.open(sharun.parent / filename) as archive:
            member = next(m for m in archive.getmembers()
                          if m.name.count('/') == 1 and m.name.endswith('/' + license_name))
            with archive.extractfile(member) as stream, (notices / (filename + '.LICENSE')).open('wb') as output:
                shutil.copyfileobj(stream, output)
    for path in list((prefix / 'bin').iterdir()):
        if path.is_symlink() or not elf(path):
            continue
        shutil.move(path, binaries / path.name)
        os.link(prefix / 'sharun', path)
    desktop = prefix / f'share/applications/{APP_ID}.desktop'
    shutil.copy2(desktop, root / desktop.name)
    shutil.copy2(prefix / f'share/icons/hicolor/256x256/apps/{APP_ID}.png', root / f'{APP_ID}.png')
    (root / '.DirIcon').symlink_to(f'{APP_ID}.png')
    (prefix / 'share/fonts/fonts.conf').write_text('''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig><dir prefix="relative">dejavu</dir><dir prefix="relative">noto</dir>
<cachedir prefix="xdg">fontconfig</cachedir>
<alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
<alias><family>sans-serif</family><prefer><family>DejaVu Sans</family><family>Noto Sans CJK SC</family></prefer></alias>
</fontconfig>
''')
    (root / 'AppRun').write_text('''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
unset SHARUN_DIR LD_LIBRARY_PATH GIO_EXTRA_MODULES GTK_PATH
export APPDIR="$root" PATH="$root/usr/bin:${PATH:-/usr/bin:/bin}"
export GSK_RENDERER="${GSK_RENDERER:-cairo}"
export FONTCONFIG_FILE="$root/usr/share/fonts/fonts.conf"
export FONTCONFIG_PATH="$root/usr/share/fonts"
export GSETTINGS_SCHEMA_DIR="$root/usr/share/glib-2.0/schemas"
export TIO_GUI_LOCALE_DIR="$root/usr/share/locale"
export LOCPATH="$root/usr/lib/locale"
export GIO_MODULE_DIR="$root/usr/shared/lib/gio/modules"
export GDK_PIXBUF_MODULEDIR="$root/usr/lib/gdk-pixbuf/loaders"
export XDG_DATA_DIRS="$root/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
cache="${XDG_CACHE_HOME:-$HOME/.cache}/tio-gui"
mkdir -p "$cache"
export GDK_PIXBUF_MODULE_FILE="$cache/loaders.cache"
"$root/usr/bin/gdk-pixbuf-query-loaders" >"$GDK_PIXBUF_MODULE_FILE"
exec "$root/usr/bin/tio-gui" "$@"
''')
    (root / 'AppRun').chmod(0o755)
    (root / 'README.txt').write_text('tio-gui portable Linux x86_64 package\nRun ./AppRun.\n'
        'Serial access still requires host device permissions. No root/user-group changes are made.\n'
        'Bundled libraries use a private loader. See usr/share/doc/tio-gui/runtime-licenses.\n'
        'Exact corresponding sources are supplied as the linux-runtime-sources release archive.\n')
    static_notices(root, sharun.parent)


def native(root, nfpm, dist):
    version = (SOURCE / 'VERSION').read_text().strip()
    launcher = root.parent / 'native-launcher'
    launcher.write_text('#!/bin/sh\nexec /opt/tio-gui/AppRun "$@"\n')
    launcher.chmod(0o755)
    config = root.parent / 'nfpm.yaml'
    config.write_text(f'''name: tio-gui
arch: amd64
platform: linux
version: {version}
section: utils
priority: optional
maintainer: Keith <keithxc@users.noreply.github.com>
description: Serial debugging workspace with a bundled GTK and tio runtime
homepage: https://github.com/keithxc/tio-gui
license: GPL-3.0-only
contents:
  - src: {root}
    dst: /opt/tio-gui
    type: tree
  - src: {launcher}
    dst: /usr/bin/tio-gui
  - src: {root}/usr/share/applications/{APP_ID}.desktop
    dst: /usr/share/applications/{APP_ID}.desktop
  - src: {root}/usr/share/icons/hicolor
    dst: /usr/share/icons/hicolor
    type: tree
  - src: {root}/usr/share/metainfo/{APP_ID}.metainfo.xml
    dst: /usr/share/metainfo/{APP_ID}.metainfo.xml
''')
    for format_, suffix in [('deb', 'deb'), ('rpm', 'rpm'), ('archlinux', 'pkg.tar.zst')]:
        run(str(nfpm), 'package', '--config', str(config), '--packager', format_,
            '--target', str(dist / f'tio-gui-{version}-linux-x86_64.{suffix}'))


def sources(root, destination):
    packages = json.loads((root / 'usr/share/doc/tio-gui/runtime-licenses/packages.json').read_text())
    inputs = sorted({p['source'] + '=' + p['source_version'] for p in packages.values()})
    run('apt-get', 'source', '--download-only', *inputs, cwd=destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['fetch', 'bundle', 'native', 'sources', 'static-notices'])
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args()
    globals()[args.action.replace('-', '_')](*args.paths)
