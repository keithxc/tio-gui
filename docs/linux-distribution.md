# Linux distribution and package preparation

Updated 2026-10-09: add checksum-locked, self-contained Linux release packaging
and corresponding runtime sources. Downstream submissions remain independent.

The Linux edition uses GTK 4, VTE and the external `tio` process. Distribution
coverage depends on library versions, CPU architecture, serial permissions and
the packaging format. A successful build on one distribution does not establish
support for every Linux release.

## Current delivery paths

| Path | What is in this repository | Remaining acceptance |
| --- | --- | --- |
| Native source build | CMake install rules, desktop entry, icons, translations and AppStream metadata | Build and test against each distribution's libraries and runtime tools |
| Nix | Flake and pinned dependencies | Validate each exposed architecture on its native Linux host |
| AppImage | x86-64 static type-2 runtime, bundled private loader/libraries and fonts | Physical USB, Wayland and hardware-specific checks remain separate |
| DEB/RPM/Arch binary | Same relocatable runtime under `/opt/tio-gui`, package-managed launcher and desktop metadata | Not native distribution source packages or repository acceptance |
| Portable tarball | Extract `AppDir` and run `AppDir/AppRun` | No automatic desktop or serial permissions changes |
| Flatpak bundle | Sideloadable x86-64 Freedesktop 25.08 package | Physical tty access, BLE and nested plugin sandbox need dedicated acceptance |
| Nixpkgs | Upstream source and packaging information | Prepare a Nixpkgs-native expression with a fixed source hash and run its review checks |
| AUR | Source installation instructions | Prepare and test a `PKGBUILD` and `.SRCINFO` in an Arch clean chroot |
| Flathub | Stable application ID and validated upstream MetaInfo | Prove device access and plugin sandbox behavior, then create and validate a manifest |
| Debian/Fedora repositories | Source installation and license information | Distribution-specific recipes, dependency review and sponsor/reviewer acceptance |

This document is not evidence that any downstream repository has accepted the
application. The project remains GPL-3.0-only; the newly authored AppStream
metadata is CC0-1.0 so software catalogs can redistribute it.

## Install release packages

Download files and `SHA256SUMS.txt` from the [0.4.3 release](https://github.com/keithxc/tio-gui/releases/tag/v0.4.3).
Linux desktop binaries are **x86-64 only**; source CI also covers ARM64, which
does not make these binary packages ARM64 builds.

```sh
# Ubuntu/Debian; Fedora; Arch respectively (choose one):
sudo apt install ./tio-gui-0.4.3-linux-x86_64.deb
sudo dnf install ./tio-gui-0.4.3-linux-x86_64.rpm
sudo pacman -U ./tio-gui-0.4.3-linux-x86_64.pkg.tar.zst

# AppImage or portable tarball; no root needed:
chmod +x tio-gui-0.4.3-linux-x86_64.AppImage
./tio-gui-0.4.3-linux-x86_64.AppImage --appimage-extract-and-run
tar -xf tio-gui-0.4.3-linux-x86_64.tar.xz
./AppDir/AppRun

# Sideloaded Flatpak, not a Flathub submission:
flatpak install --user ./tio-gui-0.4.3-linux-x86_64.flatpak
flatpak run io.github.keithxc.tio_gui
```

The native binary packages install the same portable payload under
`/opt/tio-gui`, plus `/usr/bin/tio-gui` and system desktop metadata. They do not
change serial groups, ACLs, user configuration or security policy. Upgrades and
removal use the package manager; removal preserves user settings.

The sideloaded Flatpak uses Freedesktop Platform 25.08 and grants X11/Wayland,
IPC, network, device access, BlueZ system-bus access and the Documents directory.
Device access is needed by this application's existing tty implementation and
is broader than a USB portal. No host filesystem or host-spawn escape is granted.
The nested analysis sandbox may be denied by Flatpak or host policy; failure
does not run the plugin unsandboxed. Real USB/BLE and portal behavior are not
established by a startup test.

Release packaging starts in a pinned Ubuntu 24.04 container. `tools.json` locks
sharun, the static AppImage runtime, appimagetool, nFPM, tio and QuickJS by SHA-256;
continuous URLs have checksum-matched mirrors, never unchecked fallbacks. The
runtime includes its loader/glibc, GTK/VTE dependencies, GIO/pixbuf modules,
schemas, MIME data, a UTF-8 locale, fonts, tio 3.9, lrzsz, Lua, QuickJS and bubblewrap. No private runtime
library path is exported into host commands. Bubblewrap still requires host
user-namespace support for **plugins**, not for application startup.

Copyright notices and exact Ubuntu binary/source versions are shipped below
`usr/share/doc/tio-gui/runtime-licenses`. The matching
`linux-runtime-sources.tar.xz` asset includes Ubuntu source archives with patches,
the application, tio and QuickJS sources. Nix packages retain their independently
pinned closure and do not use this binary payload.

New Linux workspaces follow the system language. Existing explicit language
preferences are preserved, including Chinese under a minimal `C` environment.
GTK initialization must not override the application's message locale.

To validate before producing archives, set `TIO_GUI_SKIP_FINALIZE=1` when running
`package.sh`, run `tests/portable_acceptance.py` through `tests/headless.py`
against `AppDir/AppRun` on a minimal offline host, then run `finalize.sh` in the
same builder. `test-native.sh` checks installation, replacement and removal in
disposable distribution containers. Those are not commands for the user's host.

## Source package requirements

Build tools: C17 compiler, CMake 3.20 or later, Ninja or Make, pkg-config and GNU
gettext (`msgfmt`). Linux library requirements come from `CMakeLists.txt`:

| Library | pkg-config module | Minimum |
| --- | --- | --- |
| GTK 4 | `gtk4` | 4.12 |
| VTE for GTK 4 | `vte-2.91-gtk4` | 0.74 |
| PCRE2 | `libpcre2-8` | As provided by the distribution |
| libsoup | `libsoup-3.0` | 3.0 |
| JSON-GLib | `json-glib-1.0` | 1.6 |

The full Linux runtime was validated with `tio` 3.9. Treat that as the baseline
for package acceptance; compiling the frontend does not verify an older `tio`
version. Include or depend on `tio`, and make optional tools discoverable on
`PATH`: `lrzsz` for transfers, and `bubblewrap`, QuickJS and Lua 5.4 for analysis
plugins. BLE additionally needs the host BlueZ service. Missing optional tools
must not prevent the serial workspace from opening.

On Ubuntu 24.04, the build and metadata dependencies used by CI are:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config gettext \
  libgtk-4-dev libvte-2.91-gtk4-dev libpcre2-dev libsoup-3.0-dev \
  libjson-glib-dev appstream desktop-file-utils dbus-x11 xvfb xauth python3
```

This installs build dependencies, not a verified `tio` runtime. Check the actual
runtime version separately with `tio --version`. Older distribution releases
whose GTK/VTE packages fall below the minimum need a compatible packaged
runtime or updated dependencies; lowering the CMake version checks alone is
not sufficient.

Use standard staged installation when preparing a package:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DTIO_GUI_SERIAL_ONLY=OFF \
  -DTIO_GUI_BUILD_AGENT=ON -DBUILD_TESTING=ON
cmake --build build --parallel 2
dbus-run-session -- xvfb-run -a ctest --test-dir build --output-on-failure
DESTDIR="$PWD/build/stage" cmake --install build
sh packaging/linux/validate-metadata.sh build/stage/usr
```

Install the application through the distribution's package manager. Do not run
it as root or install setuid helpers to bypass serial-device permissions. Device
access must follow the host's existing groups, ACLs or device rules; group names
are not universal across distributions.

## Desktop integration

Keep `io.github.keithxc.tio_gui` consistent across the application, desktop file,
icons and MetaInfo. CMake installs these below the chosen prefix:

```text
bin/tio-gui
share/applications/io.github.keithxc.tio_gui.desktop
share/metainfo/io.github.keithxc.tio_gui.metainfo.xml
share/icons/hicolor/<size>x<size>/apps/io.github.keithxc.tio_gui.png
share/locale/<language>/LC_MESSAGES/tio-gui.mo
```

With `TIO_GUI_BUILD_AGENT=ON`, installation also includes
`bin/tio-serial-agent` and the remote gateway files below
`share/tio-gui/remote/`. Python 3 is needed to run the gateway and its protocol
tests; the native agent itself links only to GIO and JSON-GLib.

Run `sh packaging/linux/validate-metadata.sh` for the source files or pass a
staged prefix as shown above. The script uses the distribution's
`desktop-file-validate` and `appstreamcli`, fails if either tool is missing, and
checks installed icon sizes when a prefix is supplied. Validation is offline;
it does not verify that remote project URLs are reachable.

Keep AppStream release versions, dates and links tied to actual published
releases. Add screenshots from a clean, non-confidential demo session before a
software-store submission. Passing basic AppStream validation does not replace
the additional [Flathub MetaInfo requirements](https://docs.flathub.org/docs/for-app-authors/metainfo-guidelines).

## CI and acceptance boundaries

`.github/workflows/linux.yml` defines native Ubuntu 24.04 x86-64 and ARM64 jobs
using distribution development libraries. Each job builds the Linux tio
edition and the headless serial agent, runs CTest (including the agent protocol
tests), stages installation and validates desktop metadata. The
runner labels are listed in the [GitHub runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).

Adding this workflow does not mean that either job has already passed. CI does
not currently exercise a real USB adapter, `tio` transport, AppImage startup,
Bluetooth, CAN, file transfers, or a Flatpak sandbox. Use the existing
[acceptance procedures](acceptance.md) for those checks. Record distribution,
architecture, desktop/session type, dependency versions and the exact package
checksum with each result. Include serial open/close, reconnect after unplug,
binary send/receive and a long-running capture on real hardware before claiming
device acceptance.

For AppImage acceptance, run the existing test in a private display with its
dependencies available:

```sh
python3 tests/headless.py python3 tests/appimage_acceptance.py /path/to/tio-gui.AppImage
```

The developer-only Nix-based AppImage currently needs Linux user namespaces; the
`--appimage-extract-and-run` option avoids a FUSE dependency but does not remove
that namespace requirement. A host that forbids those namespaces needs another
delivery path. Do not automatically change its security policy.

## Downstream submissions

Start with a stable source tag and checksum, reproducible build instructions,
the license, a dependency list and an acceptance record. Track each submission
separately from upstream release publication.

1. **Nixpkgs:** adapt the derivation into the Nixpkgs package structure, supply
   `meta` including license, homepage, maintainers and tested platforms, and
   preserve runtime wrapping. Use a fixed-output source fetch instead of
   importing this entire flake. Follow the
   [Nixpkgs contribution manual](https://nixos.org/manual/nixpkgs/stable/#chap-submitting-changes).
2. **Arch/AUR:** build from a tagged source archive, express runtime versus
   optional dependencies, generate `.SRCINFO`, and validate in a clean chroot.
   AUR is a separate submission channel rather than a GitHub package PR. Follow
   the [Arch submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines).
3. **Flathub:** first test serial enumeration, opening `/dev/ttyUSB*` and
   `/dev/ttyACM*`, unplug/reconnect, BlueZ access if offered, user-selected log
   files, and the nested bubblewrap plugin path inside the proposed sandbox.
   Flatpak USB access does not by itself demonstrate that this application's
   existing tty-device code works. Review the actual requested permissions
   against the [Flatpak sandbox documentation](https://docs.flatpak.org/en/latest/sandbox-permissions.html),
   provide clean screenshots, and run the Flathub linter against both metadata
   and the built app before submission. The sideload bundle recipe is not a
   Flathub manifest and has not been submitted there.

Do not place employer code, serial captures, confidential paths or customer
device identifiers in screenshots, package tests or public issue reports.
