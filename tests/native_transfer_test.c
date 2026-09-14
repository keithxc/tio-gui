/* SPDX-License-Identifier: GPL-3.0-only */
#include "native_bridge.h"
#include "transfer.h"
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    g_autoptr(GError) error = NULL;
    TioNativeConfig config = {.device=argv[1], .baud=115200, .bits=8, .stops=1};
    TioNativeSerial *serial = tio_native_start(&config, &error); if (!serial) return 1;
    gboolean connected = FALSE; gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!connected && g_get_monotonic_time() < deadline) {
        TioNativeEvent *event;
        while ((event = tio_native_poll(serial))) { connected |= event->kind == TIO_NATIVE_STATUS && event->connected; tio_native_event_free(event); }
        g_usleep(1000);
    }
    if (!connected) return 1;
    TioNativeBridge *bridge = tio_native_bridge_new(serial, &error); if (!bridge) { g_printerr("%s\n", error->message); return 1; }
    TioTransfer *transfer = tio_transfer_start(tio_native_bridge_path(bridge), argv[2], atoi(argv[3]), atoi(argv[4]), 12, &error);
    if (!transfer) { g_printerr("%s\n", error->message); return 1; }
    g_print("READY\n");
    gint64 drain_deadline = 0;
    while (TRUE) {
        if (!tio_transfer_active(transfer)) {
            if (!drain_deadline) drain_deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
            if (tio_native_bridge_drained(bridge) || g_get_monotonic_time() > drain_deadline) break;
        }
        while (g_main_context_iteration(NULL, FALSE)) {}
        TioNativeEvent *event;
        while ((event = tio_native_poll(serial))) {
            if (event->kind == TIO_NATIVE_RX) {
                gsize length; const guint8 *bytes = g_bytes_get_data(event->bytes, &length);
                if (!tio_native_bridge_receive(bridge, bytes, length, &error)) break;
            } else if (event->kind == TIO_NATIVE_STATUS && !event->connected) tio_transfer_cancel(transfer);
            tio_native_event_free(event);
        }
        if (error || !tio_native_bridge_tick(bridge, &error)) { tio_transfer_cancel(transfer); break; }
        g_usleep(1000);
    }
    gboolean success = tio_transfer_success(transfer); g_print("%s\n", tio_transfer_status(transfer));
    if (error) g_printerr("Bridge: %s\n", error->message);
    tio_transfer_free(transfer); tio_native_bridge_free(bridge); tio_native_stop(serial); return success ? 0 : 1;
}
