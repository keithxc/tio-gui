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

The authoritative feature list is the workspace file `../tio-gui-todo.md`
(relative to the repository root). The original sequence, serial controls,
analysis and recording backlog is covered by the dated acceptance sections below.
The 2026-09-09 search and font zoom follow-up is documented at the end of this file.
Native Windows/macOS edition acceptance and limitations are tracked in
`serial-portable.md`; physical serial, RS-485, CAN and BLE acceptance remains
separate from software tests. Do not mark unsupported or untested features
complete merely to close the list.

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

## MQTT debugging — 2026-09-08

MQTT 3.1.1 tools provide a clean-session TCP client with optional username/password,
subscription filters/unsubscribe, QoS 0/1 publication, retain, text/HEX payload preview,
message topic/QoS/retain metadata and payload analysis. Credentials are not saved.
This implementation uses plain TCP; TLS, QoS 2, persistent sessions and wills are
not part of this tool. It does not claim QoS 0 delivery acknowledgement.

The bounded codec uses the network backend, validates UTF-8/topics/flags/lengths,
correlates acknowledgement IDs, answers incoming QoS 1 publications, handles
keepalive and handshake timeout, and limits packets to 64 KiB, RX accumulation to
128 KiB and pending acknowledgements to 64. It closes on malformed/oversized input
instead of allocating the protocol's theoretical maximum packet size. Reference:
[OASIS MQTT 3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html).

A temporary localhost Mosquitto broker verifies full binary publication at QoS 1,
subscription acknowledgements, retained delivery after resubscription, unsubscribe
and cancellation. Malformed lengths/CONNACK/PUBLISH frames are rejected. The GTK
test separately drives actual connection, subscribe, HEX publish/preview, received
message and analyzer controls. The broker is stopped and its temporary configuration
removed after every test; no external broker or user credentials are used.

## SocketCAN / DBC — 2026-09-08

CAN tools open an existing configured Linux SocketCAN interface, monitor frames and
error frames, and send classic/FD, standard/extended IDs, BRS and classic RTR. FD
lengths are validated against DLC sizes rather than silently padded. CAN writes
are nonblocking and reported as accepted by the kernel, not electrically delivered.
The application does not change host interface bitrate or link configuration.

DBC import is bounded to 4 MiB, 1024 messages and 128 signals/message. Integer
signals support Intel/Motorola bit order, signed values, factor/offset and simple
M/mN multiplexing. Duplicate definitions and out-of-frame fields are rejected;
extended multiplex definitions and floating-point SIG_VALTYPE_ are explicitly
unsupported. Enum tables and presentation attributes are not interpreted. Decoded
numeric JSON feeds Analyze/plots; scaled values use doubles, so very wide integer
signals may lose numeric precision while raw frame bytes remain visible. An example
is in `examples/dbc/board.dbc`.

`tests/can_acceptance.sh` creates vcan0 only inside a new user/network namespace.
It verifies real kernel classic/FD/extended/BRS binary roundtrips, invalid frame
rejection, GUI sends and DBC-derived numeric analysis. GTK accessibility is disabled
only in that namespace test because its remapped UID cannot authenticate to the
host accessibility bus. No host CAN interface is reconfigured. Separate DBC tests
cover endian/sign/scale/multiplex and malformed definitions. Hardware bus electrical
behavior, arbitration and actual bitrate remain untested. Protocol reference:
[Linux SocketCAN documentation](https://www.kernel.org/doc/html/latest/networking/can.html).

## BLE GATT debugging — 2026-09-08

BLE tools use asynchronous BlueZ D-Bus APIs to list known devices/characteristics,
scan for LE devices for ten seconds, connect/disconnect, read, write with/without
response and start/stop notifications. Writes validate text/HEX and cap values at
512 bytes; BlueZ reports smaller device/MTU limits. Payloads feed Analyze. Pairing
and adapter power remain in system settings. A window releases its own discovery,
notification and newly acquired device connections on close, including a Connect
that finishes after the window was closed. Pre-existing device links are not
claimed as newly acquired by the window.

A separate private D-Bus daemon exports a fake BlueZ adapter/device/characteristic
and verifies the actual GTK discovery filter, connect, binary ReadValue/WriteValue,
request/command options, notifications arriving before StartNotify's reply, invalid
HEX rejection and scan/notify/link cleanup. A second window tests close during
Connect. The real system BlueZ ObjectManager is also queried read-only; no real
scan, pairing, GATT write or device connection was performed. Radio range, pairing
workflows and real peripheral interoperability remain hardware validation limits.
Reference: [BlueZ GATT API](https://bluez.readthedocs.io/en/latest/gatt-api/).

## Session and usability closeout (2026-09-08)

- Added persistent right-click session names, Alt+1…9 selection, `--device/-d`,
  `--baud/-b`, positional device, `--connect` and `--no-connect`. Explicit devices
  connect by default; restored sessions still start disconnected.
- Window close asks before stopping live sessions, then drains recording/spawn
  work. Profile/file/close dialogs resolve weak windows and stable session IDs.
  Quick editors are destroyed with their session. Missing devices remain selected
  across refresh, and loading profiles now restores binary quick-button settings.
- Tools menu separates protocol debuggers from appearance/settings. Compare
  sessions shows bounded, read-only raw log entries side by side, with common
  substring filtering and pause/follow. Reordering cannot switch its underlying
  sessions; a closed session is reported explicitly. Reopen to choose new tabs.
- GTK regression checks cover profile restoration, tab-name INI round trip,
  missing-device refresh, numeric tab actions, compare after reorder/close,
  late-dialog lookup and close draining. Short version comparison no longer
  indexes past a split string; update links must point to this project's releases.
- Update check remains asynchronous, once when opening Settings, with a 15-second
  network timeout. It only offers the release page and never replaces binaries.

## Final 0.3.0 acceptance (2026-09-08)

`nix develop --command python3 tests/headless.py sh tests/acceptance.sh`
completed successfully: 10 CTest groups, GTK application/editor/analyzer/plugin
checks, real tio binary/sequence/line-control/reconnect/transfer PTYs, TCP/UDP,
Modbus TCP/RTU including malformed/exception/timeout replies, real Mosquitto,
private BlueZ and kernel vcan. The additional keyboard driver verifies actual
GTK events through hidden VTE to a PTY for text, Enter, Tab, Escape, Up and Ctrl-C.

All four catalogs (zh_CN, zh_TW, ja, de) contain 430 translated messages with no
fuzzy/untranslated entries and pass `msgfmt --check`. Traditional Chinese was
checked on Wayland; German long labels were checked at a 1000×660 window on a
private X display. Source extraction now includes every protocol UI module and
preserves complete 64-bit format strings. Existing tool windows should be reopened
after switching language; restart updates every pre-existing control.

ASan + UBSan passed the full application GTK regression executable and capture,
log-model, DBC and Modbus codec tests (`detect_leaks=0` for process-global GTK/GLib
caches; this is not a claim of exhaustive leak checking).

The private display keeps synthetic selection/focus tests independent of the
running desktop: the same scroll test lost its synthetic primary selection under
the host Wayland desktop, but passed follow/pause/selection retention/resume on
Xvfb without changing application logic. Hardware Wayland keyboard/IME behavior
is not substituted by the X11 automation.

Transfer acceptance was strengthened after exposing a reference-peer PTY race:
lrzsz terminal cleanup can flush the final ACK/OO before tio reads it. The reference
peer now uses a socketpair relayed to the real tio PTY. Both directions of all three
protocols passed again, including exact 8192-byte comparisons, nonempty-directory
protection and stalled-peer cleanup. Application timeout messages retain the last
protocol status. The relay does not synthesize protocol acknowledgements.

Configuration import also rejects pending process spawns and recording drains,
checked both before opening the chooser and after it returns. The GTK session
test covers import while a background tab is still spawning.

## Highlight scroll rebound — 0.3.1 (2026-09-08)

Live-window sampling captured 70 frames over 22 seconds. Pixel comparisons found
unchanged log blocks moving down by 1, 2, 4, 5 and 6 physical pixels after forward
scrolling. Window chrome stayed fixed. A read-only raw socket sample captured 959
chunks / 3726 bytes in ten seconds, mostly one- and two-byte fragments.

Two scroll targets conflicted: immediate adjustment to `upper - page_size` and a
pending GtkTextView mark alignment, which excludes the six-pixel bottom margin.
The fix validates the final line layout before setting the adjustment, with a
reentrancy guard and no second animated target. Existing margin and bottom
alignment are preserved. The scroll regression now uses the actual wrapping,
monospace and six-pixel margins and samples after-paint during fragmented input.
It failed on the original code with five backward jumps and passes after the fix,
alongside selection retention, pause and resume checks. Captured device bytes and
window images remain local under `.cache/ui-jitter`, not in the repository.

## Incremental display and resource check — 0.3.2 (2026-09-08)

The highlighter preserves unchanged partial-line text and replaces only a
changed suffix (including incomplete UTF-8 replacement characters). Completed
lines append a newline without deleting the visible prompt. Highlight tags are
re-evaluated so extending ERROR to ERRORLESS does not leave a stale error tag.
Serial display updates coalesce on a 16 ms timeout; pending bytes are capped at
64 KiB with synchronous overflow flushes. Capture and analysis still receive
original packets immediately. Clear and tab destruction cancel queued updates.
A 48 logical-pixel bottom margin provides display-only breathing room; it does
not insert blank records or eliminate the need to scroll when output grows.

Validation: 10 CTest suites, GUI lifecycle checks, keyboard acceptance, and
scroll/pause/selection/resume checks pass. Fragmented scrolling produced 193
painted frames with zero backward jumps. Added regression checks cover unchanged
prefix deletion counts, split UTF-8, stale tags, queue delivery, cancellation,
and overflow. This validates update behavior, not a definitive diagnosis of
monitor/compositor ghosting.

Resource samples on this workstation (CPU percent relative to one core):

| Scenario | CPU | RSS MiB | PSS MiB |
| --- | ---: | ---: | ---: |
| Existing 0.3.1 desktop session, 30 seconds | 0.93% | 133.1 | 98.7 |
| Its tio subprocess | 0.10% | 4.0 | 2.0 |
| 0.3.2 isolated application, connected idle, 15 seconds | 0.13% | 134.6 | 73.0 |
| 0.3.2 receiving 1,500 lines / 84,000 bytes in 15.26 seconds | 12.45% | 141.6 | 88.4 |
| 0.3.2 idle after reception, 15 seconds | 0.26% | 141.6 | 83.5 |

The isolated application uses a real tio/PTY under Xvfb with Cairo software
rendering and a private session bus/configuration; the user's device is never
written to or disconnected. These are short samples, not a sustained leak test
or a GPU benchmark. RSS includes shared pages; PSS apportions them. Receive
history accounts for expected memory growth; RSS was stable after traffic
stopped. The text view retains at most 10,000 lines and each parsed line is
limited to 16 KiB.

A highlighter-only microbenchmark (12,000 lines in 3-byte fragments, no GUI)
measured 5.615 s before vs 7.328 s for suffix preservation without coalescing,
with about 49 MiB peak RSS in both cases. Incremental text preservation alone
is therefore not a CPU optimization; packet coalescing is needed to reduce
repeated partial-line highlighting. Measurements and synthetic harnesses are
under `.cache/ui-ghost/` and are not bundled in the application.

## Restore immediate serial display — 0.3.3 (2026-09-08)

Following a report of renewed jitter in 0.3.2, remove the 16 ms display queue
and restore synchronous feed-and-follow for incoming serial packets. Retain
incremental text suffix updates and the 48-pixel display-only bottom margin.
The previous resource figures describe 0.3.2; they are not performance claims
for this version. This change prioritizes the previously accepted update cadence.

Current desktop sampling (50 window images over approximately 15 seconds) and
replay did not reproduce the earlier reverse bounce. They do not establish
that all perceived jitter is resolved. The scroll regression now invokes the
same immediate display helper as the real receive callback, and checks clear
has no delayed text delivery. Xvfb checks passed with GTK warnings fatal:
193 painted fragmented-input frames, zero backward jumps, follow/pause/
selection/resume intact. GUI lifecycle checks also pass. Private captures and
replay diagnostics remain in `.cache/ui-jitter-032/`, outside version control.

## Smooth output following — 0.3.4 (2026-09-08)

Research distinguished input scrolling from output following:

- Neovide `src/renderer/animation_utils.rs` at
  `ade2d9cda777879975b1852f77dc672f5ff43b78` implements a critically damped
  spring preserving velocity. `rendered_window.rs` retains twice the view's
  line count, applies a fractional line translation, and limits distant scroll
  animation. Sources: https://github.com/neovide/neovide/blob/ade2d9cda777879975b1852f77dc672f5ff43b78/src/renderer/animation_utils.rs
  and https://github.com/neovide/neovide/blob/ade2d9cda777879975b1852f77dc672f5ff43b78/src/renderer/rendered_window.rs
- xterm.js `Viewport.ts` at `c58ea3637f3968e0e6e79cd92cf9aace7ef89ee2`
  supports smooth input scrolling, but explicitly stops animation and follows
  immediately when input/buffer changes ydisp. Thus its smooth-scroll option
  alone is not a model for continuously animated serial output.
  https://github.com/xtermjs/xterm.js/blob/c58ea3637f3968e0e6e79cd92cf9aace7ef89ee2/src/browser/Viewport.ts
- Contour separates line scroll offsets and sub-cell pixel offsets; forcing
  bottom resets the pixel offset. Its mouse/touchpad smooth-scrolling features
  must also be distinguished from output following.
  https://github.com/contour-terminal/contour/blob/7bb15af9acdfa5b0e90d308a612dc09046cc62fc/src/vtbackend/screen/Viewport.cpp

Our implementation uses an independently implemented analytical critically
 damped spring (omega 32/s), advanced by elapsed GTK frame-clock time. Data is
still fed immediately; only viewport position is animated. Incoming packets
retarget the same follower without resetting velocity. Floating-point motion
is retained internally while GTK receives integral logical-pixel positions,
avoiding layout rounding reversals observed in the fractional-position trial.
Floods skip to the last screen's distance and animate the remainder, keeping
visual backlog bounded. The existing GTK text history provides offscreen text;
no second copy of the serial log or artificial blank lines is introduced.

Wheel-up, pointer selection, clear, and unmap cancel animation; map resumes
following when appropriate. The callback removes itself at rest. This changes
highlighted log output following; native VTE terminal-mode rendering remains
owned by VTE.

Validation: a single 18-pixel line traversed 9 moving frames with a largest
4-pixel step. The fragmented receive test recorded 205 painted frames and zero
backward jumps. Tests assert that retargeting preserves the active animation,
manual pause stops it immediately, 200-line floods settle, hidden views cancel
callbacks, history selection is retained, clear has no late text, and animation
stops at rest. Tests run with GTK warnings fatal. GUI lifecycle and real keyboard
PTY acceptance passed. A real-data replay on the Wayland desktop at 125% scale
recorded 660 frames with zero backward jumps. Private experimental/replay files
are kept under `.cache/smooth-research/`.

## In-place progress and input cursor — 2026-09-08

The highlight and analysis parsers now share bounded streaming line editing.
CR returns to the start of the current line, LF commits it, and CR+LF commits
only once. Progress updates overwrite existing characters; shorter text leaves
the untouched suffix until explicitly erased. Backspace, CSI K (0/1/2), G,
backtick, C and D are supported across read boundaries. Analysis records the
final LF-terminated content, so transient progress does not inflate severity
counts. CR-only records now remain a single current line. Raw capture is unchanged.
Horizontal positions count Unicode characters, not wide/combining terminal cells;
vertical screen editing still requires VTE.

VTE explicitly uses a blinking block cursor. The read-only highlight view uses
a timer to toggle its native caret at the parsed remote position. GTK's native
[caret blinking requires editable text](https://github.com/GNOME/gtk/blob/4.22.0/gtk/gtktextview.c#L5864-L5899),
so the timer preserves the selection-only buffer and VTE keyboard forwarding.
The caret hides on focus loss, selection, paused following and CSI ?25l; CSI ?25h
restores remote visibility. Unmapping or closing a tab removes the timer.

Validation:

- Full application build and all 10 CTest suites passed.
- Shared fixtures exercised percentage progress, actual newlines, shorter
  overwrites, erase modes, cursor movement, spinners, UTF-8 and ANSI/OSC sequences
  with every fixed chunk size from one byte through the complete input.
- Highlighter tests cover immediate partial progress, parser reset, saturated
  lines and oversized cursor parameters. Analysis tests check final severity
  counts and commands interleaved with an unfinished progress line.
- AddressSanitizer/UndefinedBehaviorSanitizer highlighter and log-model suites
  passed (`ASAN_OPTIONS=detect_leaks=0`).
- `python3 tests/headless.py .cache/build/scroll-test`: timed caret blink,
  backspace position, remote visibility, focus, selection and timer cleanup
  passed; fragmented output painted 204 frames with zero backward jumps.
- `python3 tests/headless.py python3 tests/keyboard_acceptance.py
  .cache/build/keyboard-test`: real GTK keyboard events reached the VTE PTY with
  the expected text, Enter, Tab, Escape, Up and Ctrl-C bytes while caret handling
  was enabled.


## Search and persistent font zoom — 2026-09-09

Interaction references (independent implementation):
[VS Code terminal find](https://code.visualstudio.com/docs/terminal/basics#_find),
[VS Code find navigation](https://code.visualstudio.com/docs/editing/codebasics#_find-and-replace),
and [GNOME Terminal search](https://help.gnome.org/gnome-terminal/txt-search.html).
These inform match highlighting, Enter/Shift+Enter navigation, case/regex options
and wrapping. Persisting console font size is the user's explicit requirement.

- Ctrl+wheel changes console font size by one point, bounded to 6–40pt, saves it
  immediately, and applies it to existing/new tabs. VTE and the highlight view
  use the same point size. Native serial stores it in its separate `serial.ini`.
- Linux searches whichever view is displayed. Typing locates a result;
  Enter/Shift+Enter and F3/Shift+F3 navigate. Ctrl+Shift+F focuses the query without
  toggling a visible search bar closed. Plain serial control characters are kept.
- Highlight/native consoles show all matches and current/total. Background text
  changes refresh at most every 250ms without changing the selection or scrolling
  to another match. Explicit search pauses following; Back to bottom/Follow
  resumes it. Closing Linux search clears its tags and pending refresh.
- VTE retains its native search engine and shows found/no match rather than a
  fabricated total. Errors appear beside the query. Log regex searches cap the
  pattern at 4096 bytes, text at 8 Mi characters, results at 10,000, PCRE matching
  at 50ms, match steps at 10,000, depth at 100 and heap at 1 MiB. VTE patterns
  carry match/depth limits. Limit failures ask for a narrower pattern; zero-width
  matches are omitted from the line console's visible results.
- Added five messages in simplified/traditional Chinese, Japanese and German.

Validation: all 10 Linux CTest suites and three native serial CTest suites passed.
GTK `/console/search-zoom` covers Chinese, case sensitivity, regex, invalid/no
matches, wrapping, Shift+Enter, an adversarial regex, new RX preserving the current
result, VTE/highlight switching, tab isolation, new-tab font inheritance and
settings reload. `tests/search_zoom_acceptance.py` sends actual Ctrl+wheel events
through xdotool to the GTK window and verifies the displayed font and saved size.
The full GTK quick-editor suite passed its runnable cases (external harness and
localized-layout cases retain their explicit skips). Native serial GTK tests
also passed real PTY transport/logging/lifecycle plus new search and persistence
checks. Screenshot: `.cache/search-zoom/search-zoom.png` (local, not versioned).

This round runs native serial UI tests on Linux; it does not establish new
Windows/macOS installation, IME or physical serial hardware acceptance.

The final Nix package built successfully and the KDE launcher resolves to it.
The installed derivation's four changed implementation files were compared
byte-for-byte with the workspace, and the launcher passed `--help`. Existing
serial sessions were left running; close and reopen to load the new build.
All four translation catalogs pass `msgfmt --check` with 435 translated entries.


## Interactive character deletion — 2026-09-09

A real Bash/Readline PTY reproduced the stale-text failure: deleting inside
`abcdef` emitted BS followed by `CSI 1 P`. The highlight model moved the cursor
for BS but discarded DCH, leaving the deleted character visible. End-of-line
Bash deletion emits BS/space/BS and already worked. The fix follows
[xterm's documented controls](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html)
and the editing behavior described in the
[Readline manual](https://www.gnu.org/s/bash/manual/html_node/Readline-Bare-Essentials.html).
No upstream implementation was copied.

The shared line model now handles DCH (`CSI P`, delete and shift left), ECH
(`CSI X`, erase with blanks), ICH (`CSI @`, insert blanks and shift right), and
ED0 (`CSI J` / `CSI 0 J`, erase the current line's suffix). All preserve the
cursor; BS alone still only moves left, preserving spinner/overwrite behavior.
Raw capture and serial keyboard encoding continue through their existing paths.
Completed log records remain history: ED0 is supported for the current line,
not as a full-screen erase across a terminal grid. Unicode horizontal positions
retain the existing code-point model, rather than claiming terminal cell-width
or multi-line Readline support.

Validation adds shared highlighter/analyzer fixtures for default/zero/count/
large parameters, cursor positions, UTF-8, unsupported private/multi-parameter
controls, and every fragmentation size. A capacity test verifies that inserting
blanks drops complete UTF-8 characters instead of splitting them.
`tests/shell_edit_acceptance.py .cache/build/highlighter-test /path/to/bash`
requires an interactive, Readline-enabled Bash and checks real PTY responses
after end/middle Backspace, Delete, insertion, Ctrl-U and Ctrl-K. Each captured
stream is replayed one byte at a time and checks both visible text and cursor.
The two local BusyBox builds lacked command-line editing; their canonical echo
checks are not treated as acceptance of a full BusyBox ash line editor.

The targeted highlighter/analyzer suites and all eight real Bash editing steps
passed. The Nix package built successfully, its parser was verified against the
workspace, and the KDE launcher was refreshed and checked with `--help`.
