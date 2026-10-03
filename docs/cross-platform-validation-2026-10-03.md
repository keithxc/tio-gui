# Cross-platform validation — 2026-10-03

Scope: the remote-serial and distribution changes for 0.4.2, based on
`8cc6820`. The initial integration checks below used source version 0.4.1.
These checks do not establish store approval, distribution repository
acceptance, physical-device support, or installation on end-user systems.

## Evidence

| Target | Checks performed | Result |
| --- | --- | --- |
| macOS / Apple Silicon | Desktop plus agent build; existing 12 desktop CTest groups and the agent group | Passed, 13/13 at the initial integration checkpoint |
| macOS / Apple Silicon | Final agent, gateway and real HTTP-to-PTY groups; Nix package with OpenSSL available for TLS tests | Passed, 3/3; installed agent version and gateway help verified |
| Linux / x86-64 | Native clean desktop plus agent build, final CTest run | Passed, 13/13 groups |
| Linux / x86-64 | Separate headless build and tests; desktop and agent Nix packages; staged installation | Passed, 3/3 headless groups; both Nix packages built |
| Linux / ARM64 | Native Nix agent package on Raspberry Pi, including final gateway and PTY tests | Passed, 3/3 groups; installed agent version and gateway help verified |
| Windows / x86-64 | Clean MinGW cross-build of desktop, agent and three C test executables | Compiled; followed by native UCRT64 CI below |
| Linux distribution integration | Source and staged AppStream/desktop/icon validation | Passed with appstreamcli 1.1.3 and desktop-file-validate 0.28 |
| CI definitions | Linux, Windows and macOS workflow validation | actionlint passed; hosted validation is recorded below |

### 0.4.2 release checks

- Linux x86-64: fresh desktop plus agent build and 14/14 CTest groups under
  Xvfb, including the close/reopen workspace regression. Both Nix packages built;
  the agent package ran its three headless groups inside the Nix build.
- macOS: signed and notarized packages, 12 desktop CTest groups and isolated
  final-bundle PTY checks; see [package acceptance](macos-0.4.2-acceptance.md).
- Native Windows UCRT64: [CI at fde4e22](https://github.com/keithxc/tio-gui/actions/runs/37131772236)
  passed compilation, six registered CTest groups and executable versions.
  Windows omits the POSIX PTY cases; GUI and physical COM acceptance remain open.
- Native AArch64 and x86-64 Linux: the existing plugin transformation, file
  isolation, timeout and output-limit test passed on both architectures after
  selecting the native seccomp audit architecture. Process-creation restrictions
  remain enabled; unsupported ABIs fail the build instead of disabling the filter.

The final headless groups contain seven real-agent cases, thirteen HTTP/mock
gateway cases and one real HTTP → gateway → agent → PTY integration case on
POSIX. Windows skips PTY cases and still needs a real or virtual COM pair.
The gateway's TLS regression uses a generated certificate that the test client
explicitly trusts; certificate verification stays enabled.

Transport checks include all 256 byte values, canonical base64, malformed JSON
and escaped NUL rejection, cross-process device exclusivity, stdin EOF cleanup,
reconnect-wait cleanup, exclusive browser ownership, event acknowledgment,
lease expiry, explicit overflow, request limits, stalled agent cleanup and
silent TLS peers. Accepted sends and actual TX notifications are distinct.

## Browser exercise

A desktop in-app browser operated an isolated synthetic PTY peer through the
real HTTP gateway and agent:

- Connected and exchanged UTF-8 text, including Chinese characters.
- Sent HEX bytes `00 14 41 FF` and verified them in the received HEX view.
- Checked 390 × 844 and 1024 × 768 viewport layouts without horizontal overflow.
- Paused display, received additional data, then resumed and saw both messages.
- Downloaded the raw receive buffer and compared all 87 bytes against the
  expected synthetic response, including CR/LF and UTF-8.
- Disconnected/reconnected and released control successfully.

The test browser and its loopback gateway were closed afterward. These are
responsive-layout and browser interaction checks, not iPad Safari or Android
Chrome device acceptance. No physical serial adapter or device logs were used.

## Terminal workspace follow-up

The remote browser UI was subsequently changed to give the receive view the
remaining window area, with a fixed send composer and connection/tool drawers.
The native GTK desktop layout was not changed by this follow-up.

An isolated synthetic PTY and the real gateway/agent were exercised again:

- At 1024 × 768, the receive view measured 586 px high (76% of the viewport).
  The 390 × 844 and 320 × 700 layouts had no horizontal document overflow.
  A 390 × 430 reduced viewport kept the composer visible. This simulates reduced
  available space, not an actual mobile soft keyboard.
- At 844 × 390, the connection drawer scrolled internally and its Done footer
  remained visible. Escape closed the modal; actual connection confirmation
  automatically closed it. Serial settings were disabled while connected.
- Scrolling to the top froze the rendered buffer and position. After a second
  230,009-byte burst, RX advanced to 460,018 bytes while that snapshot and its
  scroll position stayed unchanged, despite the 128 KiB live-buffer limit.
- Drag-selected text remained selected and unchanged during continuing RX;
  Latest resumed the live buffer. Pause and Text/HEX switching were exercised,
  including Latest with zero pending bytes and its resumed status message.
- Shift+Enter inserted a newline without advancing TX. Enter sent the exact
  `alpha\nbeta\r\n` bytes once, confirmed at the PTY peer. Composer growth
  retained automatic following. Invalid HEX `0G` preserved the draft, displayed
  an error and transmitted no bytes.

No physical serial adapter, mobile browser, touch selection, or IME candidate
acceptance is claimed by these desktop-browser checks.

The updated browser assets were verified in the local Apple Silicon Nix
`serial-agent` package. All three agent/gateway/integration CTest groups passed
again; the packaged JavaScript and CSS matched the working files byte for byte.

## Remaining gates (device and distribution acceptance)

- Native Windows GUI interaction, full desktop tool parity, UCRT64 package migration,
  offline installation and real COM adapter tests.
- Real iPad/Android HTTPS sessions: certificate trust, soft keyboard, selection,
  orientation, background expiry and physical USB-UART round trips at the host.
- Android native local USB and any iPad-specific local USB implementation.
- Distribution-by-distribution installation, permissions, upgrade/removal and
  hardware checks; ARM64 desktop package validation; actual upstream submissions.

The agent and gateway are an experimental addition. Do not describe this report
as five-platform feature parity or support for every Linux distribution.
