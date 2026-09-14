# macOS 0.4.1 acceptance

2026-09-14. Apple Silicon / macOS 26.6.2; built with the mynix Nix devShell and
Apple Clang/SDK. This patch changes system-theme synchronization and packaging safeguards.

- Serial UI and integrated serial-tools regression tests: 2/2 passed.
- Verified startup against the host's actual macOS dark preference; inspected
  the rendered dark workspace. Switching explicit Light/Dark back to System
  restores the system value. The running timer corrects a deliberately altered
  GTK preference, and leaves explicit theme selections alone.
- Tests did not change the user's global macOS appearance. Actual global
  appearance toggling was not automated.
- Developer ID/hardened-runtime application and DMG notarized by Apple; tickets
  stapled, codesign and Gatekeeper checks passed.
- Packaged application PTY binary reception/logging and GUI text/CR transmission
  passed while a sandbox denied reads from Nix/Homebrew development roots.
- GitHub asset digests and downloaded checksums matched the local artifacts.

Full feature baseline: [0.4.0 acceptance](https://github.com/keithxc/tio-gui/blob/v0.4.0/docs/macos-0.4.0-acceptance.md). Hardware
USB/framing/line-control and manual Chinese IME candidate checks remain outside
this software verification. The console intentionally keeps the Linux dark
palette when the surrounding interface is light.
