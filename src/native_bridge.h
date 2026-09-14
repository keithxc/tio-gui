/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "native_serial.h"
#include <gio/gio.h>
typedef struct _TioNativeBridge TioNativeBridge;
TioNativeBridge *tio_native_bridge_new(TioNativeSerial *serial, GError **error);
const char *tio_native_bridge_path(TioNativeBridge *bridge);
gboolean tio_native_bridge_receive(TioNativeBridge *bridge, const guint8 *bytes, gsize length, GError **error);
gboolean tio_native_bridge_tick(TioNativeBridge *bridge, GError **error);
void tio_native_bridge_free(TioNativeBridge *bridge);

gboolean tio_native_bridge_drained(TioNativeBridge *bridge);
void tio_native_bridge_reusable(TioNativeBridge *bridge);
