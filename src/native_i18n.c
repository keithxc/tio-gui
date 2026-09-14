/* SPDX-License-Identifier: GPL-3.0-only */
#include <glib.h>
#include <string.h>
#include "native_catalog.h"
gboolean native_chinese = TRUE;
const char *native_translate(const char *id) G_GNUC_FORMAT(1);
const char *native_translate(const char *id)
{
    if (!native_chinese) return id;
    gsize low = 0, high = G_N_ELEMENTS(native_catalog);
    while (low < high) {
        gsize mid = low + (high - low) / 2;
        int order = strcmp(id, native_catalog[mid].id);
        if (!order) return native_catalog[mid].value;
        if (order < 0) high = mid; else low = mid + 1;
    }
    return id;
}
