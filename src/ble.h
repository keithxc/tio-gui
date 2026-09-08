/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gtk/gtk.h>
GtkWidget *tio_ble_window(GtkWindow *parent);
/* Injectable bus connection for protocol acceptance tests. NULL uses system bus. */
GtkWidget *tio_ble_window_for_connection(GtkWindow *parent,GDBusConnection *connection);
