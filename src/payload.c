/* SPDX-License-Identifier: GPL-3.0-only */
#include "payload.h"
#include <string.h>

GByteArray *tio_payload_build(const char *text, gboolean hex, guint ending,
                             guint crc, GError **error)
{
    g_autoptr(GByteArray) bytes = g_byte_array_new();
    if (!text || strlen(text) > 65536 || ending > 3 || crc > 3) {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                            "Invalid payload options or payload exceeds 64 KiB");
        return NULL;
    }
    for (gsize i = 0; text[i]; ++i) {
        guint8 value = (guint8)text[i];
        if (hex) {
            if (g_ascii_isspace(text[i])) continue;
            if (!g_ascii_isxdigit(text[i]) || !g_ascii_isxdigit(text[i + 1])) goto invalid;
            value = (guint8)(g_ascii_xdigit_value(text[i]) * 16 + g_ascii_xdigit_value(text[i + 1]));
            ++i;
        } else if (text[i] == '\\') {
            switch (text[++i]) {
            case 'r': value = '\r'; break;
            case 'n': value = '\n'; break;
            case 't': value = '\t'; break;
            case 'e': value = 27; break;
            case '\\': value = '\\'; break;
            case 'x':
                if (!g_ascii_isxdigit(text[i + 1]) || !g_ascii_isxdigit(text[i + 2])) goto invalid;
                value = (guint8)(g_ascii_xdigit_value(text[i + 1]) * 16 + g_ascii_xdigit_value(text[i + 2]));
                i += 2;
                break;
            default: goto invalid;
            }
        }
        g_byte_array_append(bytes, &value, 1);
    }
    if (crc) {
        guint32 value = crc == 1 ? 0 : crc == 2 ? 0xffff : 0xffffffff;
        for (guint i = 0; i < bytes->len; ++i) {
            value ^= bytes->data[i];
            for (guint bit = 0; bit < 8; ++bit) {
                if (crc == 1) value = ((value << 1) ^ ((value & 0x80) ? 0x07u : 0)) & 0xff;
                else value = (value >> 1) ^ ((value & 1) ? (crc == 2 ? 0xa001u : 0xedb88320u) : 0);
            }
        }
        if (crc == 3) value ^= 0xffffffff;
        guint count = crc == 1 ? 1 : crc == 2 ? 2 : 4;
        for (guint i = 0; i < count; ++i) {
            guint8 byte = (guint8)(value >> (8 * i));
            g_byte_array_append(bytes, &byte, 1);
        }
    }
    if (ending == 2 || ending == 3) g_byte_array_append(bytes, (const guint8 *)"\r", 1);
    if (ending == 1 || ending == 3) g_byte_array_append(bytes, (const guint8 *)"\n", 1);
    return g_steal_pointer(&bytes);
invalid:
    g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                        "Use HEX byte pairs or valid text escapes: \\r \\n \\t \\e \\\\ \\xNN");
    return NULL;
}

gchar *tio_payload_preview(const GByteArray *bytes)
{
    GString *result = g_string_new(NULL);
    for (guint i = 0; i < MIN(bytes->len, 256u); ++i)
        g_string_append_printf(result, "%s%02X", i ? " " : "", bytes->data[i]);
    if (bytes->len > 256) g_string_append(result, " …");
    g_string_append_printf(result, " (%u bytes)", bytes->len);
    return g_string_free(result, FALSE);
}
