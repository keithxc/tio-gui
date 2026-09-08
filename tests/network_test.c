/* SPDX-License-Identifier: GPL-3.0-only */
#include "network.h"
#include <stdlib.h>
#include <string.h>
typedef struct { GByteArray *rx; guint empty, tx; } Sink;
static void event(TioNetworkEvent kind, const guint8 *bytes, gsize length, gpointer data)
{
    Sink *sink = data;
    if (kind == TIO_NET_RX) { g_byte_array_append(sink->rx, bytes, (guint)length); if (!length) ++sink->empty; }
    else if (kind == TIO_NET_TX) ++sink->tx;
    else g_print("%.*s\n", (int)length, bytes);
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    gboolean udp = atoi(argv[2]); Sink sink = {g_byte_array_new(), 0, 0};
    TioNetwork *net = tio_network_connect("127.0.0.1", (guint16)atoi(argv[1]), udp, event, &sink, NULL);
    g_assert_nonnull(net); gint64 until = g_get_monotonic_time() + 5000000;
    while (!tio_network_ready(net) && g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    g_assert_true(tio_network_ready(net));
    guint8 payload[256]; for (guint i = 0; i < 256; ++i) payload[i] = (guint8)i;
    g_autoptr(GBytes) bytes = g_bytes_new(payload, sizeof payload);
    g_assert_true(tio_network_send(net, bytes, NULL));
    while ((sink.rx->len < 256 || !sink.tx || (udp && !sink.empty)) && g_get_monotonic_time() < until) {
        g_main_context_iteration(NULL, FALSE); g_usleep(1000);
    }
    g_assert_cmpmem(sink.rx->data, sink.rx->len, payload, sizeof payload);
    g_assert_cmpuint(sink.tx, ==, 1);
    if (udp) g_assert_cmpuint(sink.empty, ==, 1);
    tio_network_free(net); g_byte_array_unref(sink.rx);
    /* Cancellation before DNS/connect finishes must not call a freed sink. */
    net = tio_network_connect("localhost", (guint16)atoi(argv[1]), udp, event, NULL, NULL);
    tio_network_free(net);
    until = g_get_monotonic_time() + 100000;
    while (g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    return 0;
}
