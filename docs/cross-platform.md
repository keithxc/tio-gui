# Cross-platform delivery

Updated 2026-10-03. Keep the project and new remote components under
GPL-3.0-only. This is a delivery backlog, not a claim that every target is
already supported.

See the [2026-10-03 validation record](cross-platform-validation-2026-10-03.md)
for actual builds, automated checks, browser exercise and remaining hardware gates.

## Product boundaries

| Target | Current implementation | Remaining release gate |
| --- | --- | --- |
| Linux desktop | GTK4 + VTE + tio; x86-64 Nix package and AppImage | Native distribution CI, ARM64 desktop packaging, package-manager review |
| macOS desktop | GTK4, native serial transport, libvterm and shared serial tools | Release the latest source after hardware and signed-package checks; current downloadable packages require Apple Silicon/macOS 26+ |
| Windows desktop | Native COM transport and a separate serial frontend | Bring sequences, recording, analysis, terminal and Modbus into the shared workspace; validate the native UCRT64 CI and USB hardware |
| Headless Linux/macOS/Windows | Optional `tio-serial-agent`, sharing the desktop native transport | Real adapter matrix, long-running capture and packaged installation on each target |
| iPad / Android remote | Experimental responsive browser client to a manually started gateway | Safari/Chrome device tests, soft keyboard, background/resume, trusted HTTPS and physical USB round trips at the gateway |
| Android local USB | Planned native USB Host transport | User permission, adapter drivers, disconnect/reconnect and hardware tests |
| iPad local USB | Separate feasibility work | Supported hardware, driver entitlement and adapter protocol implementation |

Remote means that the cable is attached to the gateway computer. The browser
does not access its own USB ports. The first remote client is a text/HEX serial
console; desktop protocol tools and terminal screen emulation are separate
follow-up work. Neither the gateway nor a package install starts a network
service automatically.

## Why these boundaries exist

Android exposes USB host interfaces and user-granted device access, but host
support is a device capability and serial adapters still need protocol drivers.
See [Android USB host](https://developer.android.com/develop/connectivity/usb/host).

Apple's iPadOS DriverKit route requires an M-series iPad. The documented
[SerialDriverKit](https://developer.apple.com/documentation/serialdriverkit) and
[USBSerialDriverKit](https://developer.apple.com/documentation/usbserialdriverkit)
interfaces target macOS; an iPad USB implementation cannot simply reuse desktop
termios. See [Creating drivers for iPadOS](https://developer.apple.com/documentation/driverkit/creating-drivers-for-ipados).
[ExternalAccessory](https://developer.apple.com/documentation/externalaccessory/)
is an accessory protocol route, not access to arbitrary USB serial adapters.

Browser USB serial is also not a universal mobile backend. See
[Chrome's Web Serial documentation](https://developer.chrome.com/docs/capabilities/serial)
and [WebKit's position](https://github.com/WebKit/standards-positions/issues/199).
GTK has an Android backend, but its
[backend list](https://docs.gtk.org/gtk4/building.html) does not provide an iOS
backend. Keep these capability differences visible instead of promising one
unchanged desktop build on every mobile device.

## Shipping backlog

Each slice should fit in a 2–4 week release cycle, with evidence recorded before
moving its status to supported.

1. **Remote serial and distribution baseline.** Build the agent without GTK;
   validate binary RX/TX, device exclusivity, reconnect without retransmission,
   EOF cleanup, authentication, controller ownership, idle expiry and bounded
   queues. Run desktop regressions and validate installed AppStream metadata.
   Then complete one real iPad and one Android command round trip.
2. **Desktop feature alignment.** Move the Windows serial tools onto shared
   session interfaces in small changes. Keep transfer subprocesses and plugin
   sandboxing platform-specific until their implementations are tested. Add
   Windows hardware loopback and reconnect evidence.
3. **Linux repository candidates.** Start with Nixpkgs and AUR recipes against an
   immutable release. Add Flatpak after serial-device permission, BlueZ and
   plugin sandbox behavior have been demonstrated. Test Debian/Ubuntu, Fedora,
   Arch and NixOS; distinguish package formats from verified distributions.
4. **Android local USB.** Start with CDC ACM and one available USB-UART adapter;
   pin a publicly licensed driver implementation and keep its notices. Add
   other chips only after real-device tests. Preserve public mobile source and
   reproducible build instructions.
5. **iPad local USB investigation.** Prototype one adapter on suitable hardware
   and validate entitlements before committing to a native USB product. Keep
   the remote route usable for iPads outside those constraints.

## Next checkpoint

Use synthetic public test data. Run a command from iPad Safari and Android
Chrome through HTTPS to a gateway attached to a real USB-UART loopback. Verify
all 256 byte values, text/HEX sending, control lines supported by the adapter,
unplug/replug, background expiry and exclusive ownership. Record the OS,
adapter, build revision and results; a PTY or CI pass is not this checkpoint.

Distribution submissions remain a maintainer action after the release candidate
and checksums are verified. See [Linux distribution preparation](linux-distribution.md),
[Windows validation](windows-validation.md), and [remote serial](remote-serial.md).
