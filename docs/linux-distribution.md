# Linux distribution and package preparation

Updated 2026-10-03: add desktop metadata validation and distribution-library CI
before submitting packages to software repositories.

The Linux edition uses GTK 4, VTE and the external `tio` process. Distribution
coverage depends on library versions, CPU architecture, serial permissions and
the packaging format. A successful build on one distribution does not establish
support for every Linux release.

## Current delivery paths

| Path | What is in this repository | Remaining acceptance |
| --- | --- | --- |
| Native source build | CMake install rules, desktop entry, icons, translations and AppStream metadata | Build and test against each distribution's libraries and runtime tools |
| Nix | Flake and pinned dependencies | Validate each exposed architecture on its native Linux host |
| AppImage | Existing x86-64 recipe and `tests/appimage_acceptance.py` | Test on clean non-Nix hosts, including systems without FUSE and with restricted user namespaces |
| Nixpkgs | Upstream source and packaging information | Prepare a Nixpkgs-native expression with a fixed source hash and run its review checks |
| AUR | Source installation instructions | Prepare and test a `PKGBUILD` and `.SRCINFO` in an Arch clean chroot |
| Flathub | Stable application ID and validated upstream MetaInfo | Prove device access and plugin sandbox behavior, then create and validate a manifest |
| Debian/Fedora repositories | Source installation and license information | Distribution-specific recipes, dependency review and sponsor/reviewer acceptance |

This document is not evidence that any downstream repository has accepted the
application. The project remains GPL-3.0-only; the newly authored AppStream
metadata is CC0-1.0 so software catalogs can redistribute it.

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

The Nix-based AppImage currently needs Linux user namespaces; the
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
   and the built app before submission. No Flatpak manifest is supplied yet.

Do not place employer code, serial captures, confidential paths or customer
device identifiers in screenshots, package tests or public issue reports.
