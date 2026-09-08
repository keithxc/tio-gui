/* SPDX-License-Identifier: GPL-3.0-only */
#include "payload.h"
#include <string.h>
static void vectors(void)
{
    const guint8 expected[][4] = {{0}, {0xf4}, {0x37, 0x4b}, {0x26, 0x39, 0xf4, 0xcb}};
    for (guint crc = 1; crc <= 3; ++crc) {
        g_autoptr(GByteArray) bytes = tio_payload_build("123456789", FALSE, 3, crc, NULL);
        guint n = crc == 1 ? 1 : crc == 2 ? 2 : 4;
        g_assert_nonnull(bytes);
        g_assert_cmpuint(bytes->len, ==, 11 + n);
        g_assert_cmpmem(bytes->data + 9, n, expected[crc], n);
        g_assert_cmpmem(bytes->data + 9 + n, 2, "\r\n", 2);
    }
}
static void inputs(void)
{
    g_autoptr(GByteArray) a = tio_payload_build("00 14 FF 0d0A", TRUE, 0, 0, NULL);
    g_autoptr(GByteArray) b = tio_payload_build("\\x00\\x14\\xFF\\r\\n", FALSE, 0, 0, NULL);
    g_assert_nonnull(a);
    g_assert_nonnull(b);
    g_assert_cmpmem(a->data, a->len, b->data, b->len);
    const char *invalid[] = {"\\", "\\x", "\\x0", "\\xGG", "\\z"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        g_autoptr(GError) error = NULL;
        g_assert_null(tio_payload_build(invalid[i], FALSE, 0, 0, &error));
        g_assert_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
    }
    g_assert_null(tio_payload_build("F", TRUE, 0, 0, NULL));
    g_assert_null(tio_payload_build("FG", TRUE, 0, 0, NULL));
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/payload/crc-vectors", vectors);
    g_test_add_func("/payload/inputs", inputs);
    return g_test_run();
}
