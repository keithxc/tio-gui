/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>

/* CRC is computed over payload before the optional line ending.
 * CRC-16/MODBUS and CRC-32/ISO-HDLC are appended least significant byte first. */
GByteArray *tio_payload_build(const char *text, gboolean hex, guint ending,
                             guint crc, GError **error);
gchar *tio_payload_preview(const GByteArray *bytes);
