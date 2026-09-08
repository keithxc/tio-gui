/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gio/gio.h>
/* API v1: source defines transform(input). One UTF-8 input string, one output
 * string (JavaScript objects become JSON). No application/transport capabilities.
 * Lua=0, JavaScript=1. Source/input <=64KiB; output <=64KiB; deadline 2 seconds. */
void tio_plugin_run_async(guint language, const char *source, const char *input,
                         GCancellable *cancel, GAsyncReadyCallback callback, gpointer data);
gchar *tio_plugin_run_finish(GAsyncResult *result, GError **error);
