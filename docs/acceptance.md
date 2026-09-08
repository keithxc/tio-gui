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

## Raw-stream analysis — 2026-09-08

The per-session Analyze window consumes raw socket bytes independently of VTE and
tio logging. It provides substring/bounded PCRE2 filters, seven level counters and
checkbox filters, a retained entry history, selectable/collapsible JSON or key/value
fields, and up to three numeric curves. Curves accept JSON paths (including array
indices), key/value fields, CSV col1..colN, or named regex captures. Each curve is
bounded to 10–5000 points and can be hidden; axes auto-scale. Selecting a line or
text pauses view following without pausing capture. CSV exports the filtered
retained snapshot with discovered fields, normalized text and timestamps, quoting
cells and escaping spreadsheet formula prefixes. Nested values remain JSON cells.

Retention is limited to 10,000 lines and 8 MiB of text, individual normalized lines
to 16 KiB, JSON nesting to 32, inspection trees to 256 nodes, and regex pattern,
match/depth/heap limits. Lifetime level counts are separate from retained history.
The display shows the most recent 1000 matching entries. Analysis Clear does not
clear the terminal or files. CSV is a line-oriented text export; byte-exact capture
is a separate facility.

Tests cover segmented ANSI/CRLF, JSON/nested numeric paths, keys and quoted CSV,
retention, adversarial regex limits, formula escaping and automatic CSV columns.
The GTK analyzer test covers live/pause/resume, filter results, numeric curves,
named captures and field inspection; a synthetic screenshot was inspected.

## Capture, rotation and offline replay — 2026-09-08

Per-session recording saves versioned JSONL `.tiocap` parts with base64 exact RX,
completed socket TX, terminal INPUT requests, observed connection events and the
running serial parameters. INPUT includes tio control requests and is deliberately
separate from confirmed socket writes; neither proves physical delivery. Large
writes split into adjacent records at the same timestamp. System-clock regressions
are clamped to keep event order. Files are private and exclusively created.

Rotation supports size/time, retained part count and a disk budget for files created
by this recording. Only closed tracked parts are deleted. Each part is independently
playable; select a part to review it. This does not rotate tio's separate legacy
text log. An 8 MiB queue bounds memory; overflow or I/O failure stops recording and
shows the error instead of blocking serial reception. App/tab close waits for
pending recording writes. File-selection callbacks use weak windows and stable tab
IDs. Dragged tab order and closing a tab's global signal subscriptions were fixed.

Offline replay loads a regular file up to 128 MiB on a worker, with bounded records,
JSON depth, decoded bytes and event count. It starts paused, supports 0.1–32x speed
and optional RX hex display, and feeds only an independent analyzer model. There
is no replay transport path. Tests cover every byte value, exact timestamps,
pause/speed, retention/disk budgets, timed rotation, collisions, queue overflow,
malformed input, asynchronous GTK replay and window-close draining.

The complete GTK suite also exposed a GTK 4.22.4 Wayland input-method crash inside
`notify_im_change` when rapidly destroying successive synthetic windows. Individual
window tests pass with the desktop default input method; the combined suite is run
with `GTK_IM_MODULE=simple` to isolate it from pending compositor IM events. This is
a test environment workaround, not an application-wide input-method override.

## XMODEM / YMODEM / ZMODEM — 2026-09-08

File Transfer uses a separately cancellable lrzsz process connected directly to a
fresh tio raw socket. Nix packages include lrzsz. No shell, remote command mode,
full-path sender option or overwrite option is used. Receive requires an empty
chosen directory and enables lrzsz restricted/protect mode. XMODEM lacks a length
field and can retain final-block padding; its output is `received.bin`. Y/Z retain
sender filenames. The GUI disables terminal input and rejects ordinary socket
sends while transferring; disconnect/close cancels the process. A configurable
5–86400 second deadline prevents abandoned jobs. stderr progress is bounded to the
latest 2 KiB. A cancelled peer may need its own reset before normal commands resume.

`transfer_acceptance.py` exercises the actual application transfer engine through
real tio and a PTY lrzsz peer, all three protocols in both directions, comparing
8192 bytes containing every byte value. It also verifies refusal of nonempty receive
directories and termination of a stalled peer. GTK full-window regression passes.
Transfer protocol TX goes directly through the transfer socket and is not part of
the GUI's completed-command TX capture; incoming protocol bytes remain RX data.
Protocol details and restricted-mode behavior were checked against the installed
lrzsz help and [upstream lrzsz](https://www.ohse.de/uwe/software/lrzsz.html).

## Declarative parser presets and custom highlighting — 2026-09-08

Analyze can save/load versioned `.tiorules` files containing a substring or bounded
regex trigger, named-capture extraction, numeric field paths, level selection and
point retention. Loading validates the complete file before changing controls;
files are bounded to 64 KiB. Rules have no executable actions or automatic sends.
Built-in JSON/key/value/CSV parsing remains available without a preset.

Global Custom Highlight Rules accepts up to 32 named INI sections with pattern,
color and bold style. Rules are persisted, included in portable/full configuration
exports, and applied to new lines in every session. Invalid replacement rules leave
the old set intact. Built-in and custom GRegex rules now prepend explicit PCRE2
match/depth/heap limits; previously the match-count limit alone did not constrain
backtracking. Tests exercise an adversarial nested repetition, atomic rejection,
custom tags/reset, and parser preset roundtrips through actual analyzer controls.

## Lua / JavaScript analysis plugins — 2026-09-08

API 1 is an explicit, per-entry `transform(input)` operation from Analyze. The
plugin editor supports source load/save, Lua/JavaScript selection, read-only input
snapshot and selectable result. See `docs/plugins.md` for limits and sandbox
boundaries, plus tested examples in `examples/plugins/`. Runtime behavior was
checked against [Lua 5.4](https://www.lua.org/manual/5.4/manual.html#pdf-load) and
[QuickJS](https://bellard.org/quickjs/quickjs.html) documentation.

Tests cover both language results, absent Lua I/O capabilities, hidden host files,
infinite loops, closing output descriptors before an infinite loop, oversized
output, thrown errors, rejected asynchronous returns, GTK result display and
closing a running plugin window. Bubblewrap/seccomp isolation and resource limits
are exercised with the actual installed interpreters on Linux x86-64. There is no
fallback to executing outside the sandbox.

## TCP / UDP debugging — 2026-09-08

Settings opens independent network debugging windows with hostname/address and
port, TCP client or connected UDP peer, connect/disconnect, text/HEX payloads,
CRC/ending preview, RX/TX counters, escaped text/HEX receive previews and Analyze.
The analyzer receives the full byte chunks; the traffic view bounds preview length
and retained rows. Follow can be paused. UDP explicitly reports a configured peer
rather than claiming a successful remote handshake.

The asynchronous backend handles DNS/connect cancellation, partial TCP writes,
peer EOF, empty UDP packets and bounded send queues (1 MiB / 256 items). It emits
TX only after a full write and limits datagrams to 65507 bytes. Tests run all-byte
roundtrips with segmented TCP responses and separate empty/nonempty UDP responses,
then cancellation during connect. The same localhost peer tests drive the actual
GTK connect/send/preview/analyzer/disconnect controls. Network windows do not expose
serial-only line controls or assume an underlying tio process.

## Modbus RTU / TCP — 2026-09-08

Modbus tools support functions 01/02 (coils/discrete inputs), 03/04 (holding/input
registers), and 05/06 (single coil/register writes). Controls use explicit zero-based
protocol addresses, unit, read count and write value, with an exact request preview.
Register results show unsigned, hex and signed-16 interpretations. There are no
automatic retries or broadcasts. TCP opens an independent per-request connection;
RTU uses the current tio socket, requires an idle connected session, and keeps its
window modal so ordinary GUI sends cannot interleave. Serial framing/baud/RS-485
remain those of that tio session.

Requests run off the GTK thread and cancel on window close. Responses validate
RTU CRC or TCP MBAP transaction/protocol/length, unit, function, byte count and
write echo. Exceptions are errors, never successful values. Read counts/address
ranges and single-coil values are checked. Socket I/O has a two-second timeout and
response assembly a three-second deadline (a blocked read may finish at its own
socket timeout). This is a master request tool, not a slave simulator or bus sniffer.

Tests cover the standard CRC request vector, transaction mismatch, exception and
write-echo validation; application and GTK requests both receive fragmented TCP
and real-tio/PTY RTU responses and decode registers 0x1234/0xABCD. Additional peers
exercise mismatched transactions, exception frames and silence/timeouts. Protocol
references: [application specification](https://modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
and [TCP implementation guide](https://modbus.org/docs/Modbus_Messaging_Implementation_Guide_V1_0b.pdf).
