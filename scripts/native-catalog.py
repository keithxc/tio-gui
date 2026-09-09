#!/usr/bin/env python3
"""Compile the shared gettext catalog into the native edition.

Win32 CRT and libintl locale selection can disagree. Embedding this small catalog
keeps explicit application language selection independent of the host locale and
bundle path while retaining po/zh_CN.po as the single translation source.
"""
import gettext
import json
from pathlib import Path
import sys

with Path(sys.argv[1]).open('rb') as stream:
    catalog = gettext.GNUTranslations(stream)
entries = sorted((key, value) for key, value in catalog._catalog.items()
                 if isinstance(key, str) and key and isinstance(value, str))
quote = lambda value: json.dumps(value, ensure_ascii=False)
lines = ['/* Generated from po/zh_CN.po; do not edit. */', '#pragma once',
         'static const struct { const char *id, *value; } native_catalog[] = {']
lines += ['    {' + quote(key) + ', ' + quote(value) + '},' for key, value in entries]
lines += ['};', '']
Path(sys.argv[2]).write_text('\n'.join(lines), encoding='utf-8')
