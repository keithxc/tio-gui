# Experimental remote serial client

The remote client lets a browser control a serial adapter connected to a
Linux, macOS, or Windows gateway computer. It is an additional, GPL-3.0-only
entry point to the native serial backend. The existing desktop application
does not start a server automatically.

**This is remote serial access. It does not give Safari or an Android browser
access to a USB adapter plugged into that tablet or phone.** It is not a native
iPad/Android application or a mobile store release. Browser and hardware
acceptance must be reported separately from HTTP and PTY tests.

## Build and start locally

Requirements: a C17 compiler, CMake 3.20+, pkg-config, GLib/GIO 2.66+,
JSON-GLib 1.6+, and Python 3.10+. The headless agent does not need GTK, VTE,
or the `tio` executable. Use the project's Nix development shell where available.
Windows builds additionally need the normal native C toolchain and the agent's
GLib/JSON-GLib runtime DLLs.

Nix users on x86-64 Linux, ARM64 Linux, or Apple Silicon can build a package
including Python, the agent and the browser assets:

```sh
nix build .#serial-agent --out-link result-agent
./result-agent/bin/tio-remote
```

This creates a local build result only; it does not enable a service or replace
the installed desktop application. The generic source build is:

```sh
cmake -S . -B build-agent -DTIO_GUI_BUILD_DESKTOP=OFF -DTIO_GUI_BUILD_AGENT=ON
cmake --build build-agent
ctest --test-dir build-agent --output-on-failure
python3 remote/gateway.py --agent ./build-agent/tio-serial-agent
```

On Windows, run Python with `py -3` and pass the built `tio-serial-agent.exe`
path. Depending on the generator, it may be inside a `Release` subdirectory.

Open `http://127.0.0.1:8765` on the same computer, enter the access token printed
by the gateway, and choose **Take control**. In the **Connection** drawer, select
or enter a remote device path, configure the serial settings, and choose
**Connect**. The drawer closes after the serial worker confirms the connection.
The gateway launches exactly the executable supplied with `--agent`; the browser
cannot select commands or executables. Stop it with Ctrl-C after use. No service
is installed or enabled.

## Connect a tablet or another computer

The default listener is loopback-only. Listening on a LAN address requires a
TLS certificate and key. Obtain a certificate valid for the name used by your
clients and trusted by those clients; a private CA is also suitable when its
certificate is installed and trusted on each test device. Do not disable
browser certificate checks.

```sh
python3 remote/gateway.py --agent ./build-agent/tio-serial-agent \
  --host 192.168.1.10 --port 8765 \
  --cert /path/to/server-chain.pem --key /path/to/server-key.pem
```

Replace the example address with the gateway's actual IPv4 address. Open
`https://<certificate-hostname>:8765` on the tablet and enter the token manually.
The gateway currently binds IPv4 addresses only. A binding of `0.0.0.0` listens
on all IPv4 interfaces; use a specific LAN address when only that interface is
needed. The gateway is intended for an explicitly started development session
on a trusted network, not an unattended public Internet service.

The token is newly generated with 256 bits of randomness on every start. It is
printed to the launching terminal, sent in the Authorization header, and held
only in browser memory. It is never placed in URLs, cookies, local storage, or
page source. All assets are bundled locally; there are no CDN dependencies.

## Use the workspace

The terminal view occupies the space between the compact toolbar and the fixed
send bar. Select the port and serial-settings summary above the terminal to
reopen the **Connection** drawer. Connection parameters and serial control lines
stay in that drawer; **Tools** opens buffer export and **Release control**.

Type a command in the bottom send bar. **Enter** sends it; **Shift+Enter** adds a
new line. Confirming an input-method composition does not send the command.
Choose Text or HEX and a line ending beside the input. The input expands for up
to three lines and scrolls for longer commands. A failed send preserves the
input for correction.

Output follows incoming data until you scroll up, select terminal text, or
enable **Pause**. Those actions hold the displayed snapshot so new data does
not move the lines being read or replace the selection. Receiving and RX/TX
counters continue. Choose **Latest** to show the latest buffer and resume
following; scrolling back down or clearing a selection alone does not resume
the display. Turning off **Pause** also resumes following.

Changing Text/HEX while the display is paused or held transforms that same
snapshot. Clear a terminal text selection before changing formats. **Tools →
Save receive buffer** always exports the latest received bytes, including data
that arrived while the display was held; it does not export the frozen view.
**Clear** discards the page's current receive buffer and rendered view while
preserving session counters.

## Behavior and limits

- One browser page owns the agent at a time. Other pages receive an ownership
  error. **Release control** closes the serial session and gives up ownership.
  Manually taking control again from the same page starts a fresh session;
  it does not resume queued commands or retained receive data.
- Closing the page attempts an immediate release. Network loss or browser
  suspension can prevent that request; the default 45-second idle lease then
  closes the serial session. `--idle-timeout` accepts 15–300 seconds. A suspended
  mobile browser is not a reliable background logger.
- An accepted `open` request starts the serial worker. The **Connected** status
  appears only after the worker confirms the actual port connection.
- An accepted send is queued. **TX** counts only bytes reported written by the
  serial worker. Disconnect cancels unsent data; reconnect never replays it.
  A TX event confirms an OS write, not acknowledgment by the attached hardware.
- Text sends use UTF-8. HEX accepts complete hexadecimal byte pairs separated
  by optional whitespace. Both formats use the chosen line ending. Each send
  is limited to 65,536 bytes including the ending.
- DTR/RTS and Break depend on adapter and driver support. Device permissions,
  competing applications, unplugged hardware, and unsupported settings are
  reported through the serial agent. The gateway does not acquire root/admin
  rights or change device permissions.
- The page displays raw text or HEX, not an ANSI/VT terminal emulator. It retains
  only the latest 128 KiB of received bytes; this display is not a recording.
  RX/TX counters cover the current control session, including bytes no longer
  visible. A held display can continue showing older bytes after the live buffer
  has advanced. **Save receive buffer** downloads a byte-exact snapshot of the
  current live buffer, not a complete recording or the frozen display contents.
- Events are acknowledged by sequence number. The server buffers at most 4 MiB
  or 8,192 events. A slow consumer that exceeds either limit receives an explicit
  data-loss error and the serial session closes. The UI requires release before
  reconnecting. There is no automatic replay of serial writes.
- HTTP request bodies are limited to 96 KiB; event responses to 256 events /
  256 KiB; agent output lines to 128 KiB. The server allows 16 simultaneous
  request threads, one event poll per owner, and 600 POST requests per minute
  across the gateway. A stalled agent request times out and stops the agent.
- Cross-origin browser requests are rejected. There is no permissive CORS
  response. The page uses text-only rendering for received data and a restrictive
  Content Security Policy. CLI clients may omit Origin but still require the
  access token and owner identifier.

## Local protocol

The native agent accepts newline-delimited JSON on stdin and returns JSON on
stdout. Each request has an `id` string and an `op`: `devices`, `open`, `send`,
`line`, or `close`. Responses carry the matching `id` and `ok`; status and
RX/TX/DONE events arrive independently. Serial bytes use canonical base64.

The gateway owns that process and exposes these same-origin HTTP endpoints:

| Endpoint | Body | Result |
| --- | --- | --- |
| `POST /api/claim` | `{}` | Take exclusive control |
| `POST /api/request` | Agent request without `id` | Agent response without `id` |
| `POST /api/events` | `{"after": 0}` | Long-poll events with `seq`, plus optional `fault` |
| `POST /api/release` | `{}` | Close the session and release control |

Every POST needs `Authorization: Bearer <token>`, `X-Tio-Client: <64 lowercase
hex characters>`, and `Content-Type: application/json`. Generate the client ID
from 32 random bytes and keep it only for that page/client instance. The event
cursor is the last event successfully consumed; acknowledging a cursor removes
older buffered events. Only acknowledge events after processing them. Do not
retry serial send or line-control requests automatically: a lost HTTP response
does not prove that a write was not accepted.

## Verification

```sh
python3 tests/remote_gateway_test.py -v
```

The mock-agent suite exercises actual HTTP requests, authorization, origin and
ownership rejection, event acknowledgment, buffer overflow, idle expiry,
request limits, process-pipe framing, and refusal to bind plaintext on a LAN.
When `openssl` is available, it also generates a temporary test certificate and
checks that a silent TLS client cannot block other connections. This test trusts
only its generated certificate; it does not disable TLS verification.

Before advertising a tested mobile/browser combination, record its OS/browser
version and complete a real-device session: connect, bidirectional binary and
UTF-8 data, soft keyboard, orientation changes, text selection, unplug/replug,
background/foreground expiry, and release/reacquire. Validate actual USB
adapters separately, including control lines and the intended baud rates.

## Mobile USB roadmap

Android local USB requires a native USB host backend and chip-specific drivers.
Android's [USB host API](https://developer.android.com/develop/connectivity/usb/host)
provides enumeration and endpoint access with user permission; host capability
is not guaranteed on every device. The open-source
[usb-serial-for-android](https://github.com/mik3y/usb-serial-for-android) library
is one candidate to evaluate with pinned dependencies and physical adapters.

For iPad, [DriverKit support](https://developer.apple.com/documentation/driverkit/creating-drivers-for-ipados)
requires an M-series iPad and appropriate capabilities; it does not cover all
iPads. Apple documents [SerialDriverKit](https://developer.apple.com/documentation/serialdriverkit)
and [USBSerialDriverKit](https://developer.apple.com/documentation/usbserialdriverkit)
for macOS. iPad USB support would be a separate USBDriverKit implementation and
hardware validation effort. [ExternalAccessory](https://developer.apple.com/documentation/externalaccessory/)
supports compatible accessory protocols, not arbitrary USB serial adapters.

[Chrome's Web Serial documentation](https://developer.chrome.com/docs/capabilities/serial)
describes desktop support and a restricted Android WebUSB polyfill; WebKit's
[Web Serial position](https://github.com/WebKit/standards-positions/issues/199)
does not provide a Safari implementation. The remote client intentionally uses
ordinary authenticated HTTP(S) to reach the gateway's serial port.

The gateway, browser sources, and protocol remain open source under the
repository's GPL-3.0-only license. Future mobile distributions should retain
dependency license notices and independently review their packaging requirements.
