# Backlog implementation and acceptance

## Quick-send editor — 2026-09-08

Implemented text/HEX payloads, CR/LF/Tab/Escape insertion, explicit CRC-8/SMBUS,
CRC-16/MODBUS and CRC-32/ISO-HDLC, line endings, byte previews, optional delay
before sending (0–60 seconds), and standalone INI button-group import/export.
CRC covers the payload; the line ending follows the checksum. Multi-byte CRCs
are little-endian. Invalid input prevents saving/exporting. Imports validate
all rows before changing the draft. Cancel leaves the session unchanged.

Quick sends use tio's raw socket with asynchronous writes. Concurrent writes
are rejected instead of interleaving packets. Disconnect cancels delayed sends.
Write completion means handed to tio, not acknowledgment by the target device.
Button configuration survives defaults, profile copies and restored tabs.

Validation:

- `ctest --test-dir .cache/build --output-on-failure`: payload CRC check vectors,
  invalid escapes/HEX, binary bytes, preset roundtrip, corrupt import rejection,
  settings persistence, existing highlighter tests.
- `.cache/build/quick-editor-test`: actual GTK editor preview and invalid-input
  gating; asynchronous binary write and pending-delay cancellation.
- `python3 tests/socket_acceptance.py`: isolated PTY + real tio 3.9, all 256 byte
  values preserved in both directions through the socket.
- GTK snapshot inspected at `.cache/quick-editor.png`: all controls visible at
  980×620, scroll container supports smaller windows.

No physical device was modified during these tests. DTR/RTS and RS-485 electrical
behavior are outside these tests. Translation catalogues will be refreshed after
UI additions settle.

## Remaining work

The authoritative feature list is `../tio-gui-todo.md` in the workspace.
Next: saved send sequences with run-once, loop, pause and stop. Then serial line
controls, reconnect controls, About/diagnostics, raw-stream analysis, recording,
export/rotation, and remaining long-term backlog items. Do not mark unsupported
or untested features complete merely to close the list.

## Send sequences — 2026-09-08

Named sequences support up to 256 editable/reorderable steps and 100 saved
sequences. Each step has text/HEX, line ending, CRC and a post-send delay. Run
once or loop, pause/resume, and stop are independent per session. Settings
backup includes sequences. Execution snapshots the draft; editing or closing
the editor does not change the active run. Disconnect cancels it immediately.
Completed socket writes precede each delay; scheduler resolution is 10 ms and
this is not a hard real-time generator. Stop prevents later steps; bytes already
handed to tio cannot be recalled.

The original backlog proposed Lua scripts. Inspection of the
[tio 3.9 scripting implementation](https://github.com/tio/tio/blob/v3.9/src/script.c)
and [serial loop](https://github.com/tio/tio/blob/v3.9/src/tty.c) showed synchronous
script execution. A long-running or paused script occupies that loop. The
implementation instead schedules bounded asynchronous raw-socket writes while
normal RX continues, and never pushes protocol payloads through the pty.

Validation: sequence lifecycle tests cover backpressure, post-send delay, pause,
resume, looping, stop, failure and serialization. GTK tests cover save/load of
multiple-step drafts, persistence, rejection of invalid HEX, and window reopen.
`python3 tests/socket_acceptance.py .cache/build/quick-editor-test` exercises the
application's actual sequence runner and send callback through real tio 3.9 and
an isolated PTY: binary `00 14 FF`, then Modbus request
`01 03 00 00 00 0A C5 CD`, with a measured 118 ms gap for a configured 100 ms delay.

## Serial lines and RS-485 — 2026-09-08

Connection settings expose unchanged/low/high defaults for DTR and RTS, pulse
duration, RS-485 and validated configuration. Live controls provide Break,
DTR/RTS low/high and pulses. Pulse uses tio's native control command. Explicit
levels use a private generated Lua file and wait for tio's filename prompt
before submitting its path with CR. A single combined write was experimentally
shown to stall the prompt; LF also does not terminate it. The implemented
handshake was tested through actual VTE and tio. Runtime files are removed on
disconnect. GUI reports submission, not confirmation of a physical voltage.

`serial-options` CTest checks CLI construction and rejects invalid RS-485 tokens
and values. `python3 tests/serial_line_acceptance.py .cache/build` launches real
tio on an isolated PTY with a **test-only** ioctl shim. Actual GUI handlers and
VTE exercise high/low/pulse commands; the shim checks the resulting modem bits,
RS-485 flags and pre/post delays. No command data reaches the serial peer.
The shim is never installed or loaded into the application. It verifies software
behavior and is not an electrical test. tio uses electrical HIGH/LOW terminology:
HIGH clears the active-low modem-control bit. Break submission is exercised but
its electrical waveform is not measured.

## Reconnect policy and observed state — 2026-09-08

Per-session settings now expose automatic reconnect, direct/new/latest device
selection, exclusions for device/driver/topology, and optional desktop notification
or sound. New/latest omits the positional device and delegates selection to tio.

The GUI observes only its exact child's `/proc/PID/fd` entries, without opening
or reading the serial device. It displays waiting until tio has a serial descriptor,
disables payload sends while disconnected, stops pending sequences/delays on an
observed disconnect, and counts observed reconnections. The last reason distinguishes
an absent serial descriptor from a child exit; it does not invent a USB hardware
failure reason. A one-second observer may miss disconnects shorter than its interval.

`connection-state` tests a PTY descriptor opening and closing. The end-to-end
`python3 tests/reconnect_acceptance.py .cache/build` unplugs/replaces a PTY beneath
real tio and runs the actual GUI observer, verifying connected → disconnected →
reconnected, input gating, last reason and count = 1. CLI options are covered by
`serial-options`; desktop notification delivery remains dependent on the desktop.

## Portable configuration and diagnostics — 2026-09-08

Full INI backups remain available. A separate portable export retains profiles,
quick buttons, log options and sequences while removing machine-specific serial
identities, local log paths, exclusion patterns, restored tabs and send history.
Payload text is intentionally preserved. Import rechecks that **all** sessions
are disconnected after file selection and updates each session's settings. File
callbacks retain the window and ignore completion after it is closed.

The About window shows application/tio/GTK/VTE/kernel versions, GPL-3.0-only and
project/license links, and copies the displayed diagnostic text. It omits host
name, user name, paths and device payloads. tio version is queried asynchronously
with a five-second timeout. Tests verify portable exports omit seeded private
paths/history without mutating source settings, and the GTK About window resolves
the installed tio version and displays expected diagnostic fields.
