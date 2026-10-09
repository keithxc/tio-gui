#!/usr/bin/env bash
# Run in a disposable distribution container with /source and /work mounted.
set -euo pipefail
format=${1:?Usage: test-native.sh deb|rpm|archlinux}
version=$(tr -d '\n' </source/VERSION)
case "$format" in
  deb) suffix=deb; install_package() { dpkg -i "$1"; }; remove_package() { dpkg -r tio-gui; } ;;
  rpm) suffix=rpm; install_package() { rpm -Uvh --replacepkgs "$1"; }; remove_package() { rpm -e tio-gui; } ;;
  archlinux) suffix=pkg.tar.zst; install_package() { pacman --noconfirm -U "$1"; }; remove_package() { pacman --noconfirm -R tio-gui; } ;;
  *) exit 2 ;;
esac
export XDG_CONFIG_HOME="/work/qa/native-$format/config"
mkdir -p "$XDG_CONFIG_HOME/tio-gui"
touch "$XDG_CONFIG_HOME/tio-gui/preserve-on-removal"
package="/work/dist/tio-gui-$version-linux-x86_64.$suffix"
install_package "$package"
test "$(/usr/bin/tio-gui --version)" = "tio-gui $version"
test -f /usr/share/applications/io.github.keithxc.tio_gui.desktop
test -f /usr/share/metainfo/io.github.keithxc.tio_gui.metainfo.xml
test -f /opt/tio-gui/usr/share/doc/tio-gui/runtime-licenses/packages.json
install_package "$package"
test "$(/usr/bin/tio-gui --version)" = "tio-gui $version"
remove_package
test ! -e /usr/bin/tio-gui
test ! -e /opt/tio-gui/AppRun
test -f "$XDG_CONFIG_HOME/tio-gui/preserve-on-removal"
echo "PASS: $format install, reinstall/upgrade, CLI, desktop metadata, removal and settings preservation"
