/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include <gtk/gtk.h>

typedef struct _TioHighlighter TioHighlighter;

TioHighlighter *tio_highlighter_new(GtkTextBuffer *buffer);
void tio_highlighter_feed(TioHighlighter *highlighter, const guint8 *data,
                          gsize length);
void tio_highlighter_clear(TioHighlighter *highlighter);
void tio_highlighter_free(TioHighlighter *highlighter);
