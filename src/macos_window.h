/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gtk/gtk.h>

/* Extend the window content under a transparent native macOS title bar so the
 * session tabs share the traffic-light row. Returns FALSE on other platforms. */
gboolean tio_macos_unified_titlebar(GtkWindow *window);
