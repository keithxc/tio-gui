/* SPDX-License-Identifier: GPL-3.0-only */
#include "network.h"
#include <string.h>
struct _TioNetwork {
    guint refs;
    gboolean udp, closed, ready;
    GSocketConnection *connection;
    GCancellable *cancel;
    GSource *reader, *writer;
    GQueue pending;
    gsize queued, offset;
    TioNetworkCallback callback;
    gpointer data;
};
static TioNetwork *ref(TioNetwork *net) { ++net->refs; return net; }
static void unref(gpointer data)
{
    TioNetwork *net = data;
    if (--net->refs) return;
    g_clear_object(&net->connection); g_clear_object(&net->cancel);
    g_queue_clear_full(&net->pending, (GDestroyNotify)g_bytes_unref); g_free(net);
}
static void emit(TioNetwork *net, TioNetworkEvent event, const guint8 *bytes, gsize length)
{
    if (net->callback) net->callback(event, bytes, length, net->data);
}
static void stop(TioNetwork *net, const char *reason)
{
    if (net->closed) return;
    net->closed = TRUE; net->ready = FALSE; g_cancellable_cancel(net->cancel);
    if (net->reader) { g_source_destroy(net->reader); g_clear_pointer(&net->reader, g_source_unref); }
    if (net->writer) { g_source_destroy(net->writer); g_clear_pointer(&net->writer, g_source_unref); }
    if (net->connection) g_io_stream_close(G_IO_STREAM(net->connection), NULL, NULL);
    g_queue_clear_full(&net->pending, (GDestroyNotify)g_bytes_unref); net->queued = 0;
    emit(net, TIO_NET_STATUS, (const guint8 *)reason, strlen(reason));
}
void tio_network_free(TioNetwork *net)
{
    if (!net) return;
    net->callback = NULL; stop(net, "Disconnected"); unref(net);
}
static gboolean readable(GSocket *socket, GIOCondition condition, gpointer data)
{
    TioNetwork *net = ref(data);
    guint8 bytes[65536]; guint budget = 32;
    while (!net->closed && budget--) {
        g_autoptr(GError) error = NULL;
        gssize length = g_socket_receive(socket, (char *)bytes, sizeof bytes, NULL, &error);
        if (length < 0) {
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) stop(net, error->message);
            else if (condition & (G_IO_HUP | G_IO_ERR)) stop(net, "Peer disconnected");
            break;
        }
        if (!length && !net->udp) { stop(net, "Peer disconnected"); break; }
        emit(net, TIO_NET_RX, bytes, (gsize)length);
    }
    gboolean keep = !net->closed; unref(net); return keep;
}
static gboolean writable(GSocket *socket, GIOCondition condition, gpointer data)
{
    (void)condition; TioNetwork *net = ref(data); guint budget = 32;
    while (!net->closed && !g_queue_is_empty(&net->pending) && budget--) {
        GBytes *bytes = g_queue_peek_head(&net->pending);
        gsize length; const guint8 *payload = g_bytes_get_data(bytes, &length);
        g_autoptr(GError) error = NULL;
        gssize count = g_socket_send(socket, payload ? (const char *)payload + net->offset : "", length - net->offset, NULL, &error);
        if (count < 0) {
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) stop(net, error->message);
            break;
        }
        net->offset += (gsize)count;
        if (net->udp && net->offset != length) { stop(net, "Incomplete UDP datagram write"); break; }
        if (net->offset == length) {
            g_queue_pop_head(&net->pending); net->queued -= length; net->offset = 0;
            emit(net, TIO_NET_TX, payload, length); g_bytes_unref(bytes);
        }
    }
    gboolean keep = !net->closed && !g_queue_is_empty(&net->pending);
    if (!keep && net->writer) { g_source_destroy(net->writer); g_clear_pointer(&net->writer, g_source_unref); }
    unref(net); return keep;
}
static void connected(GObject *source, GAsyncResult *result, gpointer data)
{
    TioNetwork *net = data; g_autoptr(GError) error = NULL;
    GSocketConnection *connection = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), result, &error);
    if (net->closed) { g_clear_object(&connection); unref(net); return; }
    if (!connection) { stop(net, error->message); unref(net); return; }
    net->connection = connection;
    GSocket *socket = g_socket_connection_get_socket(connection); g_socket_set_blocking(socket, FALSE);
    g_socket_set_timeout(socket, 0);
    net->ready = TRUE;
    net->reader = g_socket_create_source(socket, G_IO_IN | G_IO_HUP | G_IO_ERR, net->cancel);
    g_source_set_callback(net->reader, G_SOURCE_FUNC(readable), ref(net), unref); g_source_attach(net->reader, NULL);
    const char *message = net->udp ? "UDP peer configured (no connection handshake)" : "TCP connected";
    emit(net, TIO_NET_STATUS, (const guint8 *)message, strlen(message)); unref(net);
}
TioNetwork *tio_network_connect(const char *host, guint16 port, gboolean udp,
                               TioNetworkCallback callback, gpointer data, GError **error)
{
    if (!host || !*host || strlen(host) > 253 || !port || strpbrk(host, " /\r\n\t")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Enter a hostname/address and port 1–65535"); return NULL;
    }
    TioNetwork *net = g_new0(TioNetwork, 1); net->refs = 1; net->udp = udp;
    net->callback = callback; net->data = data; net->cancel = g_cancellable_new();
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_socket_client_set_enable_proxy(client, FALSE); g_socket_client_set_timeout(client, 5);
    if (udp) { g_socket_client_set_socket_type(client, G_SOCKET_TYPE_DATAGRAM); g_socket_client_set_protocol(client, G_SOCKET_PROTOCOL_UDP); }
    g_autoptr(GSocketConnectable) address = g_network_address_new(host, port);
    g_socket_client_connect_async(client, address, net->cancel, connected, ref(net));
    return net;
}
gboolean tio_network_send(TioNetwork *net, GBytes *bytes, GError **error)
{
    gsize length = g_bytes_get_size(bytes);
    if (!net || !net->ready || net->closed || length > (net->udp ? 65507u : 65536u) || net->queued + length > 1048576) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK, "Network not ready, payload too large or 1 MiB send queue full"); return FALSE;
    }
    if (!length && !net->udp) return TRUE;
    /* Even empty UDP packets consume a queue slot. */
    if (net->pending.length >= 256) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK, "Network send queue is full"); return FALSE;
    }
    g_queue_push_tail(&net->pending, g_bytes_ref(bytes)); net->queued += length;
    if (!net->writer) {
        net->writer = g_socket_create_source(g_socket_connection_get_socket(net->connection), G_IO_OUT | G_IO_ERR, net->cancel);
        g_source_set_callback(net->writer, G_SOURCE_FUNC(writable), ref(net), unref); g_source_attach(net->writer, NULL);
    }
    return TRUE;
}
gboolean tio_network_ready(const TioNetwork *net) { return net && net->ready && !net->closed; }
