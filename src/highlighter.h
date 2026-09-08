/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include <gtk/gtk.h>

typedef struct _TioHighlighter TioHighlighter;

TioHighlighter *tio_highlighter_new(GtkTextBuffer *buffer);
void tio_highlighter_feed(TioHighlighter *highlighter, const guint8 *data,
                          gsize length);
void tio_highlighter_clear(TioHighlighter *highlighter);
void tio_highlighter_free(TioHighlighter *highlighter);

/* Replace up to 32 bounded regex/color rules atomically; empty text resets. */
gboolean tio_highlighter_rules(TioHighlighter *highlighter, const char *ini, GError **error);
