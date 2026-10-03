# Settings storage

Normal application saves alternate between two files in the user configuration
directory: `tio-gui/config.ini` and `config.ini.bk` for the Linux edition, or
`tio-gui/serial.ini` and `serial.ini.bk` for the native serial editions. The `.bk`
file is a peer slot and may contain the newest settings; it is not necessarily
an older backup. A new installation uses defaults silently when neither exists.

On startup, both slots are checked and the newest valid one is loaded. A missing,
truncated or damaged slot does not prevent recovery from the other valid slot.
If neither existing file is valid, loading reports an error and leaves the files
untouched. Saving also refuses to overwrite an existing store with no valid
copy. Preserve both damaged files for diagnosis, then move them aside before
importing a known-good export or creating a fresh configuration.

## Format and writes

Each new slot contains a storage header followed by the ordinary INI payload:

```ini
[storage]
version=1
sequence=0
sha256=<64 hexadecimal characters>

[general]
...
```

SHA-256 covers the decimal sequence number, a newline, and the exact payload
bytes. It detects accidental damage; it is not a signature or protection against
someone who can rewrite the files.

The sequence is an unsigned 64-bit counter. It wraps from
`18446744073709551615` to `0`; comparison uses modular ordering, so the wrapped
`0` is newer than its predecessor. Counters exactly half the range apart are
ambiguous and rejected with both copies preserved. Do not edit the counter or
checksum by hand.

The first save into an empty directory writes only the primary slot at sequence
`0`. The next save writes `.bk` at sequence `1`. Each later save replaces only
the slot that is not the current valid copy, retaining the other copy. A
cross-process lock protects the read/select/write transaction. The writer uses
a temporary file and requests GLib's consistent and durable replacement flags,
with mode `0600` on systems with POSIX file permissions. On Windows, replacement
can include a remove/rename gap for the inactive slot; the selected valid slot
remains untouched. Filesystem and storage hardware still determine the guarantees
available during power loss. See the [GLib write contract](https://docs.gtk.org/glib/func.file_set_contents_full.html).

Readers validate file snapshots without taking the writer lock. The lock
prevents simultaneous writers from choosing and replacing the same slot; it
does not merge settings from several open application instances. The last
successful full snapshot wins. Do not remove the lock file while an instance
may be saving.

## Existing files, exports and recovery

An older primary settings file without a storage header remains readable. Normal
saving migrates it into the two-slot format without a separate conversion
command, retaining the original primary until another successful save replaces
that slot.
Migration and restoration do not automatically connect restored serial tabs.

Use **Export settings** for a standalone backup, or **Export portable settings**
when sharing settings without local paths, device identities or history. These
exports remain ordinary single INI files, without a storage header or `.bk` and
lock companions. In macOS and the native Linux serial build, export snapshots all
current tabs without advancing the normal store. To edit settings manually,
edit an export and import it; editing a live slot would invalidate its checksum
or be superseded by the other slot.

Import reads exactly the selected file; it never silently falls back to a
neighboring `.bk`. In macOS and the native Linux serial build, the
`.before-import` backup contains the complete current in-memory workspace,
including unsaved tab controls, rather than a potentially stale primary slot.
The full Linux edition does not create this import backup. Import commits the
new store before replacing the visible workspace, so a save failure leaves the
current tabs intact.

This storage change does not add autosave triggers or a new save interval. It
protects snapshots at the application's existing save points; settings changed
after the last successful save can still be lost if the process crashes.
