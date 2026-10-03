# Windows development and validation

Updated 2026-10-03: add native Windows CI and separate the current serial frontend
from the remaining desktop feature-parity and hardware work.

## Current scope

Windows builds `src/serial_windows_main.c`, the native serial frontend retained
from 0.3.7. It supports port discovery, framing, same-COM reconnect, direct input,
text/HEX sends, four basic quick buttons, raw receive logs, profiles, search,
zoom, Chinese/English and themes. See [native serial editions](serial-portable.md)
for the available packages and their limits.

The richer `src/serial_main.c` frontend is currently selected only for macOS and
the optional native Linux build. Windows does not yet include its libvterm
terminal, sequence editor, capture/replay, analyzer, Modbus, file transfers or
sandboxed analysis plugins. Do not advertise desktop feature parity until those
paths are implemented and tested on Windows.

## Native Windows build

Install [MSYS2](https://www.msys2.org/) and open its **UCRT64** terminal. Use the
matching MinGW CMake and dependencies; MSYS2's POSIX CMake builds a different kind
of application. The upstream [CMake guide](https://www.msys2.org/docs/cmake/) and
[CI example](https://github.com/msys2/setup-msys2/blob/master/examples/cmake.yml)
describe this environment.

```sh
pacman -Syu
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-gtk4 \
  mingw-w64-ucrt-x86_64-pcre2 \
  mingw-w64-ucrt-x86_64-json-glib \
  mingw-w64-ucrt-x86_64-gettext-tools \
  mingw-w64-ucrt-x86_64-python \
  mingw-w64-ucrt-x86_64-openssl

cmake -S . -B build-windows -G Ninja \
  -DTIO_GUI_SERIAL_ONLY=ON -DTIO_GUI_BUILD_AGENT=ON \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows --parallel 2
ctest --test-dir build-windows --output-on-failure --timeout 60
./build-windows/tio-gui.exe --version
./build-windows/tio-gui.exe
```

If the first update asks you to close the terminal, reopen UCRT64 and complete
the update before installing the remaining packages. This is a development
build: run it from UCRT64 so the matching DLLs are available. Copying the `.exe`
alone does not create a portable package.

For an agent-only build without GTK, use a separate build directory:

```sh
cmake -S . -B build-agent -G Ninja \
  -DTIO_GUI_BUILD_AGENT=ON -DTIO_GUI_BUILD_DESKTOP=OFF \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-agent --parallel 2
ctest --test-dir build-agent --output-on-failure --timeout 60
./build-agent/tio-serial-agent.exe --version
```

This target needs GLib/GIO and JSON-GLib at runtime. Its stdin/stdout protocol is
for the remote gateway; it does not open a network listener itself. Agent tests
exercise the protocol on Windows, while PTY transport cases run only on POSIX
hosts. USB communication still requires Windows driver and hardware validation.

`.github/workflows/windows.yml` builds both the desktop frontend and serial agent,
then runs CTest directly on a GitHub-hosted Windows runner for pushes and pull
requests. It records exact installed dependency versions and CMake/CTest
diagnostics. The existing serial backend tests on Windows cover invalid
configuration and an absent COM port;
payload and highlighter tests cover their shared software logic. A green run is
not evidence of successful communication with a physical serial adapter.

## Existing installer pipeline

`packaging/fetch-mingw.py`, `packaging/windows-shell.nix` and
`packaging/build-windows.py` form the existing Linux-to-Windows x86-64 NSIS
pipeline. It records dependency checksums and recursively bundles DLL imports.
`tests/windows_package_smoke.py` checks installation, relocation, font resources,
software tests, application startup, normal shutdown and uninstall under Wine.

The pipeline currently uses the older **MINGW64/MSVCRT** runtime. MSYS2
[deprecated MINGW64 in March 2026](https://www.msys2.org/docs/environments/).
The native CI uses the recommended **UCRT64** environment. Before migrating the
installer, move its compiler runtime, downloaded dependency repository and DLL
staging together; do not combine UCRT64 libraries with the old MINGW64 build.
Re-run installer/relocation tests and real Windows acceptance after migration.

### Development checks on 2026-10-03

An isolated Linux x86-64 build using MinGW GCC 15.3, GTK 4.22.4, GLib 2.88.3 and
JSON-GLib 1.10.8 successfully compiled the retained desktop frontend, the serial
agent and the three Windows C test executables. These checks used the existing
MINGW64 sysroot. The UCRT64 workflow passed `actionlint` validation; its first
native Windows runner execution is still pending. No new installer or hardware
acceptance is claimed by this build check.

## Remaining checkpoints

1. Share terminal, sequence, capture/replay, analysis and Modbus code with the
   native desktop frontend. Add Windows GUI regression coverage before removing
   the retained frontend.
2. Implement portable transfer and plugin backends. The current transfer bridge
   uses Unix-domain sockets and POSIX file descriptors; the Linux plugin runner
   uses seccomp. Selecting `serial_main.c` in CMake alone is insufficient.
3. Validate the built installer on Windows, including a standard-user account,
   paths containing spaces and Chinese characters, installation/upgrade/removal,
   light/dark themes, DPI scaling, clipboard and Chinese IME input.
4. Run byte-exact TX/RX loopback with a physical adapter, including all 256 byte
   values, sustained traffic and log-write failure. Test framing and custom baud
   rates against hardware that supports them.
5. Check USB removal/reinsert, COM number changes, two identical adapters, sleep
   and wake, busy-port errors, DTR/RTS/Break and RTS/CTS. Record the Windows build,
   adapter model/driver, application revision and observed results.
6. Prepare the final signed or explicitly unsigned package and its immutable
   checksums before submitting package-manager manifests. CI diagnostics are not
   release assets and do not establish Windows ARM64 or older Windows support.
