# tio-gui

> A lightweight, reliable GUI for [`tio`](https://github.com/tio/tio), designed for embedded developers who need effortless serial device discovery, automatic reconnection, terminal I/O, and logging.

`tio-gui` is a Linux-first session manager and controller for `tio`. The GUI discovers devices, builds validated options, starts and stops sessions, and exposes status and logs; `tio` remains responsible for all serial I/O and reconnection behavior.

## Status

Early development. The first release targets x86-64 Linux.

## Initial scope

- Show common `/dev/ttyACM*` and `/dev/ttyUSB*` devices by default, with ACM devices first
- Optionally reveal every `/dev/tty*` device from the advanced settings
- Select common baud rates, including 1,500,000 baud, or enter a custom rate
- Configure data bits, stop bits, parity, flow control, and local echo under a compact advanced section
- Switch the interface immediately between the system default, Simplified Chinese, Traditional Chinese, English, Japanese, and German
- Start and stop a `tio` session inside an embedded terminal
- Clear both the visible terminal and its scrollback history without sending data to the device
- Send commands from a dedicated bottom input bar or interact directly with the terminal
- Configure four persistent quick-send buttons with escaped control-byte support
- Toggle `tio --output-mode hex16` from a persistent bottom-right HEX control
- Install a Wayland-compatible desktop entry and branded `tio` application icon
- Preserve `tio` automatic reconnection behavior
- Enable timestamps and session logging to a user-selected directory
- Report missing devices, permissions, and process failures clearly
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

The initial packaging order is Nix/NixOS first, AppImage second, and Arch Linux (PKGBUILD/AUR) third. Ubuntu/Debian and RPM-native packages are intentionally out of scope for the first public versions.

## Build on other Linux distributions

Install a C17 compiler, CMake, pkg-config, GTK4, VTE for GTK4, and `tio`, then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tio-gui
```

## Architecture

The GUI launches `tio` as a separate child process attached to a VTE pseudo-terminal. Version 1 deliberately does not implement an alternative serial backend: it manages and controls `tio` through its public command-line behavior.

User preferences are stored in `~/.config/tio-gui/config.ini`. Session logs default to the user's `Documents/tio-gui` directory.

Developers can set `TIO_GUI_NON_UNIQUE=1` to run an isolated preview beside an existing session without activating or controlling that existing window.

The Nix development shell defaults to the `zh_TW.UTF-8` locale for the maintainer's local debugging. Source strings, documentation, packaged defaults, and GitHub communication remain English; release builds follow the user's system locale.

Enabling session logging also enables ISO 8601 line timestamps with millisecond precision. `tio` 3.9 does not provide size-based log rotation; a safe maximum-file-size policy is planned as part of the future session proxy instead of being simulated with an unsafe file truncation workaround.

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
