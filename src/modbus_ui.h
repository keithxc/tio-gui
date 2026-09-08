/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gtk/gtk.h>
/* Non-NULL socket binds this modal tool to an otherwise idle active tio session. */
GtkWidget *tio_modbus_window(GtkWindow *parent, const char *socket_path);
