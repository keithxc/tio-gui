/* SPDX-License-Identifier: GPL-3.0-only */
#include "connection_state.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>

gchar *tio_connection_device(GPid pid, const char *preferred_device)
{
    if (pid <= 0) return NULL;
    g_autofree gchar *directory = g_strdup_printf("/proc/%d/fd", (int)pid);
    g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
    if (!dir) return NULL;
    GStatBuf preferred;
    gboolean has_preferred = preferred_device && g_stat(preferred_device, &preferred) == 0 && S_ISCHR(preferred.st_mode);
    const char *entry;
    while ((entry = g_dir_read_name(dir))) {
        g_autofree gchar *path = g_build_filename(directory, entry, NULL);
        g_autofree gchar *target = g_file_read_link(path, NULL);
        if (!target || g_str_has_suffix(target, " (deleted)")) continue;
        GStatBuf info;
        if (g_stat(path, &info) != 0 || !S_ISCHR(info.st_mode)) continue;
        if (has_preferred && info.st_rdev == preferred.st_rdev) return g_strdup(preferred_device);
        if (g_str_has_prefix(target, "/dev/tty") && !g_str_equal(target, "/dev/tty") &&
            g_file_test(target, G_FILE_TEST_EXISTS)) return g_strdup(target);
    }
    return NULL;
}
