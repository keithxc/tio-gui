/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>

typedef struct _TioNativeSerial TioNativeSerial;
typedef struct {
    const char *device;
    guint baud, bits, stops;
    guint parity; /* none, odd, even */
    guint flow;   /* none, RTS/CTS, XON/XOFF */
    gboolean reconnect;
} TioNativeConfig;
typedef enum { TIO_NATIVE_STATUS, TIO_NATIVE_RX, TIO_NATIVE_TX } TioNativeEventKind;
typedef struct {
    TioNativeEventKind kind;
    GBytes *bytes;
    gchar *message;
    gboolean connected;
} TioNativeEvent;

/* All public calls belong to the UI thread. Only the worker touches the port.
 * Events and sends are bounded; bytes are never replayed after reconnect. */
GStrv tio_native_devices(void);
TioNativeSerial *tio_native_start(const TioNativeConfig *config, GError **error);
gboolean tio_native_send(TioNativeSerial *serial, GBytes *bytes, GError **error);
/* line 0: DTR, 1: RTS, 2: 250 ms Break (high ignored). */
gboolean tio_native_line(TioNativeSerial *serial, guint line, gboolean high, GError **error);
TioNativeEvent *tio_native_poll(TioNativeSerial *serial);
/* Finish the worker, then drain queued events before destroying the session. */
void tio_native_finish(TioNativeSerial *serial);
gboolean tio_native_pending(TioNativeSerial *serial);
void tio_native_event_free(TioNativeEvent *event);
void tio_native_stop(TioNativeSerial *serial);
