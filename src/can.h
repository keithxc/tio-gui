/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gio/gio.h>
#include <linux/can.h>
typedef struct _TioCan TioCan;
typedef void (*TioCanCallback)(const struct canfd_frame *frame, gboolean fd, const char *error, gpointer data);
TioCan *tio_can_open(const char *interface, TioCanCallback callback, gpointer data, GError **error);
gboolean tio_can_send(TioCan *bus, guint32 id, gboolean extended, gboolean fd, gboolean brs,
                      gboolean rtr, const guint8 *bytes, gsize length, GError **error);
void tio_can_free(TioCan *bus);
