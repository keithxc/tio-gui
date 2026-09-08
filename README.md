# tio-gui

> A lightweight, reliable GUI for [`tio`](https://github.com/tio/tio), designed for embedded developers who need effortless serial device discovery, automatic reconnection, terminal I/O, and logging.

`tio-gui` is a Linux-first session manager and controller for `tio`. The GUI discovers devices, builds validated options, starts and stops sessions, and exposes status and logs; `tio` remains responsible for all serial I/O and reconnection behavior.

## Status

Version 0.3.4 targets x86-64 Linux with tio 3.9. Software acceptance covers real
PTY serial transport, protocol peers and GTK controls; electrical and wireless
hardware limits are recorded in [the acceptance report](docs/acceptance.md).

## Features

### Devices and connection

- Show common `/dev/ttyACM*` and `/dev/ttyUSB*` devices by default, with ACM devices first
- Optionally reveal every `/dev/tty*` device from the window menu
- Select common baud rates, including 1,500,000 baud, or enter a custom rate
- Configure data bits, stop bits, parity, flow control, and local echo per session, in that
  session's own settings expander
- Control DTR/RTS low, high or pulse and send Break through tio. Configure initial
  line levels, pulse duration and RS-485 in the session settings. Hardware and
  driver support determine whether an adapter can apply these options.
- Set the `tio` output character delay and output line delay for slow or fragile receivers
- Start and stop a `tio` session inside an embedded terminal
- Configure automatic reconnection, same/new/latest device selection, exclusions and
  optional notifications/sound. Connection status reflects the serial descriptor
  actually open in tio, with observed reconnect counts and the last disconnect reason.
  Device selection remains delegated to tio.
- Report missing devices, permissions, and process failures clearly

### Sessions

- Run several serial devices at once, one session per tab, each with its own `tio` process,
  terminal, log and settings; a background tab keeps capturing while another is in front
- `Ctrl+T` opens a session, `Ctrl+W` closes one, `Ctrl+PgUp`/`Ctrl+PgDn` and
  `Alt+1`…`Alt+9` switch between them. Drag tabs to reorder; right-click the name to rename.
- Tools → Compare sessions shows two raw logs with common filtering and pause/follow
- Connecting to a port another session already holds is refused with a plain message, and the
  device list marks those ports as in use
- Closing a connected tab or the whole window asks first and drains pending recordings
- Optionally reopen the tabs that were open last time; restored tabs are configured but do not
  connect on their own

### Connection profiles

- Save the device, serial settings, logging options, and quick buttons as a named profile
- Switch profiles, update the active one, duplicate it, or delete it from the toolbar
- Profiles also remember the matching `/dev/serial/by-id` name, so a saved entry keeps working
  after the kernel renumbers `/dev/ttyUSB*`

### Terminal

- Clear both the visible terminal and its scrollback history without sending data to the device
- Search the scrollback with `Ctrl+Shift+F`, including case-sensitive and regular-expression matching
- Follow new output while the view sits at the bottom, and pause following once you scroll up;
  a button reports new output and returns to the bottom
- Selecting text also pauses following in both the terminal and highlight view. Each view
  keeps its own position; new data keeps arriving below while you inspect or copy earlier
  output. Scroll to the bottom, press Enter, or click the bottom button to resume
  (resuming clears the selection). Enter retains normal terminal / send-bar behavior.
  Ctrl+Shift+C copies the selection from the visible view. Retention remains limited to
  10,000 lines, so the oldest output eventually expires during long captures.
- Read the device, baud rate, framing, flow control, connected time, and log size from the status bar
- See received bytes, line count and throughput taken from the serial stream itself, not from
  the rendered terminal, so they stay correct with hex output or timestamps switched on

### Sending

- Send commands from a dedicated bottom input bar or interact directly with the terminal
- Browse the last 100 sent commands with `Up` and `Down`, or pick one from the history list
- Choose the line ending appended to each send: none, LF, CR, or CR+LF
- Configure four persistent quick-send buttons with text or HEX payloads, control-byte
  insertion, line endings, CRC-8/SMBUS, CRC-16/MODBUS or CRC-32/ISO-HDLC, and
  a byte preview before saving. CRC is computed before the line ending; multi-byte
  checksums are appended little-endian. Invalid input prevents saving.
- Optionally delay a quick send by up to 60 seconds; disconnect cancels it
- Import/export button groups independently of device and logging settings
- Quick sends use the raw socket so binary values, including Ctrl-T, arrive intact
- Build and save named send sequences with reorderable text/HEX steps, line endings,
  CRC and post-send delays. Run once or loop; pause, resume or stop per session.
  Disconnect stops the sequence. Closing the sequence editor leaves it running.
- Toggle `tio --output-mode hex16` from a persistent bottom-right HEX control

### Logging

- Enable timestamps and session logging to a user-selected directory
- Name the log file explicitly or let tio-gui generate a dated name per session
- Choose a device-first or date-first filename rule, or build a custom filename with
  `{device}`, `{date}`, and `{time}` placeholders and inspect the result before connecting
- Append to an existing log or start a new one, and optionally strip control characters
- Open the log directory from the session's settings and see the current log file and its size
- Get a one-time warning when the current log passes a configurable size

### Interface

- Keep the main workspace focused on frequent switches: window-wide appearance, language,
  backup and project details sit in the top-left menu, and everything describing one
  connection sits in that session's own expander
- Follow the desktop theme by default, or explicitly select the light or dark appearance
- Export complete INI backups or portable profiles/buttons/sequences with machine paths,
  device identities and send history removed. Import requires all sessions disconnected.
- Open About for application/tio/GTK/VTE/kernel versions, license and project links;
  copy diagnostics without hostname, local paths or captured device content.
- Select any timestamp format supported by `tio`: 24-hour clock, time since start,
  delta since the previous line, ISO 8601, or Unix epoch, with a live preview
- Open the GitHub project and inspect the application version directly from the settings menu
- Check the latest GitHub Release asynchronously the first time settings are opened; when a newer
  semantic version exists, show a compact notice linking to its release download page
- Choose system default, Simplified Chinese, Traditional Chinese, English, Japanese or German.
  Core controls update immediately; reopen tools or restart to refresh all existing controls.
- Keyboard shortcuts: `Ctrl+Shift+L` clear, `Ctrl+Shift+C`/`Ctrl+Shift+V` copy and paste,
  `Ctrl+Shift+F` search, `F5` connect, `F6` disconnect. Plain control characters such as
  `Ctrl+C` are never intercepted and always reach the device
- A second launch opens a session in the running window instead of a competing one
- Install a Wayland-compatible desktop entry and branded `tio` application icon

SSH, SFTP, Windows, macOS, and a general-purpose terminal emulator are deliberately outside the first release.

## Build on NixOS

```sh
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/tio-gui
```

Build the packaged application:

```sh
nix build
./result/bin/tio-gui
```

The project version is maintained in `VERSION`. Check an installed build with
`tio-gui --version`.

Translatable strings live in `po/tio-gui.pot`. After adding or changing a string, run
`cmake --build build --target update-pot` and merge the result into each catalogue in `po/`.

The initial packaging order is Nix/NixOS first, AppImage second, and Arch Linux (PKGBUILD/AUR) third. Ubuntu/Debian and RPM-native packages are intentionally out of scope for the first public versions.

## Build on other Linux distributions

Install a C17 compiler, CMake, gettext, pkg-config, GTK4 ≥4.12, VTE for GTK4 ≥0.74,
PCRE2, libsoup 3, JSON-GLib and `tio` 3.9. File transfer uses lrzsz; analysis plugins
require bubblewrap, QuickJS and Lua 5.4. BLE requires the system BlueZ service. Then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tio-gui
```

## Architecture

The GUI launches one `tio` per session as a separate child process attached to that session's VTE
pseudo-terminal. Version 1 deliberately does not implement an alternative serial backend: it manages and controls `tio` through its public command-line behavior.

Each session also asks `tio` for a unix socket with `--socket` and attaches to it. That socket carries
the bytes as received, unaffected by `--output-mode` or `--timestamp`, and `tio`'s own messages never
appear on it. So one session has three independent streams: the pty that VTE renders, the log file
`tio` writes, and this raw tap. Anything that needs the actual bytes — received counters,
highlighting, filtering, recording and plotting — reads the tap rather than scraping the screen. A socket
path that would not fit in `sun_path` is dropped rather than passed on, because `tio` refuses to start
at all on a long one and connecting matters more than the counters.

User preferences are stored in `~/.config/tio-gui/config.ini`. The file holds general options
(including the theme) under `[general]`, the starting values for a new session under `[defaults]`,
the send history under `[send]`, one `[profile:<name>]` group per saved profile, and one `[tab:<n>]`
group per session that was open at exit. Files written by 0.1.x and 0.2.x are migrated automatically
on first start. Session logs default to the user's `Documents/tio-gui` directory.

Settings are split by what they belong to. The window menu holds what there is one of per window:
theme, language, update checks, configuration backup, the device filter and the log size warning.
Everything that describes one link — framing, flow control, delays, local echo, timestamp format,
log filename and log options — sits in that session's own expander, because each tab configures its
own connection.

One window holds every session. Starting tio-gui again opens another tab in it. Developers can set
`TIO_GUI_NON_UNIQUE=1` to run an isolated preview beside an existing window without activating or
controlling it.

The Nix development shell defaults to the `zh_TW.UTF-8` locale for the maintainer's local debugging. Source strings, documentation, packaged defaults, and GitHub communication remain English; release builds follow the user's system locale.

Enabling session logging also enables ISO 8601 line timestamps with millisecond precision. tio-gui always
passes an absolute `--log-file` path, including for automatically named logs, so it can show and monitor
the file it started. `tio` 3.9 does not provide size-based log rotation; tio-gui therefore only warns once
when a log passes the configured size. The separate raw recorder rotates closed parts safely, without truncating tio logs.

## Serial log analysis

Open **Analyze…** for per-session filtering, level counts, expandable JSON/key/value
fields and live numeric curves. Plot JSON paths, key/value names, CSV `col1`/`col2`,
or named regex captures; choose up to three comma-separated fields and click Plot.
Hide individual curves with the legend checkboxes. Selecting text or an entry
pauses following; enable Follow to catch up. Capture and tio logging continue.
Export filtered CSV includes discovered fields and protects spreadsheet formulas.

The analyzer retains at most 10,000 normalized lines / 8 MiB, limits each line to
16 KiB, and shows the latest 1000 matches. Counters cover all lines since analysis
was cleared. Numeric curves retain at most 5000 points each. Regex matching and
JSON nesting are bounded. This view is for line-oriented logs, not terminal screen
emulation or byte-exact archival.

## Recording and protocol tools

Use the session's **Recording / safe rotation** controls for timestamped, byte-exact
`.tiocap` files. Configure part size/time, retained parts and disk budget. Open a
recording or retained part from Analyze for offline playback at 0.1–32× speed;
playback never sends data to devices. Recording and analyzer retention are separate.
Socket attachment has a startup gap; recording cannot recover earlier bytes.

The session expander contains DTR/RTS/Break, RS-485, reconnect policy, Modbus RTU
and XMODEM/YMODEM/ZMODEM transfer. Transfers support cancellation and timeouts;
receive into an empty directory. XMODEM may retain final-block padding.

The **Tools** menu opens independent protocol windows:

| Tool | Implemented scope |
| --- | --- |
| TCP / UDP | TCP client or UDP peer, binary send preview, counters and analysis |
| Modbus TCP / RTU | Functions 01–06, zero-based addresses, validated responses, no automatic retry |
| MQTT | MQTT 3.1.1 over plain TCP, QoS 0/1, subscribe/publish/retain, username/password |
| CAN / DBC | SocketCAN classic/FD, extended ID, BRS/RTR; integer DBC signals and simple multiplexing |
| BLE GATT | BlueZ scan/connect, characteristic read/write and notifications; pairing in system settings |
| Compare sessions | Read-only latest 200 raw log entries per side, shared filter and pause/follow |

These are debugging tools with explicit boundaries: MQTT TLS/QoS 2, advanced DBC
multiplexing and non-Linux backends are outside this release. See
[acceptance details](docs/acceptance.md) for data limits and hardware coverage.

Analyze can save declarative parser rules and run manually reviewed Lua/JavaScript
transformations on one entry snapshot. Plugins have no device API and run in a
bounded sandbox. See [plugin API and examples](docs/plugins.md).

## Command line and acceptance

```sh
tio-gui /dev/ttyUSB0 --baud 115200         # open a new tab and connect
tio-gui --device /dev/ttyACM0 -b 250000 --no-connect
tio-gui --help
nix develop --command python3 tests/headless.py sh tests/acceptance.sh
```

The acceptance runner uses private PTYs, localhost services, a private display,
a fake BlueZ service and a separate vcan network namespace. It does not connect
to attached boards. A real BlueZ **read-only** check can additionally be run with
`TIO_TEST_BLUEZ_SYSTEM=1 .cache/build/ble-test` inside the development shell.

## Semantic highlighting

An optional line-oriented highlight view consumes the raw tap and uses these conservative defaults:

The highlight view accepts keyboard input through VTE, including Enter, arrows and control
keys. Mouse selection stays local for copying, and unfinished lines (such as shell prompts)
are displayed immediately. This is a line-oriented log view; full-screen terminal programs
and cursor-addressed screen updates still require the ordinary terminal view.

- red: `ERROR`, `FAIL`, `FAILED`, `FATAL`, `PANIC`, `CRITICAL`, `ASSERT`
- yellow: `WARN`, `WARNING`, `TIMEOUT`, `RETRY`
- green: `PASS`, `PASSED`, `OK`, `SUCCESS`, `READY`
- blue: `INFO`, `NOTICE`
- grey: `DEBUG`, `TRACE`
- accent colours: dates and durations, IP and email addresses, URLs, file paths, key/value
  keys, command options, decimal numbers, hexadecimal values or addresses, literals and brackets

Matching is case-insensitive and word-boundary aware to avoid false positives such as highlighting
`OK` inside another word. The renderer consumes the raw tap instead of modifying the terminal stream
or `tio` log output. It strips terminal control sequences and bounds its scrollback, line length and
matches per rule so malformed device output cannot grow memory without limit. User-defined
regular-expression rules can be layered on after the safe defaults.

### Process isolation

`tio-gui` only signals the exact child process that it started. It never searches for, reconfigures, or terminates `tio` sessions launched by the user or by other applications. If another session already owns a serial device, `tio-gui` reports the resulting conflict instead of interfering with that session.

## License

GPL-3.0-only. See `LICENSE`.
