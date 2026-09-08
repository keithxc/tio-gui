/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gio/gio.h>
typedef struct _TioNetwork TioNetwork;
typedef enum { TIO_NET_STATUS, TIO_NET_RX, TIO_NET_TX } TioNetworkEvent;
typedef void (*TioNetworkCallback)(TioNetworkEvent event, const guint8 *data, gsize length, gpointer user_data);
TioNetwork *tio_network_connect(const char *host, guint16 port, gboolean udp,
                               TioNetworkCallback callback, gpointer data, GError **error);
gboolean tio_network_send(TioNetwork *network, GBytes *bytes, GError **error);
gboolean tio_network_ready(const TioNetwork *network);
void tio_network_free(TioNetwork *network);
