# macOS 0.4.0 acceptance

Date: 2026-09-14. Source: `v0.4.0`. Host: Apple Silicon, macOS 26.6.2.
This report distinguishes software tests from physical-device validation.

## Software checks

- Release build through `mynix#tio-gui` with Nix-managed build/runtime
  dependencies, Apple Clang, GTK 4.22.4, libvterm 0.3.3 and JSON-GLib 1.10.8.
  The bundle includes Lua 5.4.7 and QuickJS 2026-06-04.
- Native CTest suite: 12/12 passed. Covers serial PTY transport and disconnect,
  payload/CRC, highlighting, the serial workspace, sequence persistence and
  ordering, quick presets, log model, capture/replay, plugin isolation, transfers,
  integrated tools and terminal behavior.
- Transfer acceptance: XMODEM-CRC, YMODEM and ZMODEM, send and receive, each with
  an 8,192-byte all-byte-values payload compared exactly at the receiving peer.
- Integrated UI/PTY checks: delayed sends and disconnect cancellation, literal
  backslashes and control characters, sequence pause/resume, three successive
  Modbus RTU requests, split ANSI log formatting, import refusal while connected,
  backup before import, and recording drain/replay after quit.
- Terminal checks: ANSI colors/cursor positioning, alternate screen, UTF-8/wide
  text, bounded history, application cursor keys, bracketed paste, follow-on-Enter
  and visible search results. Screenshots of the workspace and tool windows were
  inspected for clipping and layout.
- AddressSanitizer/UndefinedBehaviorSanitizer: native serial, integrated tools UI
  and native terminal tests passed. Leak detection was disabled for the GTK run.
- Linux regression in a separate temporary workspace: seven common-code CTest
  targets passed (capture, log model, connection state, serial options, sequence,
  quick presets and payload). The Linux GUI test had eight passes and four
  explicit skips: dedicated real-tio sequence/line/reconnect harnesses and the
  optional localized-layout check were not run in this regression.

## Distributed application

- Runtime libraries and the `sz`, `rz`, Lua and QuickJS helpers are bundled.
  Load commands were checked for accidental Homebrew/Nix dependencies.
- Developer ID Application signing with hardened runtime; application and DMG
  accepted by Apple's notarization service, with tickets stapled and validated.
  Codesign verification and Gatekeeper assessment passed.
  App submission: `c8e48185-05bb-45be-898f-bf07bcf79089`.
  DMG submission: `38541bdb-4669-4a7b-86e0-4475f18e5568`.
- The extracted distribution was opened from a path containing spaces using
  isolated settings and a PTY. Access to `/nix/store` and Homebrew roots was
  explicitly denied by a test sandbox. With a system-only PATH it received and logged
  2,065 exact binary bytes and sent `bundle-smoke` plus CR through its GUI.
- Bundled Lua/QuickJS passed plugin isolation/timeouts; bundled lrzsz passed
  all six binary transfer cases after relocation.
- Release asset checksums are published in `SHA256SUMS`.

## Not covered by these results

The release targets **Apple Silicon / macOS 26+**. Intel and older macOS are not
validated. No physical USB adapter was attached: custom baud, framing/electrical
levels, RTS/CTS, Break, DTR/RTS/pulses, unplug/replug and USB identity matching
remain hardware acceptance items. Chinese IME candidate selection/cancellation
requires a manual check beyond automated UTF-8 transport.

macOS has no Linux RS-485 kernel ioctl; use a hardware auto-direction adapter.
Terminal mouse reporting and clipboard escape commands are not exposed. CAN,
BLE and network-only tools are outside this serial release. Windows retains its
existing frontend; this release does not certify new Windows behavior.

No existing application installation or running serial session was replaced.
