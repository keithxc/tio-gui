# Local development workflow

After each successful application build, update the user's KDE application menu
entry to launch the latest build so they can inspect the result immediately.
Use a runnable package with its runtime dependencies, update
`~/.local/bin/tio-gui` and `~/.local/share/applications/io.github.keithxc.tio_gui.desktop`,
and refresh the KDE application cache with `kbuildsycoca6`.
Verify the launcher resolves to the new build. Do not stop a running serial
session automatically; tell the user to close and reopen the app when necessary.
