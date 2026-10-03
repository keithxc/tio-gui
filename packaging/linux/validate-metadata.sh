#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# With no argument, validate the source metadata. An argument names the
# installed prefix inside a DESTDIR, for example: build/stage/usr.
set -eu

if [ "$#" -gt 1 ]; then
    echo "Usage: $0 [staged-install-prefix]" >&2
    exit 2
fi

for validator in desktop-file-validate appstreamcli; do
    if ! command -v "$validator" >/dev/null 2>&1; then
        echo "Missing validator: $validator" >&2
        exit 1
    fi
done

app_id=io.github.keithxc.tio_gui
if [ "$#" -eq 1 ]; then
    desktop=$1/share/applications/$app_id.desktop
    metainfo=$1/share/metainfo/$app_id.metainfo.xml
    for size in 16 32 48 64 128 256 512; do
        icon=$1/share/icons/hicolor/${size}x${size}/apps/$app_id.png
        if [ ! -s "$icon" ]; then
            echo "Missing installed icon: $icon" >&2
            exit 1
        fi
    done
else
    project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
    desktop=$project_root/data/$app_id.desktop
    metainfo=$project_root/data/$app_id.metainfo.xml
fi

desktop-file-validate "$desktop"
appstreamcli validate --no-net "$metainfo"
