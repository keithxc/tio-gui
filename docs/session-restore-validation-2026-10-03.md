# Linux session restoration — 2026-10-03

The Linux edition previously defaulted `restore-tabs` to false. Its window-close
handler also cleared the saved tab list when this startup option was disabled.
Consequently, a normal three-tab workspace could reopen with just one default
tab. Restoring configurations does not connect serial ports.

The fix enables restoration for new configurations and legacy configurations
without the key. Explicit `restore-tabs=false` values remain respected. Linux
now saves the open tabs regardless of that startup preference, in notebook
order. Closing an individual tab still removes it from the next workspace.
Closing the last tab captures preferences before releasing its controls. The
restoration sequence also preserves a saved disabled timestamp setting when
logging is enabled.

## Regression evidence

The `session-restore` CTest runs the real Linux GTK application lifecycle with
an isolated `XDG_CONFIG_HOME`, Xvfb and a private D-Bus session. No physical
serial port is opened. Each reopen creates a new application instance and
reloads settings from disk.

- On the previous implementation, the new regression failed after reopening:
  expected three tabs, observed one.
- With the fix, the same test passed: three tabs retained their order, names,
  unavailable device paths, individual baud rates (including custom 250000),
  quick commands, reconnect options and logging/timestamp choice.
- Closing one tab restored only the remaining two. An explicit startup opt-out
  opened one tab but still saved the previous workspace on exit. Changing the
  preference then closing the final tab also persisted correctly.
- Every restored tab remained disconnected, with no serial subprocess or log
  recording started.
- Four settings regressions cover new defaults, a missing legacy key, explicit
  false round trips, and tab configuration/order round trips. Together with the
  existing quick-command test, all five passed on Linux and Apple Silicon.
- The complete Linux desktop build and all 11 registered CTest groups passed
  on x86-64 Linux, including `session-restore` running under Xvfb. The Nix
  desktop package also built successfully.

Without a display, `session-restore` reports a CTest skip (77). The Linux CI
workflow supplies Xvfb, so the UI lifecycle regression executes there.

## Existing installations

An older generated `restore-tabs=false` cannot be distinguished from an
intentional opt-out. In application settings, enable **Reopen sessions on
startup** before closing the main window. A running older process keeps its own
copy of settings and can overwrite edits made directly to its configuration
file; do not change that file underneath a live serial session.

If an older configuration has no `tab:N` sections, it contains no previous tab
workspace to restore. Enable restoration and configure the desired tabs before
closing the main window normally.
