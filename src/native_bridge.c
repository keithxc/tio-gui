/* SPDX-License-Identifier: GPL-3.0-only */
#include "native_bridge.h"
#include <glib/gstdio.h>
#include <gio/gunixsocketaddress.h>
struct _TioNativeBridge {
    TioNativeSerial *serial;
    GSocketService *service;
    GSocketConnection *connection;
    GByteArray *received;
    gchar *directory, *path;
    gboolean eof, write_closed, reusable;
};
static gboolean incoming(GSocketService *service, GSocketConnection *connection, GObject *source, gpointer data)
{
    (void)service; (void)source; TioNativeBridge *bridge = data;
    if (bridge->connection && bridge->reusable && (bridge->eof ||
        (g_socket_condition_check(g_socket_connection_get_socket(bridge->connection), G_IO_HUP) & G_IO_HUP))) {
        g_io_stream_close(G_IO_STREAM(bridge->connection), NULL, NULL); g_clear_object(&bridge->connection);
        bridge->eof = bridge->write_closed = FALSE; g_byte_array_set_size(bridge->received, 0);
    }
    if (bridge->connection) return FALSE;
    bridge->connection = g_object_ref(connection);
    g_socket_set_blocking(g_socket_connection_get_socket(connection), FALSE); return TRUE;
}
TioNativeBridge *tio_native_bridge_new(TioNativeSerial *serial, GError **error)
{
    TioNativeBridge *bridge = g_new0(TioNativeBridge, 1); bridge->serial = serial;
    bridge->received = g_byte_array_new();
    bridge->directory = g_dir_make_tmp("tio-transfer-XXXXXX", error);
    g_autoptr(GSocketAddress) address = NULL;
    if (!bridge->directory) goto failed;
    bridge->path = g_build_filename(bridge->directory, "stream", NULL);
    bridge->service = g_socket_service_new();
    address = g_unix_socket_address_new(bridge->path);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(bridge->service), address, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, error)) goto failed;
    g_signal_connect(bridge->service, "incoming", G_CALLBACK(incoming), bridge);
    return bridge;
failed:
    tio_native_bridge_free(bridge); return NULL;
}
const char *tio_native_bridge_path(TioNativeBridge *bridge) { return bridge->path; }
gboolean tio_native_bridge_receive(TioNativeBridge *bridge, const guint8 *bytes, gsize length, GError **error)
{
    if (bridge->received->len + length > 8 * 1024 * 1024) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Transfer receive queue overflow"); return FALSE;
    }
    g_byte_array_append(bridge->received, bytes, length); return TRUE;
}
gboolean tio_native_bridge_tick(TioNativeBridge *bridge, GError **error)
{
    if (!bridge->connection) return TRUE;
    GSocket *socket = g_socket_connection_get_socket(bridge->connection);
    if (bridge->received->len && !bridge->write_closed) {
        g_autoptr(GError) local = NULL;
        gssize count = g_socket_send(socket, (const char *)bridge->received->data, bridge->received->len, NULL, &local);
        if (count > 0) g_byte_array_remove_range(bridge->received, 0, count);
        else if (local && !g_error_matches(local, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) { bridge->write_closed = TRUE; g_byte_array_set_size(bridge->received, 0); }
    }
    if (tio_native_idle(bridge->serial)) {
        char buffer[16384]; g_autoptr(GError) local = NULL;
        gssize count = g_socket_receive(socket, buffer, sizeof buffer, NULL, &local);
        if (count == 0) bridge->eof = TRUE;
        if (count > 0) {
            g_autoptr(GBytes) bytes = g_bytes_new(buffer, count);
            if (!tio_native_send(bridge->serial, bytes, error)) return FALSE;
        } else if (local && !g_error_matches(local, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) { g_propagate_error(error, g_steal_pointer(&local)); return FALSE; }
    }
    return TRUE;
}
void tio_native_bridge_free(TioNativeBridge *bridge)
{
    if (!bridge) return;
    if (bridge->service) { g_socket_service_stop(bridge->service); g_signal_handlers_disconnect_by_data(bridge->service, bridge); g_socket_listener_close(G_SOCKET_LISTENER(bridge->service)); }
    if (bridge->connection) g_io_stream_close(G_IO_STREAM(bridge->connection), NULL, NULL);
    g_clear_object(&bridge->connection); g_clear_object(&bridge->service); g_byte_array_unref(bridge->received);
    if (bridge->path) g_unlink(bridge->path);
    if (bridge->directory) g_rmdir(bridge->directory);
    g_free(bridge->directory); g_free(bridge->path); g_free(bridge);
}

gboolean tio_native_bridge_drained(TioNativeBridge *bridge) { return bridge->eof && tio_native_idle(bridge->serial); }

void tio_native_bridge_reusable(TioNativeBridge *bridge) { bridge->reusable = TRUE; }
