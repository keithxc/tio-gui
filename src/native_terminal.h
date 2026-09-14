/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gtk/gtk.h>
typedef struct _TioNativeTerminal TioNativeTerminal;
typedef void (*TioTerminalOutput)(const char *, gsize, gpointer);
TioNativeTerminal *tio_terminal_new(TioTerminalOutput output, gpointer data);
GtkTextView *tio_terminal_view(TioNativeTerminal *terminal);
void tio_terminal_feed(TioNativeTerminal *terminal, const guint8 *bytes, gsize length);
void tio_terminal_render(TioNativeTerminal *terminal, gboolean follow);
gboolean tio_terminal_key(TioNativeTerminal *terminal, guint key, GdkModifierType modifiers);
void tio_terminal_paste(TioNativeTerminal *terminal, const char *text);
void tio_terminal_clear(TioNativeTerminal *terminal);
void tio_terminal_free(TioNativeTerminal *terminal);
