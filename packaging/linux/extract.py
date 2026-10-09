#!/usr/bin/env python3
"""Extract a type-2 SquashFS AppImage without the host's binfmt handler."""
from pathlib import Path
import subprocess
import sys

source, destination = map(Path, sys.argv[1:])
data = source.read_bytes()
offset = 0
while True:
    offset = data.find(b'hsqs', offset)
    if offset < 0:
        raise SystemExit(f'No valid SquashFS filesystem in {source}')
    result = subprocess.run(['unsquashfs', '-s', '-o', str(offset), str(source)], capture_output=True)
    if result.returncode == 0:
        break
    offset += 4
subprocess.run(['unsquashfs', '-no-progress', '-d', str(destination), '-o',
                str(offset), str(source)], check=True)
