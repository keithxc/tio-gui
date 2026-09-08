/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>
#include <string.h>

/* Bounded, streaming line editing shared by the highlight and analysis views.
 * This is not a screen emulator: horizontal positions count Unicode characters,
 * not terminal cells. Full-screen applications belong in VTE. */
typedef struct {
    guint cursor, parameter;
    gboolean esc, csi, osc, osc_esc, unsupported;
    gboolean private, cursor_hidden;
    guint continuation;
} TioTextLine;

static inline guint tio_text_line_next(GByteArray *line, guint position)
{
    if (position < line->len) ++position;
    while (position < line->len && (line->data[position] & 0xc0) == 0x80) ++position;
    return position;
}

static inline guint tio_text_line_previous(GByteArray *line, guint position)
{
    if (position) --position;
    while (position && position < line->len && (line->data[position] & 0xc0) == 0x80) --position;
    return position;
}

static inline void tio_text_line_right(TioTextLine *state, GByteArray *line,
                                      guint count, guint limit)
{
    while (count-- && state->cursor < limit)
        state->cursor = state->cursor < line->len
            ? tio_text_line_next(line, state->cursor) : state->cursor + 1;
}

/* Returns TRUE only for LF; callers publish the line and clear its bytes.
 * CR moves to column one without committing a new log entry. */
static inline gboolean tio_text_line_feed(TioTextLine *state, GByteArray *line,
                                          guint8 byte, guint limit)
{
    if (state->osc) {
        if (byte == 7 || (state->osc_esc && byte == '\\')) state->osc = FALSE;
        state->osc_esc = byte == 27;
        return FALSE;
    }
    if (byte == 27) {
        state->esc = TRUE;
        state->csi = FALSE;
        state->continuation = 0;
        return FALSE;
    }
    if (state->csi) {
        if (byte >= '0' && byte <= '9')
            state->parameter = MIN(limit, state->parameter * 10 + byte - '0');
        else if (byte >= 0x40 && byte <= 0x7e) {
            guint count = MAX(1u, state->parameter);
            if (!state->unsupported && state->private && state->parameter == 25) {
                if (byte == 'l') state->cursor_hidden = TRUE;
                if (byte == 'h') state->cursor_hidden = FALSE;
            }
            if (!state->unsupported && !state->private) switch (byte) {
            case 'K':
                if (state->parameter == 0)
                    g_byte_array_set_size(line, MIN(state->cursor, line->len));
                else if (state->parameter == 2) {
                    guint cells = state->cursor > line->len ? state->cursor - line->len : 0;
                    for (guint p = 0; p < MIN(state->cursor, line->len);
                         p = tio_text_line_next(line, p)) ++cells;
                    g_byte_array_set_size(line, 0);
                    state->cursor = cells;
                }
                else if (state->parameter == 1) {
                    /* Erasing preserves the cursor and characters to its right. */
                    guint end = tio_text_line_next(line, MIN(state->cursor, line->len));
                    guint cells = 0;
                    for (guint p = 0; p < end; p = tio_text_line_next(line, p)) ++cells;
                    guint tail = line->len - end;
                    guint cursor = state->cursor >= line->len
                        ? cells + state->cursor - line->len : cells - 1;
                    if (tail) memmove(line->data + cells, line->data + end, tail);
                    if (cells) memset(line->data, ' ', cells);
                    g_byte_array_set_size(line, cells + tail);
                    state->cursor = cursor;
                }
                break;
            case 'G': case '`':
                state->cursor = 0;
                tio_text_line_right(state, line, count - 1, limit);
                break;
            case 'C':
                tio_text_line_right(state, line, count, limit);
                break;
            case 'D':
                while (count-- && state->cursor)
                    state->cursor = tio_text_line_previous(line, state->cursor);
                break;
            default: break; /* SGR and screen controls do not become log text. */
            }
            state->csi = FALSE;
        } else if (byte == '?' && !state->private && !state->parameter)
            state->private = TRUE;
        else state->unsupported = TRUE; /* Multiple/intermediate parameters. */
        return FALSE;
    }
    if (state->esc) {
        state->esc = FALSE;
        state->csi = byte == '[';
        state->osc = byte == ']';
        state->osc_esc = FALSE;
        state->parameter = 0;
        state->unsupported = FALSE;
        state->private = FALSE;
        return FALSE;
    }
    if (byte < 32) state->continuation = 0;
    if (byte == '\r') { state->cursor = 0; return FALSE; }
    if (byte == '\n') { state->cursor = 0; return TRUE; }
    if (byte == '\b') {
        state->cursor = tio_text_line_previous(line, state->cursor);
        return FALSE;
    }
    if (byte < 32 && byte != '\t') return FALSE;
    if (byte == 127 || state->cursor >= limit) return FALSE;

    /* A UTF-8 leading byte replaces one old character. Its continuation bytes
     * are inserted, including when the character spans separate socket reads. */
    gboolean continuation = state->continuation && (byte & 0xc0) == 0x80;
    if (continuation) --state->continuation;
    else {
        state->continuation = byte >= 0xc2 && byte <= 0xdf ? 1 :
            byte >= 0xe0 && byte <= 0xef ? 2 : byte >= 0xf0 && byte <= 0xf4 ? 3 : 0;
        if (state->cursor < line->len) {
            guint end = tio_text_line_next(line, state->cursor);
            g_byte_array_remove_range(line, state->cursor, end - state->cursor);
        }
    }
    if (state->cursor > line->len) {
        guint old = line->len;
        g_byte_array_set_size(line, state->cursor);
        memset(line->data + old, ' ', state->cursor - old);
    }
    if (line->len >= limit) return FALSE;
    guint old = line->len;
    g_byte_array_set_size(line, old + 1);
    memmove(line->data + state->cursor + 1, line->data + state->cursor, old - state->cursor);
    line->data[state->cursor++] = byte;
    return FALSE;
}
