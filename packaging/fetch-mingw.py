#!/usr/bin/env python3
"""Fetch verified MSYS2 mingw64 packages for a Linux cross build.

Uses repository SHA256 checksums, records exact package versions, and never
executes package installation hooks. Requires tar with zstd support.
"""
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import urllib.request
import time

base = "https://repo.msys2.org/mingw/mingw64/"
root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=True)
cache = root / "packages"
cache.mkdir(exist_ok=True)


def download(url, path):
    if not path.exists():
        for attempt in range(4):
            try:
                with urllib.request.urlopen(url, timeout=30) as response:
                    temp = path.with_suffix(path.suffix + ".tmp")
                    temp.write_bytes(response.read())
                    temp.replace(path)
                break
            except (OSError, TimeoutError):
                if attempt == 3: raise
                time.sleep(1)


db = root / "mingw64.db"
download(base + "mingw64.db", db)
packages = {}
raw_db = subprocess.check_output(["zstd", "-dc", str(db)])
with tarfile.open(fileobj=io.BytesIO(raw_db)) as archive:
    for member in archive:
        if not member.name.endswith("/desc"):
            continue
        content = archive.extractfile(member).read().decode()
        fields = {}
        for block in content.strip().split("\n\n"):
            lines = block.splitlines()
            fields[lines[0].strip("%")] = lines[1:]
        packages[fields["NAME"][0]] = fields

pending = ["mingw-w64-x86_64-gtk4"]
providers = {}
for name, package in packages.items():
    for provided in package.get("PROVIDES", []):
        providers[re.split(r"[<>=]", provided)[0]] = name
selected = {}
while pending:
    name = re.split(r"[<>=]", pending.pop())[0]
    if name not in packages:
        name = providers.get(name, name)
    if name in selected:
        continue
    if name not in packages:
        raise SystemExit(f"Missing dependency: {name}")
    package = packages[name]
    selected[name] = package
    pending.extend(package.get("DEPENDS", []))

manifest = []
for name, package in sorted(selected.items()):
    filename = package["FILENAME"][0]
    path = cache / filename
    print(filename, flush=True)
    download(base + filename, path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != package["SHA256SUM"][0]:
        raise SystemExit(f"Checksum mismatch: {filename}")
    marker = root / (filename + ".extracted")
    if not marker.exists():
        subprocess.run(["tar", "--zstd", "-xf", str(path), "-C", str(root)], check=True)
        marker.touch()
    manifest.append({"name": name, "version": package["VERSION"][0], "sha256": digest, "url": base + filename})
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(root / "mingw64")
