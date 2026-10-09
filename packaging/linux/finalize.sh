#!/usr/bin/env bash
# Run inside the builder after portable runtime validation.
set -euo pipefail
export TMPDIR=/work/tmp ARCH=x86_64
version=$(tr -d '\n' </source/VERSION)
python3 /source/packaging/linux/portable.py static-notices /work/AppDir /work/cache
chmod +x /work/cache/appimagetool /work/cache/appimage-runtime
if [ ! -x /work/cache/appimagetool-extracted/AppRun ]; then
  python3 /source/packaging/linux/extract.py /work/cache/appimagetool /work/cache/appimagetool-extracted
fi
(cd /work/dist && /work/cache/appimagetool-extracted/AppRun --runtime-file /work/cache/appimage-runtime \
  -u 'gh-releases-zsync|keithxc|tio-gui|latest|tio-gui-*-linux-x86_64.AppImage.zsync' \
  /work/AppDir "/work/dist/tio-gui-$version-linux-x86_64.AppImage")
tar -cJf "/work/dist/tio-gui-$version-linux-x86_64.tar.xz" -C /work AppDir
python3 /source/packaging/linux/portable.py native /work/AppDir /work/cache/nfpm /work/dist
# Retain corresponding Ubuntu sources with patches and upstream tool sources.
sed -i 's/Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
apt-get update
mkdir -p /work/runtime-sources/tio-gui
cp /work/cache/{tio,quickjs,libfuse}.tar.xz /work/runtime-sources/
cp /work/cache/{type2-runtime,squashfuse}.tar.gz /work/runtime-sources/
cp /work/cache/{musl.COPYRIGHT,mimalloc.LICENSE,zlib.LICENSE,zstd.LICENSE} /work/runtime-sources/
cp -a /source/. /work/runtime-sources/tio-gui/
python3 /source/packaging/linux/portable.py sources /work/AppDir /work/runtime-sources
tar -cJf "/work/dist/tio-gui-$version-linux-runtime-sources.tar.xz" -C /work runtime-sources
