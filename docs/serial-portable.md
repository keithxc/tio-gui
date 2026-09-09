# Native serial edition — Windows and macOS

This edition connects directly to the operating system's serial API. It includes
GTK and its runtime libraries and does not require tio, MSYS2, Homebrew or Nix on
the computer where it is installed. Linux's original tio/VTE edition is still
available through the default Linux build.

## Installation

- Windows x86-64: run `tio-gui-0.3.6-windows-x86_64-setup.exe`. Installation is per
  user, with Start menu/Desktop shortcuts and an entry in installed applications.
- macOS Apple Silicon: open `tio-gui-0.3.6-macos-arm64.dmg` and drag tio-gui to
  Applications. The current Homebrew-based package requires macOS 26 or later.
  Intel is not included in this build.

These test packages have no paid publisher certificate. The macOS app has an
ad-hoc signature, not Apple notarization. Windows may show an unknown-publisher
prompt. On macOS, approve the trusted download in System Settings → Privacy &
Security if Gatekeeper blocks it. Do not disable system-wide security checks.

## Appearance and language

The native edition uses the same compact connection/console/send layout as the
Linux edition. Connection and logging options are collapsed; line controls are
in a menu, and Search or Ctrl+Shift+F opens the search row (Escape closes it).
The gear in the tab bar opens application settings.

Windows packages include a private Noto Sans SC fallback font (SIL OFL 1.1), so
Chinese text remains readable on minimal English installations without changing
the system font installation. Font source URLs and checksums ship with the package.

The workspace and appearance settings are built from the same UI constructors as
Linux (`src/workspace_ui.h`). Missing backend capabilities remove their actions;
shared controls keep the same order, spacing and style. OS fonts, DPI and window
decoration still follow the desktop. Quick sends sit below the send row; Customize
opens their editor. Search uses Ctrl+Shift+F, with Enter/F3 navigation.

Theme defaults to Follow system, with Light and Dark overrides applied immediately
and saved in `serial.ini`. On Windows the system choice follows the user's app
color preference, including changes while the application is open. The console
uses the Linux edition's existing dark log palette. New/legacy native installations
default to simplified Chinese; English can be selected in settings and takes effect
immediately. Changing language does not interrupt an active serial connection.

Normal client-decorated windows use matching top/bottom corner radii and clip the
content at the bottom; maximized/fullscreen windows use square corners.

## Included

- Multiple independent serial tabs; refresh detected ports or enter a port name.
- Custom baud rate, 5–8 data bits, 1/2 stop bits, none/odd/even parity, none/RTS-CTS/
  XON-XOFF flow control. Unsupported driver settings report an error.
- Text/HEX sends, none/LF/CR/CRLF endings, four editable quick sends.
- Direct console typing, UTF-8 input, arrow keys, Ctrl-C and other Ctrl-letter
  bytes. Ctrl-Shift-C/V copies/pastes; macOS also supports Command-C/V.
- UTF-8 line console with semantic highlighting, progress-line updates, HEX
  display, search, selectable text, local echo and a Follow switch.
- Ctrl+wheel adjusts the console font (6–40pt), shared by tabs and saved on exit
  or immediately when zoomed. Ctrl+Shift+F focuses search from the console;
  Enter/F3 and Shift+Enter/Shift+F3 navigate with wrap-around. Search supports
  case/regex, highlights matches and shows current/total while pausing Follow.
- Byte-exact received logs, appended to the selected file; changing display to
  HEX does not change the logged bytes. TX counters count confirmed writes.
- Same-port reconnect every second; queued sends are discarded after disconnect
  and never replayed automatically. Opening the app restores tabs disconnected.
- DTR/RTS low/high controls; manual RTS is disabled by the backend with RTS/CTS.
- Send a 250 ms Break, with driver support checked when requested.
- Named profiles; save a name and use “Open in new tab” to reopen it.

## Boundaries

This is a line-oriented serial console, not a full VT terminal. Full-screen
programs such as vim/top are not supported. CAN, BLE, network/protocol tools,
script plugins, file-transfer protocols, RS-485 automation, DTR/RTS pulses,
timestamp formatting, send sequences and Linux profile/device-ID migration are
outside this edition. Logs contain received bytes only, without timestamps.
Quick sends have their own line endings, without CRC or delay.

Opening a serial port may assert DTR/RTS according to the driver. Hardware flow
control, nonstandard baud rates, framing and line levels require real hardware
verification. Reconnect uses the same COM number or `/dev/cu.*` path, not a USB
serial-number identity; select the port again if its name changes.

The display keeps at most 10,000 lines / 16 KiB per line. The worker's receive
queue is bounded to 8 MiB and stops the session on overflow. A log write failure
stops the session and reports the failure. Never use this debugging tool as a
guaranteed lossless high-speed acquisition system without validating the rate.

Settings are in `serial.ini` under GLib's user configuration directory in the
`tio-gui` folder, separate from Linux's `config.ini`. Uninstalling preserves
settings and user-selected logs.

## Hardware acceptance

1. Attach the USB serial adapter to the VM (Windows) or Mac and install its driver
   if needed. Click Refresh and select the port. Start with 115200, 8N1, no flow.
2. Check interactive command input, Enter, Ctrl-C, arrow keys and UTF-8 output.
3. Send HEX `00 11 13 14 7F 80 FF` to a loopback/known peer and verify exact bytes.
4. Enable a log, receive data, switch HEX on/off, disconnect and inspect the file.
5. Unplug/replug with Auto reconnect enabled. Verify received data resumes and
   commands queued before unplug are not replayed.
6. Open two tabs on different adapters; close a connected tab and cancel/accept
   the prompt. Check the other tab continues receiving.
7. Check your required baud rates, parity, RTS/CTS and DTR/RTS on actual hardware.
8. Install and launch from a path containing spaces, then uninstall. Confirm logs
   and settings remain and that no development tools are required to launch.

## Rebuilding

On macOS, install build dependencies `brew install gtk4 pcre2 pkgconf cmake ninja gettext python`:

```sh
export PATH="$(brew --prefix gettext)/bin:$PATH"
cmake -S . -B build -G Ninja -DTIO_GUI_SERIAL_ONLY=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
cmake --build build
ctest --test-dir build --output-on-failure
python3 packaging/package-macos.py build dist
```

For Windows cross builds on Linux, provide MinGW gcc/objdump, pkg-config, CMake,
Ninja, NSIS, glib-compile-schemas, Python and zstd:

```sh
python3 packaging/fetch-mingw.py .cache/mingw
python3 packaging/build-windows.py .cache/mingw .cache/serial-windows dist
```

On NixOS, `nix-shell packaging/windows-shell.nix` provides those tools and the
MinGW compiler's additional mcfgthread runtime. The original Linux build remains
available with `-DTIO_GUI_SERIAL_ONLY=OFF`.

The fetched package manifest records versions, URLs and verified SHA256 hashes.
Packaging fails if a non-system DLL dependency cannot be bundled. The Mac packager
rewrites and audits dynamic-library paths before signing and creating the DMG.
Software tests cover PTY binary transport, disconnection and same-path reconnect
on Unix, plus invalid settings, absent ports, payloads and display parsing.

## macOS acceptance — 2026-09-09

Continued the uncommitted native serial edition from the Linux development
machine and validated it on Apple Silicon with AppleClang and Homebrew GTK 4.22.4.

- Release build and all four CTest suites passed: native serial (five cases),
  payload, highlighter and serial GUI.
- PTY tests cover all 256 byte values in both directions, disconnection,
  same-path reconnect, inherited flow/parity settings, HEX + CRLF sending,
  byte-exact logging across display changes, profiles and failed-open recovery.
- Fixed inherited input flags so selecting no software flow control clears
  IXON/IXOFF/IXANY, and no parity clears stale INPCK/IGNPAR.
- Fixed controls remaining locked after a failed/non-reconnecting session.
- Added IM focus lifecycle and printable-key fallback for native GTK contexts;
  GUI regression covers ordinary keys, Ctrl-C and UTF-8 commits.
- Installed the bundled app in `/Applications/tio-gui.app`, verified its ad-hoc
  signature, launched it, and exercised a PTY peer through the installed GUI.
  The send field delivered `mac-serial-ok` plus CR; direct keyboard input
  delivered exact bytes `61 62 63 0D 03` (abc, Return, Ctrl-C).
- Visually verified readable default text on the dark console after correcting
  the text-view foreground style. The packager audits bundled library paths.

No USB serial adapter was connected. Electrical loopback, real unplug/replug,
custom baud rates, parity and DTR/RTS/RTS-CTS remain hardware acceptance items.
Chinese input-method composition and Windows installation were not manually
validated in this Mac session. PTY checks do not establish hardware acceptance.
