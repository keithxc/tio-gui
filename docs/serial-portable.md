# Native serial editions

## macOS 0.4.0

Download `tio-gui-0.4.0-macos-arm64.dmg`, open it and drag **tio-gui.app** into
Applications. The ZIP contains the same application. Requires Apple Silicon and
macOS 26 or newer. Intel is not included. GTK, libvterm, lrzsz, Lua and QuickJS
are bundled; the destination Mac needs no Homebrew, Nix or development tools.

The release application and disk image use Developer ID Application signing,
hardened runtime and Apple notarization. Keep the existing bundle identity
`io.github.keithxc.tio_gui.serial`; this is a direct-download application.

### Serial workspace

The compact connection, options, search, console, send and quick-button rows use
the same constructors as Linux. The sequence editor is shared source. Native OS
window decoration and font metrics may differ.

- Independent, reorderable tabs; right-click or double-click a name to rename it.
- Port discovery, manual paths, standard/custom baud, 5–8 bits, 1/2 stops,
  none/odd/even parity, none/RTS-CTS/XON-XOFF flow control.
- Direct/new/latest device selection, path exclusion regex, reconnect and optional
  connection notifications/sound. Unique USB serial identities are obtained through
  IOKit; adapters without them retry the same path. Ambiguous identities are not
  silently assigned to another adapter.
- DTR/RTS defaults, low/high/pulse controls, Break, character and line send delays.
- Text/HEX sends, line endings, history, four editable quick buttons with CRC-8,
  CRC-16/MODBUS or CRC-32, delay, control-character inserts and exact byte preview.
  Button groups and named sequences import/export through compatible settings.
- The Linux sequence editor: reorder steps, save/load/delete, loop, pause/resume,
  stop, per-step CRC/endings/delay. Disconnect cancels pending work; it is never
  replayed on reconnect. Manual sends are blocked while a transfer owns the port.
- Semantic highlighted logs and a libvterm terminal view, with ANSI colors,
  cursor addressing, alternate screen, UTF-8, application cursor keys and
  bracketed paste. Ctrl+wheel changes font size; search supports case/regex,
  previous/next matches and a counter. Search operates on the highlighted log.
- Received-file logging with append/replace, optional ANSI stripping and five
  timestamp styles. Old native installations retain append/raw-log behavior.
  Binary `.tiocap` recording always preserves exact RX/TX bytes and input/events,
  with size/time rotation, retention, disk bounds and replay. Closing waits for
  pending recording writes.
- The same log analyzer, level/regex filtering, field extraction, CSV export,
  graphs, replay, custom highlight rules and side-by-side session comparison.
- XMODEM-CRC/YMODEM/ZMODEM send and receive, cancellation and timeout. Receives
  require an empty directory. Terminal input is excluded while a transfer owns
  the stream; final serial writes are drained before releasing it.
- Modbus RTU function codes 1–6 on the active serial session, using the same
  request/validation UI and engine as Linux.
- Lua and JavaScript analysis plugins in separate sandboxed processes with a
  two-second deadline, 64 KiB source/input/output limits and resident-memory
  monitoring. No serial API is exposed to plugins.
- Named profiles, full/portable configuration import/export, migration from
  Linux configuration files, optional disconnected tab restoration, Chinese/
  English and system/light/dark appearance. Import requires disconnected sessions
  and keeps a `.before-import` backup.

Shortcuts follow Linux: Ctrl+T/W, Ctrl+PageUp/PageDown, Alt+1…9, F5/F6,
Ctrl+Shift+L/C/V/F and F3/Shift+F3. macOS also accepts Command+T/W/C/V.
Plain Ctrl+C still sends the interrupt byte to the serial peer.

### Platform and verification boundaries

RS-485 kernel configuration is a Linux driver interface. On macOS use an adapter
with hardware automatic direction; this application does not emulate timing-critical
RS-485 direction in the UI. Driver names and tio-specific device IDs are Linux
concepts; native selection uses path regex and USB identity. Electrical framing,
custom baud rates, DTR/RTS levels, pulses, real USB unplug/replug and identity
matching still need a physical adapter test. No adapter was attached for this release.

The terminal is implemented with libvterm, not VTE. Its primary-screen scrollback
is bounded to 2,000 lines/512 KiB; the highlighted log retains its separate larger
limit. Terminal mouse reporting and OS clipboard escape commands are not exposed.
CAN, BLE and network-only tools are outside this **serial** release.
Chinese IME candidate interaction requires a separate manual check; UTF-8 commit
and literal-backslash transport are covered by software tests.

Receive queues and display history are bounded. Overflow or failed log writes
stop the session rather than silently claiming lossless capture. Validate sustained
rates on your own adapter and storage before using this for acquisition.

### Rebuilding and verification

All third-party build tools and runtime libraries are managed by the Nix
`devShell`; the maintainer's `mynix#tio-gui` imports this same shell definition
with mynix's locked nixpkgs. Apple Clang/SDK, codesign, notarytool and hdiutil
remain system tools. `TIO_MACOS_RUNTIME` supplies the packager with exact Nix
runtime paths and source/license metadata. The resulting app contains relocated
libraries, with no Nix or Homebrew runtime requirement. The packager retains a
Homebrew fallback for other builders; the 0.4.0 release is built through mynix.

```sh
# Uses the project's pinned Nix packages. On the maintainer's machines:
# nix develop /path/to/mynix#tio-gui
nix develop
cmake -S . -B build-nix -G Ninja -DTIO_GUI_SERIAL_ONLY=ON \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
cmake --build build-nix
ctest --test-dir build-nix --output-on-failure
python3 packaging/package-macos.py build-nix dist \
  --identity "$TIO_SIGN_IDENTITY" --notary-profile "$TIO_NOTARY_PROFILE"
```

Signing credentials stay in the local keychain. Without the signing arguments,
the packager creates an ad-hoc development package. Release publishing requires
Developer ID verification, accepted notarization, stapled tickets, Gatekeeper
checks, clean-environment bundle checks and uploaded checksum verification.
The final verification record is in [macOS 0.4.0 acceptance](macos-0.4.0-acceptance.md).

## Windows 0.3.7

The existing per-user NSIS installer remains available in the 0.3.7 release.
Its native frontend is retained separately; this macOS release does not change
Windows UI scope or replace Windows assets. It includes serial framing, direct
input, text/HEX sends, four basic quick buttons, raw receive logs, profiles,
search, zoom, Chinese/English, themes and same-COM reconnect. It remains a line
console without the additional macOS serial tools listed above. Windows packages
have no publisher certificate. Real Windows driver/hardware acceptance remains
separate from the existing Wine/package smoke checks.

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
