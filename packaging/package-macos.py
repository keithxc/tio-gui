#!/usr/bin/env python3
"""Build a relocatable GTK .app and DMG from a Nix or Homebrew native build."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

from macos_runtime import system_library_for

# Nix also provides xcrun/otool shims for its SDK. Distribution tools must use
# the installed Apple toolchain, especially notarytool and codesign.
os.environ["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:" + os.environ.get("PATH", "")
for key in ("DEVELOPER_DIR", "SDKROOT"):
    if os.environ.get(key, "").startswith("/nix/"): os.environ.pop(key)

repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--identity", default="-")
parser.add_argument("--notary-profile")
args = parser.parse_args()
if args.notary_profile and args.identity == "-":
    parser.error("Notarization requires a Developer ID Application identity")
build = args.build.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
version = (repo / "VERSION").read_text().strip()
with (build / "tio-gui.app/Contents/Info.plist").open("rb") as file:
    build_info = plistlib.load(file)
if build_info.get("CFBundleShortVersionString") != version or build_info.get("CFBundleVersion") != version:
    raise SystemExit("Built app version does not match VERSION; reconfigure and rebuild before packaging")
# Each packaging attempt gets a fresh workspace. macOS can protect a previously
# signed/launched bundle against in-place changes through App Management.
stage = Path(tempfile.mkdtemp(prefix=f"dmg-stage-{version}-", dir=build))
def writable_tree(root):
    if not root.exists(): return
    for path in [root, *root.rglob("*")]:
        if not path.is_symlink(): path.chmod(path.stat().st_mode | 0o200 | (0o100 if path.is_dir() else 0))

def copy_tree(source, target):
    writable_tree(target)
    shutil.copytree(source, target, dirs_exist_ok=True)
    writable_tree(target)

app = stage / "tio-gui.app"
(output / "app-bundle-path.txt").write_text(str(app) + "\n")
shutil.copytree(build / "tio-gui.app", app)
contents = app / "Contents"
resources = contents / "Resources"
libraries = contents / "Frameworks"
resources.mkdir(exist_ok=True)
libraries.mkdir(exist_ok=True)
shutil.copytree(build / "locale", resources / "share/locale", dirs_exist_ok=True)
runtime = json.loads(Path(os.environ["TIO_MACOS_RUNTIME"]).read_text()) if os.environ.get("TIO_MACOS_RUNTIME") else None
brew = None if runtime else Path(subprocess.check_output(["brew", "--prefix"], text=True).strip())
pixbuf = Path(runtime["pixbuf"]) if runtime else brew
query_loaders = runtime["queryLoaders"] if runtime else brew / "bin/gdk-pixbuf-query-loaders"
compile_schemas = runtime["compileSchemas"] if runtime else brew / "bin/glib-compile-schemas"
data_roots = [Path(p) for p in runtime["dataRoots"]] if runtime else [brew]


def run(*args):
    try:
        return subprocess.check_output([str(x) for x in args], text=True)
    except subprocess.CalledProcessError as error:
        if error.output: print(error.output, file=sys.stderr)
        raise


def dependencies(path):
    return [line.strip().split(" (compatibility")[0] for line in run("otool", "-L", path).splitlines()[1:]]


pending = [(contents / "MacOS" / "tio-gui", build / "tio-gui.app/Contents/MacOS/tio-gui")]
copied = {}
runtime_sources = []
for name in ("sz", "rz", "lua", "qjs"):
    source = Path(runtime["helpers"][name]) if runtime else brew / "bin" / name
    if not source.exists(): raise SystemExit(f"Missing bundled runtime: {name}")
    target = contents / "MacOS" / name
    shutil.copy2(source.resolve(), target)
    pending.append((target, source.resolve()))
    runtime_sources.append(source.resolve())
for module in (pixbuf / "lib/gdk-pixbuf-2.0/2.10.0/loaders").glob("*.so"):
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
            name = dependency.removeprefix("@rpath/")
            lines = run("otool", "-l", original).splitlines()
            rpaths = [lines[i + 2].strip().split("path ", 1)[1].split(" (offset", 1)[0]
                      for i, line in enumerate(lines) if line.strip() == "cmd LC_RPATH"]
            candidates = [Path(r.replace("@loader_path", str(original.parent))) / name for r in rpaths]
            if brew: candidates.append(brew / "lib" / name)
            source = next((p for p in candidates if p.exists()), source)
        if not source.exists():
            raise SystemExit(f"Unresolved dependency: {dependency} in {original}")
        source = source.resolve()
        system_library = system_library_for(source)
        if system_library:
            subprocess.run(["install_name_tool", "-change", dependency,
                            system_library, str(target)], check=True)
            continue
        if source.name not in copied:
            destination = libraries / source.name
            shutil.copy2(source, destination)
            copied[source.name] = source
            pending.append((destination, source))
        elif copied[source.name] != source:
            raise SystemExit(f"Conflicting library names: {source}")
        relative = os.path.relpath(libraries / source.name, target.parent)
        subprocess.run(["install_name_tool", "-change", dependency, "@loader_path/" + relative, str(target)], check=True)
    # Bundled code must never consult an installed Nix/Brew runtime through LC_RPATH.
    lines = run("otool", "-l", target).splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            rpath = lines[i + 2].strip().split("path ", 1)[1].split(" (offset", 1)[0]
            if rpath.startswith(("/nix/", "/opt/homebrew/", "/usr/local/")):
                subprocess.run(["install_name_tool", "-delete_rpath", rpath, str(target)], check=True)
    if target.suffix in (".dylib", ".so"):
        subprocess.run(["install_name_tool", "-id", "@loader_path/" + target.name, str(target)], check=True)
    processed.append(target)

for relative in ["share/glib-2.0/schemas", "share/icons/hicolor"]:
    for root in data_roots:
        source = root / relative
        if source.exists():
            copy_tree(source, resources / relative)
# Nix stores GTK schemas in a versioned gsettings-schemas subtree.
if runtime:
    for root in data_roots:
        for schema in (root / "share/gsettings-schemas").glob("*/glib-2.0/schemas"):
            copy_tree(schema, resources / "share/glib-2.0/schemas")
for path in (resources / "share/glib-2.0/schemas").rglob("*"):
    if path.is_file(): path.chmod(0o644)
subprocess.run([str(compile_schemas), str(resources / "share/glib-2.0/schemas")], check=True)
cache = run(query_loaders)
cache = cache.replace(str(pixbuf / "lib/gdk-pixbuf-2.0"), "@BUNDLE_RESOURCES@/lib/gdk-pixbuf-2.0")
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
used_sources = list(copied.values()) + runtime_sources
(resources / "BUILD-DEPENDENCIES.txt").write_text("\n".join(str(p) for p in sorted(used_sources)) + "\n")
if runtime:
    manifest = dict(runtime)
    # Exact runtime roots + derivations allow retrieving corresponding sources
    # from the same pinned nixpkgs without collecting unrelated local packages.
    roots = sorted({str(Path(*p.parts[:4])) for p in used_sources if str(p).startswith("/nix/store/")})
    manifest["bundledStoreRoots"] = [{"path": p, "deriver": run("nix-store", "--query", "--deriver", p).strip()} for p in roots]
    for root in roots:
        for pattern in ("share/licenses/**/*", "share/doc/*/COPYING*", "share/doc/*/LICENSE*"):
            for source in Path(root).glob(pattern):
                if source.is_file(): shutil.copy2(source, licenses / (Path(root).name + "-" + source.name))
else:
    used_formulae = []
    for formula in (brew / "Cellar").iterdir():
        if any(str(source).startswith(str(formula) + "/") for source in used_sources) or formula.name == "gtk4":
            used_formulae.append(formula.name)
            for pattern in ("*/LICENSE*", "*/COPYING*", "*/COPYRIGHT*"):
                for source in formula.glob(pattern):
                    if source.is_file(): shutil.copy2(source, licenses / (formula.name + "-" + source.name))
    formula_info = json.loads(run("brew", "info", "--json=v2", "--formula", *sorted(used_formulae)))
    manifest = [{"name": f["name"], "license": f.get("license"), "version": f.get("linked_keg"),
                 "source": f.get("urls", {}).get("stable", {})} for f in formula_info["formulae"]]
(resources / "BUILD-DEPENDENCIES.json").write_text(json.dumps(manifest, indent=2) + "\n")
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
info_path.chmod(0o644)
with info_path.open("wb") as f: plistlib.dump(info, f)
for target in processed:
    for dependency in dependencies(target):
        if not dependency.startswith(("@loader_path/", "/usr/lib/", "/System/Library/")):
            raise SystemExit(f"Unbundled library: {target}: {dependency}")
    if target == contents / "MacOS" / "tio-gui": continue
    command = ["codesign", "--force", "--sign", args.identity]
    if args.identity != "-": command += ["--timestamp", "--options", "runtime"]
    subprocess.run(command + [str(target)], check=True)
command = ["codesign", "--force", "--sign", args.identity]
if args.identity != "-": command += ["--timestamp", "--options", "runtime"]
subprocess.run(command + [str(app)], check=True)
subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
# Exercise actual GLib conversions with development roots denied. A valid
# signature and LC_LOAD_DYLIB audit cannot detect iconv's dlopen/data paths.
subprocess.run([sys.executable, str(repo / "tests/macos_conversion_smoke.py"), str(app)], check=True)
if args.identity != "-":
    signature = subprocess.check_output(["codesign", "-dvv", str(app)], stderr=subprocess.STDOUT, text=True)
    if "Authority=Developer ID Application:" not in signature or "Runtime Version=" not in signature:
        raise SystemExit("Expected Developer ID Application with hardened runtime")
if args.notary_profile:
    submission = output / f"tio-gui-{version}-notary.zip"
    subprocess.run(["ditto", "-c", "-k", "--keepParent", str(app), str(submission)], check=True)
    result = run("xcrun", "notarytool", "submit", submission, "--keychain-profile", args.notary_profile, "--wait", "--output-format", "json")
    (output / "app-notary.json").write_text(result)
    import json
    if json.loads(result).get("status") != "Accepted": raise SystemExit("App notarization failed")
    subprocess.run(["xcrun", "stapler", "staple", str(app)], check=True)
    subprocess.run(["xcrun", "stapler", "validate", str(app)], check=True)
    subprocess.run(["spctl", "--assess", "--type", "execute", "--verbose=2", str(app)], check=True)
    submission.unlink()
os.symlink("/Applications", stage / "Applications")
shutil.copy2(repo / "docs/serial-portable.md", stage / "README.txt")
arch = run("uname", "-m").strip()
dmg = output / f"tio-gui-{version}-macos-{arch}.dmg"
subprocess.run(["hdiutil", "create", "-ov", "-format", "UDZO", "-volname", "tio-gui Serial", "-srcfolder", str(stage), str(dmg)], check=True)
if args.identity != "-": subprocess.run(command + [str(dmg)], check=True)
if args.notary_profile:
    result = run("xcrun", "notarytool", "submit", dmg, "--keychain-profile", args.notary_profile, "--wait", "--output-format", "json")
    (output / "dmg-notary.json").write_text(result)
    if json.loads(result).get("status") != "Accepted": raise SystemExit("DMG notarization failed")
    subprocess.run(["xcrun", "stapler", "staple", str(dmg)], check=True)
    subprocess.run(["xcrun", "stapler", "validate", str(dmg)], check=True)
zip_path = output / f"tio-gui-{version}-macos-{arch}.zip"
subprocess.run(["ditto", "-c", "-k", "--keepParent", str(app), str(zip_path)], check=True)
print(dmg)
print(zip_path)
