# tio-gui

> A lightweight, reliable GUI for [`tio`](https://github.com/tio/tio), designed for embedded developers who need effortless serial device discovery, automatic reconnection, terminal I/O, and logging.

`tio-gui` is a Linux-first session manager and controller for `tio`. The GUI discovers devices, builds validated options, starts and stops sessions, and exposes status and logs; `tio` remains responsible for all serial I/O and reconnection behavior.

## Status

Early development. The first release targets x86-64 Linux.

## Initial scope

### Devices and connection

- Show common `/dev/ttyACM*` and `/dev/ttyUSB*` devices by default, with ACM devices first
- Optionally reveal every `/dev/tty*` device from the advanced settings
- Select common baud rates, including 1,500,000 baud, or enter a custom rate
- Configure data bits, stop bits, parity, flow control, and local echo under a compact advanced section
- Set the `tio` output character delay and output line delay for slow or fragile receivers
- Start and stop a `tio` session inside an embedded terminal
- Preserve `tio` automatic reconnection behavior
- Report missing devices, permissions, and process failures clearly

### Connection profiles

- Save the device, serial settings, logging options, and quick buttons as a named profile
- Switch profiles, update the active one, duplicate it, or delete it from the toolbar
- Profiles also remember the matching `/dev/serial/by-id` name, so a saved entry keeps working
  after the kernel renumbers `/dev/ttyUSB*`

### Terminal and session

- Clear both the visible terminal and its scrollback history without sending data to the device
- Search the scrollback with `Ctrl+Shift+F`, including case-sensitive and regular-expression matching
- Follow new output while the view sits at the bottom, and pause following once you scroll up;
  a button reports new output and returns to the bottom
- Read the device, baud rate, framing, flow control, connected time, and log size from the status bar

### Sending

- Send commands from a dedicated bottom input bar or interact directly with the terminal
- Browse the last 100 sent commands with `Up` and `Down`, or pick one from the history list
- Choose the line ending appended to each send: none, LF, CR, or CR+LF
- Configure four persistent quick-send buttons with escaped control-byte support
- Toggle `tio --output-mode hex16` from a persistent bottom-right HEX control

### Logging

- Enable timestamps and session logging to a user-selected directory
- Name the log file explicitly or let tio-gui generate a dated name per session
- Append to an existing log or start a new one, and optionally strip control characters
- Open the log directory from the settings panel and see the current log file and its size
- Get a one-time warning when the current log passes a configurable size

### Interface

- Switch the interface immediately between the system default, Simplified Chinese, Traditional Chinese, English, Japanese, and German
- Keyboard shortcuts: `Ctrl+Shift+L` clear, `Ctrl+Shift+C`/`Ctrl+Shift+V` copy and paste,
  `Ctrl+Shift+F` search, `F5` connect, `F6` disconnect. Plain control characters such as
  `Ctrl+C` are never intercepted and always reach the device
- A second launch activates the running window instead of opening a competing session
- Install a Wayland-compatible desktop entry and branded `tio` application icon
- Follow the system locale, with English, Simplified Chinese, and Traditional Chinese included initially

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

Install a C17 compiler, CMake, pkg-config, GTK4, VTE for GTK4, PCRE2, and `tio`, then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tio-gui
```

## Architecture

The GUI launches `tio` as a separate child process attached to a VTE pseudo-terminal. Version 1 deliberately does not implement an alternative serial backend: it manages and controls `tio` through its public command-line behavior.

User preferences are stored in `~/.config/tio-gui/config.ini`. The file holds the last session under
`[session]`, general options under `[general]`, the send history under `[send]`, and one
`[profile:<name>]` group per saved profile. Files written by 0.1.x are migrated automatically on first
start. Session logs default to the user's `Documents/tio-gui` directory.

Normal launches keep a single window and a single session: starting tio-gui again activates the window
that is already running. Developers can set `TIO_GUI_NON_UNIQUE=1` to run an isolated preview beside an
existing session without activating or controlling that existing window.

The Nix development shell defaults to the `zh_TW.UTF-8` locale for the maintainer's local debugging. Source strings, documentation, packaged defaults, and GitHub communication remain English; release builds follow the user's system locale.

Enabling session logging also enables ISO 8601 line timestamps with millisecond precision. tio-gui always
passes an absolute `--log-file` path, including for automatically named logs, so it can show and monitor
the file it started. `tio` 3.9 does not provide size-based log rotation; tio-gui therefore only warns once
when a log passes the configured size. A safe rotation policy is planned as part of the future session
proxy instead of being simulated with an unsafe file truncation workaround.

## Planned highlighting

An optional line-oriented highlight view is planned with these conservative defaults:

- red: `ERROR`, `FAIL`, `FAILED`, `FATAL`, `PANIC`, `CRITICAL`, `ASSERT`
- yellow: `WARN`, `WARNING`, `TIMEOUT`, `RETRY`
- green: `PASS`, `PASSED`, `OK`, `SUCCESS`, `READY`
- blue: `INFO`, `NOTICE`
- grey: `DEBUG`, `TRACE`
- accent colour: decimal numbers and hexadecimal values or addresses

Matching will be case-insensitive and word-boundary aware to avoid false positives such as highlighting `OK` inside another word. The renderer will consume a display copy of the session instead of modifying the raw terminal stream or `tio` log output. User-defined regular-expression rules can be layered on after the safe defaults.

### Process isolation

`tio-gui` only signals the exact child process that it started. It never searches for, reconfigures, or terminates `tio` sessions launched by the user or by other applications. If another session already owns a serial device, `tio-gui` reports the resulting conflict instead of interfering with that session.

## License

GPL-3.0-only. See `LICENSE`.
