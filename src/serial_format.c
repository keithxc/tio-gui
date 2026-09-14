/* SPDX-License-Identifier: GPL-3.0-only */
#include "serial_format.h"
void tio_serial_format_init(TioSerialFormat *state, gint64 time_us) { *state = (TioSerialFormat){0, TRUE, time_us, time_us}; }
static gchar *stamp(TioSerialFormat *s, const char *format, gint64 now)
{
    gint64 usec = now;
    if (!g_strcmp0(format, "epoch")) return g_strdup_printf("[%" G_GINT64_FORMAT ".%06" G_GINT64_FORMAT "] ", now / G_TIME_SPAN_SECOND, now % G_TIME_SPAN_SECOND);
    if (!g_strcmp0(format, "24hour-start") || !g_strcmp0(format, "24hour-delta")) {
        usec = MAX(0, now - (!g_strcmp0(format, "24hour-start") ? s->start_us : s->previous_us));
        return g_strdup_printf("[%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ".%06" G_GINT64_FORMAT "] ",
            usec / G_TIME_SPAN_HOUR, usec / G_TIME_SPAN_MINUTE % 60, usec / G_TIME_SPAN_SECOND % 60, usec % G_TIME_SPAN_SECOND);
    }
    g_autoptr(GDateTime) date = g_date_time_new_from_unix_local(now / G_TIME_SPAN_SECOND);
    g_autofree gchar *base = g_date_time_format(date, !g_strcmp0(format, "iso8601") ? "%Y-%m-%dT%H:%M:%S%z" : "%H:%M:%S");
    return g_strdup_printf("[%s.%06" G_GINT64_FORMAT "] ", base, now % G_TIME_SPAN_SECOND);
}
GBytes *tio_serial_format(TioSerialFormat *s, const guint8 *bytes, gsize length,
                         gboolean timestamps, const char *format, gboolean strip, gint64 time_us)
{
    GByteArray *out = g_byte_array_new();
    for (gsize i = 0; i < length; i++) {
        guint8 c = bytes[i]; gboolean escape = s->escape || c == 27;
        if (!s->escape && c == 27) s->escape = 1;
        else if (s->escape == 1) s->escape = c == '[' ? 2 : c == ']' ? 3 : (c >= 0x20 && c <= 0x2f) ? 5 : 0;
        else if (s->escape == 2 && c >= 0x40 && c <= 0x7e) s->escape = 0;
        else if (s->escape == 3) { if (c == 7) s->escape = 0; else if (c == 27) s->escape = 4; }
        else if (s->escape == 4) s->escape = c == '\\' ? 0 : 3;
        else if (s->escape == 5 && c >= 0x30 && c <= 0x7e) s->escape = 0;
        if (strip && escape) continue;
        if (!escape && s->line_start) {
            if (timestamps) { g_autofree gchar *prefix = stamp(s, format, time_us); g_byte_array_append(out, (const guint8 *)prefix, strlen(prefix)); }
            s->previous_us = time_us; s->line_start = FALSE;
        }
        g_byte_array_append(out, &c, 1);
        if (!escape && c == '\n') s->line_start = TRUE;
    }
    return g_byte_array_free_to_bytes(out);
}
