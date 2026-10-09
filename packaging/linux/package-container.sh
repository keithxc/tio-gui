#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive TMPDIR=/work/tmp
apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build meson pkg-config gettext curl ca-certificates \
  libgtk-4-dev libvte-2.91-gtk4-dev libjson-glib-dev libsoup-3.0-dev \
  libpcre2-dev libyaml-dev liblua5.4-dev libglib2.0-dev \
  libglib2.0-bin libgdk-pixbuf2.0-bin glib-networking librsvg2-common adwaita-icon-theme locales \
  file patchelf squashfs-tools zsync desktop-file-utils appstream python3 openssl \
  xvfb xdotool dbus-x11 imagemagick fonts-dejavu-core fonts-noto-cjk lrzsz lua5.4 bubblewrap
python3 /source/packaging/linux/portable.py fetch /work/cache
tar -xf /work/cache/nfpm.tar.gz -C /work/cache nfpm
mkdir -p /work/tio-source /work/quickjs-source
tar -xf /work/cache/tio.tar.xz -C /work/tio-source --strip-components=1
tar -xf /work/cache/quickjs.tar.xz -C /work/quickjs-source --strip-components=1
if [ -f /work/tio-build/build.ninja ]; then
  meson setup --reconfigure /work/tio-build /work/tio-source --prefix=/usr --buildtype=release
else
  meson setup /work/tio-build /work/tio-source --prefix=/usr --buildtype=release
fi
meson compile -C /work/tio-build -j 4
make -C /work/quickjs-source -j4 qjs
cmake -S /source -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DTIO_GUI_BUILD_AGENT=ON -DBUILD_TESTING=ON
cmake --build /work/build --parallel 4
python3 /source/tests/headless.py ctest --test-dir /work/build --output-on-failure
DESTDIR=/work/AppDir cmake --install /work/build
bash /source/packaging/linux/validate-metadata.sh /work/AppDir/usr
DESTDIR=/work/AppDir meson install -C /work/tio-build
install -m755 /work/quickjs-source/qjs /work/AppDir/usr/bin/qjs
mkdir -p /work/AppDir/usr/lib/locale
localedef --no-archive -i en_US -f UTF-8 /work/AppDir/usr/lib/locale/en_US.UTF-8
cc -O2 -Wall -Wextra /source/packaging/linux/bwrap-runtime.c -o /work/bwrap-runtime
python3 /source/packaging/linux/portable.py bundle /work/AppDir /work/cache/sharun
python3 /source/tests/headless.py /work/AppDir/AppRun --version
if [ "${TIO_GUI_SKIP_FINALIZE:-0}" != 1 ]; then
  bash /source/packaging/linux/finalize.sh
fi
