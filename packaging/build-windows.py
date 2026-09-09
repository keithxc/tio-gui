#!/usr/bin/env python3
"""Cross compile with MinGW and stage a self-contained Windows NSIS installer.

Run inside a shell providing MinGW gcc/objdump, CMake, pkg-config, NSIS and
glib-compile-schemas, after fetch-mingw.py. No Windows emulation is needed.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys

repo = Path(__file__).resolve().parents[1]
sysroot = Path(sys.argv[1]).resolve()
build = Path(sys.argv[2]).resolve()
output = Path(sys.argv[3]).resolve()
build.mkdir(parents=True, exist_ok=True)
output.mkdir(parents=True, exist_ok=True)
prefix = sysroot / "mingw64"
icon_images = [(size, (repo / f"data/icons/hicolor/{size}x{size}/apps/io.github.keithxc.tio_gui.png").read_bytes()) for size in (16, 32, 48, 64, 128, 256)]
offset = 6 + 16 * len(icon_images)
icon = struct.pack("<HHH", 0, 1, len(icon_images))
for size, data in icon_images:
    icon += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
icon += b"".join(data for _, data in icon_images)
(build / "tio-gui.ico").write_bytes(icon)
(build / "icon.rc").write_text('1 ICON "' + (build / "tio-gui.ico").as_posix() + '"\n')
environment = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=str(sysroot),
                   PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"), PKG_CONFIG_PATH="")
toolchain = build / "toolchain.cmake"
toolchain.write_text('set(CMAKE_SYSTEM_NAME Windows)\n'
                     'set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)\n'
                     'set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)\n')
subprocess.run(["cmake", "-S", str(repo), "-B", str(build), "-G", "Ninja",
                "-DTIO_GUI_SERIAL_ONLY=ON", "-DCMAKE_BUILD_TYPE=Release",
                "-DTIO_GUI_WINDOWS_ICON_RC=" + str(build / "icon.rc"),
                "-DCMAKE_EXE_LINKER_FLAGS=" + os.environ.get("TIO_WINDOWS_LDFLAGS", ""),
                "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain)], env=environment, check=True)
subprocess.run(["cmake", "--build", str(build)], env=environment, check=True)
stage = build / "stage"
if stage.exists(): shutil.rmtree(stage)
(stage / "bin").mkdir(parents=True)
shutil.copy2(build / "tio-gui.exe", stage / "bin/tio-gui.exe")
pending = [stage / "bin/tio-gui.exe"]
shutil.copy2(prefix / "bin/gdk-pixbuf-query-loaders.exe", stage / "bin/gdk-pixbuf-query-loaders.exe")
pending.append(stage / "bin/gdk-pixbuf-query-loaders.exe")
for source in (prefix / "lib/gdk-pixbuf-2.0/2.10.0/loaders").glob("*.dll"):
    destination = stage / "lib/gdk-pixbuf-2.0/2.10.0/loaders" / source.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination); pending.append(destination)
available = {p.name.lower(): p for p in (prefix / "bin").glob("*.dll")}
if os.environ.get("TIO_WINDOWS_EXTRA_DLL_DIR"):
    available.update({p.name.lower(): p for p in Path(os.environ["TIO_WINDOWS_EXTRA_DLL_DIR"]).glob("*.dll")})
# Windows system imports; everything else must be packaged, including recursive imports.
system = set("kernel32.dll user32.dll gdi32.dll advapi32.dll shell32.dll ole32.dll oleaut32.dll "
             "ws2_32.dll msvcrt.dll ntdll.dll comdlg32.dll comctl32.dll shlwapi.dll winmm.dll "
             "imm32.dll setupapi.dll cfgmgr32.dll version.dll winspool.drv secur32.dll crypt32.dll "
             "bcrypt.dll dnsapi.dll iphlpapi.dll dwmapi.dll dwrite.dll d2d1.dll d3d11.dll dxgi.dll "
             "opengl32.dll usp10.dll uxtheme.dll normaliz.dll winhttp.dll wtsapi32.dll hid.dll "
             "msimg32.dll psapi.dll d3d9.dll d3dcompiler_47.dll avrt.dll powrprof.dll propsys.dll "
             "userenv.dll shcore.dll dbghelp.dll authz.dll netapi32.dll wintrust.dll "
             "dcomp.dll dxva2.dll mf.dll mfplat.dll mfuuid.dll mfreadwrite.dll ksuser.dll "
             "mswsock.dll oleacc.dll cabinet.dll rpcrt4.dll winusb.dll ncrypt.dll gdiplus.dll bcryptprimitives.dll d3d12.dll".split())
copied = set()
missing = set()
while pending:
    binary = pending.pop()
    report = subprocess.check_output(["x86_64-w64-mingw32-objdump", "-p", str(binary)], text=True)
    for line in report.splitlines():
        if "DLL Name:" not in line: continue
        name = line.split("DLL Name:")[1].strip().lower()
        if name in copied or name in system or name.startswith(("api-ms-win-", "ext-ms-win-")): continue
        if name not in available:
            missing.add(name); continue
        source = available[name]; destination = stage / "bin" / source.name
        shutil.copy2(source, destination); copied.add(name); pending.append(destination)
if missing: raise SystemExit("Missing runtime dependencies: " + ", ".join(sorted(missing)))
for relative in ["share/glib-2.0/schemas", "share/icons/Adwaita", "share/icons/hicolor", "etc/fonts", "share/fontconfig", "share/licenses"]:
    if (prefix / relative).exists(): shutil.copytree(prefix / relative, stage / relative, dirs_exist_ok=True)
subprocess.run(["glib-compile-schemas", str(stage / "share/glib-2.0/schemas")], check=True)
shutil.copy2(repo / "LICENSE", stage / "LICENSE.txt")
shutil.copy2(repo / "docs/serial-portable.md", stage / "README.txt")
shutil.copy2(sysroot / "manifest.json", stage / "BUILD-DEPENDENCIES.json")
version = (repo / "VERSION").read_text().strip()
installer = output / f"tio-gui-{version}-windows-x86_64-setup.exe"


def nsis(value):
    return str(value).replace("$", "$$").replace('"', '$\\"')


script = build / "installer.nsi"
lines = [
    'Unicode true', '!include "MUI2.nsh"', 'Name "tio-gui Serial"',
    'OutFile "' + nsis(installer) + '"',
    'InstallDir "$LOCALAPPDATA\\Programs\\tio-gui-serial"', 'RequestExecutionLevel user',
    'SetCompressor /SOLID lzma', '!insertmacro MUI_PAGE_WELCOME',
    '!insertmacro MUI_PAGE_LICENSE "' + nsis(repo / "LICENSE") + '"',
    '!insertmacro MUI_PAGE_DIRECTORY', '!insertmacro MUI_PAGE_INSTFILES',
    '!insertmacro MUI_PAGE_FINISH', '!insertmacro MUI_UNPAGE_CONFIRM',
    '!insertmacro MUI_UNPAGE_INSTFILES', '!insertmacro MUI_LANGUAGE "English"',
    'Section "Install"', 'SetOutPath "$INSTDIR"', 'File /r "' + nsis(stage) + '/*"',
    'CreateShortcut "$SMPROGRAMS\\tio-gui Serial.lnk" "$INSTDIR\\bin\\tio-gui.exe"',
    'CreateShortcut "$DESKTOP\\tio-gui Serial.lnk" "$INSTDIR\\bin\\tio-gui.exe"',
    'WriteUninstaller "$INSTDIR\\Uninstall.exe"',
    'WriteRegStr HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\tio-gui-serial" "DisplayName" "tio-gui Serial"',
    'WriteRegStr HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\tio-gui-serial" "DisplayVersion" "' + version + '"',
    'WriteRegStr HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\tio-gui-serial" "UninstallString" \'"$INSTDIR\\Uninstall.exe"\'',
    'SectionEnd', 'Section "Uninstall"',
    'Delete "$SMPROGRAMS\\tio-gui Serial.lnk"', 'Delete "$DESKTOP\\tio-gui Serial.lnk"',
    'DeleteRegKey HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\tio-gui-serial"',
]
for path in sorted(stage.rglob("*")):
    if path.is_file(): lines.append('Delete "$INSTDIR\\' + nsis(path.relative_to(stage)).replace('/', '\\') + '"')
for path in sorted((p for p in stage.rglob("*") if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
    lines.append('RMDir "$INSTDIR\\' + nsis(path.relative_to(stage)).replace('/', '\\') + '"')
lines.extend(['Delete "$INSTDIR\\Uninstall.exe"', 'RMDir "$INSTDIR"', 'SectionEnd'])
script.write_text("\n".join(lines) + "\n")
subprocess.run(["makensis", "-V2", str(script)], check=True)
print(installer)
