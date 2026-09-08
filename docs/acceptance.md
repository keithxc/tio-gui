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
