# tio-gui

> A lightweight, reliable GUI for [`tio`](https://github.com/tio/tio), designed for embedded developers who need effortless serial device discovery, automatic reconnection, terminal I/O, and logging.

`tio-gui` is a Linux-first session manager and controller for `tio`. The GUI discovers devices, builds validated options, starts and stops sessions, and exposes status and logs; `tio` remains responsible for all serial I/O and reconnection behavior.

## Status

Early development. The first release targets x86-64 Linux.

## Initial scope

- Discover `/dev/serial/by-id`, `/dev/ttyUSB*`, and `/dev/ttyACM*` devices
- Select common baud rates
- Start and stop a `tio` session inside an embedded terminal
- Preserve `tio` automatic reconnection behavior
- Enable timestamps and session logging to a user-selected directory
- Report missing devices, permissions, and process failures clearly

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

## Build on other Linux distributions

Install a C17 compiler, CMake, pkg-config, GTK4, VTE for GTK4, and `tio`, then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tio-gui
```

## Architecture

The GUI launches `tio` as a separate child process attached to a VTE pseudo-terminal. Version 1 deliberately does not implement an alternative serial backend: it manages and controls `tio` through its public command-line behavior.

### Process isolation

`tio-gui` only signals the exact child process that it started. It never searches for, reconfigures, or terminates `tio` sessions launched by the user or by other applications. If another session already owns a serial device, `tio-gui` reports the resulting conflict instead of interfering with that session.

## License

GPL-3.0-only. See `LICENSE`.
