#!/usr/bin/env python3
"""Exercise an installer in an isolated Wine prefix under tests/headless.py.

This verifies software loading/install/uninstall, not USB driver behavior.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import getpass
import tempfile
import sys
import time

repo = Path(__file__).resolve().parents[1]
installer = Path(sys.argv[1]).resolve()
prefix = Path(tempfile.mkdtemp(prefix="wine-gui-", dir=repo / ".cache"))
environment = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all", GSK_RENDERER="cairo", GDK_BACKEND="win32", WINEDLLOVERRIDES="mscoree,mshtml=")
directory = prefix / "drive_c/users" / getpass.getuser() / "AppData/Local/Programs/tio-gui-serial"
relocated = prefix / "drive_c/tio package test"
win_directory = r"C:\tio package test"


def wine(*args, **kwargs):
    return subprocess.run(["wine", *map(str, args)], env=environment, timeout=120, check=True, **kwargs)


wine(installer, "/S")
assert (directory / "bin/tio-gui.exe").exists()
font_manifest = json.loads((directory / "FONT-DEPENDENCIES.json").read_text())
font = directory / "share/fonts/NotoSansSC-Regular.otf"
assert hashlib.sha256(font.read_bytes()).hexdigest() == font_manifest[0]["sha256"]
assert (directory / "share/licenses/NotoSansSC/OFL.txt").is_file()
shutil.copytree(directory, relocated, dirs_exist_ok=True)
wine(relocated / "bin/tio-gui.exe", "--version")
test_environment = dict(environment, WINEPATH=win_directory + r"\bin")
for test in ["native-serial-test", "payload-test", "highlighter-test"]:
    subprocess.run(["wine", str(repo / ".cache/serial-windows" / (test + ".exe"))], env=test_environment, timeout=60, check=True)
log = repo / ".cache/windows-startup.log"
with log.open("w") as output:
    process = subprocess.Popen(["wine", str(relocated / "bin/tio-gui.exe")], env=environment, stdout=output, stderr=output)
    try:
        time.sleep(8)
        assert process.poll() is None, log.read_text()
        windows = subprocess.check_output(["xdotool", "search", "--name", "tio-gui.*Serial"], text=True).splitlines()
        assert windows, "No application window"
        try:
            from PIL import ImageGrab
            ImageGrab.grab().save(repo / ".cache/windows-serial.png")
        except ImportError:
            pass
        # Click the GTK titlebar close control: XDestroyWindow bypasses Win32's
        # normal close message and cannot exercise application shutdown.
        geometry = subprocess.check_output(["xdotool", "getwindowgeometry", "--shell", windows[-1]], text=True)
        width = int(dict(line.split("=", 1) for line in geometry.splitlines() if "=" in line)["WIDTH"])
        subprocess.run(["xdotool", "mousemove", "--window", windows[-1], str(width - 20), "22", "click", "1"], check=True)
        process.wait(timeout=20)
    finally:
        if process.poll() is None:
            process.terminate(); process.wait(timeout=10)
    assert "ERROR" not in log.read_text(), log.read_text()
    assert "Could not load bundled font" not in log.read_text(), log.read_text()
wine(directory / "Uninstall.exe", "/S")
for _ in range(100):
    if not (directory / "bin/tio-gui.exe").exists(): break
    time.sleep(0.1)
assert not (directory / "bin/tio-gui.exe").exists()
shutil.rmtree(relocated)
print("Windows installer, bundled runtime, GUI startup, tests and uninstall passed (Wine)")
