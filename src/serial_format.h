/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>
typedef struct { guint escape; gboolean line_start; gint64 start_us, previous_us; } TioSerialFormat;
void tio_serial_format_init(TioSerialFormat *format, gint64 time_us);
GBytes *tio_serial_format(TioSerialFormat *state, const guint8 *bytes, gsize length,
                         gboolean timestamps, const char *format, gboolean strip, gint64 time_us);
