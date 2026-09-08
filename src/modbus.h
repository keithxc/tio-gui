/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gio/gio.h>
typedef struct {
    gboolean tcp;
    const char *endpoint; /* TCP host or active tio Unix socket path */
    guint16 port, address, quantity, value, transaction;
    guint8 unit, function;
} TioModbusRequest;
GBytes *tio_modbus_frame(const TioModbusRequest *request, GError **error);
/* Full response validation: framing/CRC, unit/function, quantity and write echo. */
gchar *tio_modbus_decode(const TioModbusRequest *request, GBytes *response, GError **error);
void tio_modbus_request_async(const TioModbusRequest *request, GCancellable *cancel,
                             GAsyncReadyCallback callback, gpointer data);
gchar *tio_modbus_request_finish(GAsyncResult *result, GError **error);
