# macOS 0.4.2 acceptance

2026-10-03. The native serial edition was built on Apple Silicon / macOS 27.0
with Apple Clang 21 and the project's pinned Nix development shell. The
deployment target and bundle minimum remain macOS 26.0. The build completed
without compiler warnings.

- All 12 existing CTest suites passed, including native serial, serial UI,
  terminal, transfer, capture, plugins and integrated serial tools. The settings
  tests cover the enabled default, legacy files without `restore-tabs`, an
  explicit `false` across save/load, and three tabs' order and configuration.
  The highlighter suite's separate interactive shell-edit case remains an
  explicit skip; this run does not claim that additional acceptance check.
- Both bundle version fields and the packaged executable report `0.4.2`.
  The executable is ARM64 and retains the bundle identifier
  `io.github.keithxc.tio_gui.serial`.
- The application and DMG were signed with Developer ID Application and hardened
  runtime. Apple accepted both notarization submissions. Stapled tickets validate,
  and Gatekeeper accepts both as `Notarized Developer ID`.
- The final ZIP was extracted into a separate directory. Its actual application
  passed `codesign --verify --deep --strict`, Gatekeeper and ticket validation.
  The DMG passed signature, Gatekeeper, ticket and disk-image checksum checks.
- The ZIP-extracted application ran with a system-only executable search path
  and a sandbox denying reads from Nix and Homebrew development roots. An
  isolated PTY received and logged exactly 2,065 bytes, including every byte
  value; GUI transmission produced exactly `bundle-smoke` followed by CR.
  The smoke script's old Connect-button coordinate hit the native title bar;
  the same checks passed after using the button position observed in the current
  window. The repository script was not changed for this verification.
- The DMG README was refreshed from the final serial-edition documentation,
  then the DMG was signed and notarized again. The ZIP remained unchanged.

Final local artifacts:

| Artifact | SHA-256 |
| --- | --- |
| `tio-gui-0.4.2-macos-arm64.zip` | `8f5e4e5c98d6afff6f4108a5e477d87f0def0e2e3b33c37fdbf44b9a9587917d` |
| `tio-gui-0.4.2-macos-arm64.dmg` | `783cfd50d3c8f56a2c1f3a925b0127fc9d824cc46e692ffe9c3fc3635266e804` |

This record covers prepared local artifacts, not uploaded-asset verification or
installation/activation. No existing serial session was stopped. Intel Macs and
execution on macOS 26 were not tested in this run. Physical USB adapters,
electrical framing, custom baud, DTR/RTS, unplug/replug and manual Chinese IME
candidate interaction still require separate hardware/manual acceptance. PTY
checks do not establish those results.
