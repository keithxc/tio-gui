#!/usr/bin/env python3
"""Check bundled GLib charset conversion without the builder's runtime data."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys


if len(sys.argv) == 2:
    environment = {"HOME": os.environ["HOME"], "PATH": "/usr/bin:/bin",
                   "LANG": "en_US.UTF-8", "PYTHONDONTWRITEBYTECODE": "1"}
    subprocess.run([
        "/usr/bin/sandbox-exec", "-p",
        '(version 1)(allow default)(deny network*)(deny file-write*)'
        '(deny file-read* (subpath "/nix/store") (subpath "/opt/homebrew")'
        ' (subpath "/usr/local/Cellar"))',
        "/usr/bin/python3", str(Path(__file__).resolve()), sys.argv[1], "--child",
    ], env=environment, check=True, timeout=30)
else:
    app = Path(sys.argv[1]).resolve()
    glib = ctypes.CDLL(str(app / "Contents/Frameworks/libglib-2.0.0.dylib"))
    glib.g_convert.restype = ctypes.c_void_p
    glib.g_convert.argtypes = [ctypes.c_char_p, ctypes.c_ssize_t, ctypes.c_char_p,
                              ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t),
                              ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_void_p)]
    glib.g_free.argtypes = [ctypes.c_void_p]
    for charset, payload, expected in [
        (b"US-ASCII", b"bundle-smoke", b"bundle-smoke"),
        (b"ISO-8859-1", b"caf\xe9", "café".encode()),
        (b"GB18030", b"\xc4\xe3\xba\xc3", "你好".encode()),
    ]:
        consumed, written, error = ctypes.c_size_t(), ctypes.c_size_t(), ctypes.c_void_p()
        result = glib.g_convert(payload, len(payload), b"UTF-8", charset,
                                ctypes.byref(consumed), ctypes.byref(written), ctypes.byref(error))
        assert result and not error.value, f"Bundled conversion failed for {charset.decode()}"
        try:
            assert consumed.value == len(payload)
            assert ctypes.string_at(result, written.value) == expected
        finally:
            glib.g_free(result)
        print(f"PASS bundled {charset.decode()} -> UTF-8 with development roots denied", flush=True)
