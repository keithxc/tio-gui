#!/usr/bin/env python3
"""Build a relocatable GTK .app and DMG from a native Homebrew build."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

repo = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
output.mkdir(parents=True, exist_ok=True)
version = (repo / "VERSION").read_text().strip()
stage = build / "dmg-stage"
if stage.exists():
    shutil.rmtree(stage)
stage.mkdir()
app = stage / "tio-gui.app"
shutil.copytree(build / "tio-gui.app", app)
contents = app / "Contents"
resources = contents / "Resources"
libraries = contents / "Frameworks"
resources.mkdir(exist_ok=True)
libraries.mkdir(exist_ok=True)
shutil.copytree(build / "locale", resources / "share/locale", dirs_exist_ok=True)
brew = Path(subprocess.check_output(["brew", "--prefix"], text=True).strip())


def run(*args):
    return subprocess.check_output([str(x) for x in args], text=True)


def dependencies(path):
    return [line.strip().split(" (compatibility")[0] for line in run("otool", "-L", path).splitlines()[1:]]


pending = [(contents / "MacOS" / "tio-gui", build / "tio-gui.app/Contents/MacOS/tio-gui")]
copied = {}
for module in (brew / "lib/gdk-pixbuf-2.0/2.10.0/loaders").glob("*.so"):
    target = resources / "lib/gdk-pixbuf-2.0/2.10.0/loaders" / module.name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(module, target)
    pending.append((target, module))

processed = []
while pending:
    target, original = pending.pop()
    target.chmod(0o755)
    subprocess.run(["codesign", "--remove-signature", str(target)], capture_output=True)
    for dependency in dependencies(target):
        if dependency.startswith(("/usr/lib/", "/System/Library/")):
            continue
        source = Path(dependency)
        if dependency.startswith("@loader_path/"):
            source = original.parent / dependency.removeprefix("@loader_path/")
        elif dependency.startswith("@rpath/"):
            source = brew / "lib" / dependency.removeprefix("@rpath/")
        if not source.exists():
            raise SystemExit(f"Unresolved dependency: {dependency} in {original}")
        source = source.resolve()
        if source.name not in copied:
            destination = libraries / source.name
            shutil.copy2(source, destination)
            copied[source.name] = source
            pending.append((destination, source))
        elif copied[source.name] != source:
            raise SystemExit(f"Conflicting library names: {source}")
        relative = os.path.relpath(libraries / source.name, target.parent)
        subprocess.run(["install_name_tool", "-change", dependency, "@loader_path/" + relative, str(target)], check=True)
    if target.suffix in (".dylib", ".so"):
        subprocess.run(["install_name_tool", "-id", "@loader_path/" + target.name, str(target)], check=True)
    processed.append(target)

for relative in ["share/glib-2.0/schemas", "share/icons/hicolor"]:
    source = brew / relative
    if source.exists():
        shutil.copytree(source, resources / relative, dirs_exist_ok=True)
subprocess.run([str(brew / "bin/glib-compile-schemas"), str(resources / "share/glib-2.0/schemas")], check=True)
cache = run(brew / "bin/gdk-pixbuf-query-loaders")
cache = cache.replace(str(brew / "lib/gdk-pixbuf-2.0"), "@BUNDLE_RESOURCES@/lib/gdk-pixbuf-2.0")
# Homebrew sometimes records Cellar paths instead of the linked prefix.
for line in cache.splitlines():
    if line.startswith('"/') and '"' in line[1:]:
        path = line.split('"')[1]
        if "/loaders/" in path:
            cache = cache.replace(path, "@BUNDLE_RESOURCES@/lib/gdk-pixbuf-2.0/2.10.0/loaders/" + Path(path).name)
(resources / "loaders.cache.in").write_text(cache)
licenses = resources / "licenses"
licenses.mkdir()
shutil.copy2(repo / "LICENSE", licenses / "tio-gui-GPL-3.0.txt")
for formula in (brew / "Cellar").iterdir():
    if any(str(source).startswith(str(formula) + "/") for source in copied.values()) or formula.name == "gtk4":
        for source in formula.glob("*/LICENSE*"):
            if source.is_file():
                shutil.copy2(source, licenses / (formula.name + "-" + source.name))
(resources / "BUILD-DEPENDENCIES.txt").write_text("\n".join(str(p) for p in sorted(copied.values())) + "\n")
iconset = build / "tio-gui.iconset"
iconset.mkdir(exist_ok=True)
for size in [16, 32, 128, 256, 512]:
    png = repo / f"data/icons/hicolor/{size}x{size}/apps/io.github.keithxc.tio_gui.png"
    shutil.copy2(png, iconset / f"icon_{size}x{size}.png")
    double = repo / f"data/icons/hicolor/{size*2}x{size*2}/apps/io.github.keithxc.tio_gui.png"
    if double.exists(): shutil.copy2(double, iconset / f"icon_{size}x{size}@2x.png")
subprocess.run(["iconutil", "-c", "icns", "-o", str(resources / "tio-gui.icns"), str(iconset)], check=True)
info_path = contents / "Info.plist"
with info_path.open("rb") as f: info = plistlib.load(f)
info.update({"CFBundleIconFile": "tio-gui.icns", "NSHighResolutionCapable": True,
             "LSMinimumSystemVersion": "26.0", "NSHumanReadableCopyright": "GPL-3.0-only"})
with info_path.open("wb") as f: plistlib.dump(info, f)
for target in processed:
    for dependency in dependencies(target):
        if not dependency.startswith(("@loader_path/", "/usr/lib/", "/System/Library/")):
            raise SystemExit(f"Unbundled library: {target}: {dependency}")
    if target.parent != contents / "MacOS":
        subprocess.run(["codesign", "--force", "--sign", "-", str(target)], check=True)
subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
os.symlink("/Applications", stage / "Applications")
shutil.copy2(repo / "docs/serial-portable.md", stage / "README.txt")
arch = run("uname", "-m").strip()
dmg = output / f"tio-gui-{version}-macos-{arch}.dmg"
subprocess.run(["hdiutil", "create", "-ov", "-format", "UDZO", "-volname", "tio-gui Serial", "-srcfolder", str(stage), str(dmg)], check=True)
print(dmg)
