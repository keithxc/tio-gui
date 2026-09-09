/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#include <pcre2.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>

/* Shared by the line consoles. Never scan rendered VTE text. Work is bounded
 * both inside PCRE and across matches, including patterns with empty matches. */
typedef struct { gint start, end; } TioSearchMatch;
typedef enum { TIO_SEARCH_NEXT, TIO_SEARCH_FIRST, TIO_SEARCH_REFRESH } TioSearchMode;
static int tio_search_deadline(pcre2_callout_block *block, void *data)
{
    (void)block;
    return g_get_monotonic_time() > *(gint64 *)data ? PCRE2_ERROR_CALLOUT : 0;
}
static gboolean tio_console_search(GtkTextView *view, const char *query,
                                   gboolean match_case, gboolean regex,
                                   gboolean forward, TioSearchMode reset,
                                   GtkLabel *feedback, GtkWidget *entry)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkTextIter first, last, a, b;
    gtk_text_buffer_get_bounds(buffer, &first, &last);
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, "tio-search-match");
    if (!tag) tag = gtk_text_buffer_create_tag(buffer, "tio-search-match",
                                               "background", "#e5b94b", "foreground", "#141414", NULL);
    gtk_text_tag_set_priority(tag, gtk_text_tag_table_get_size(table) - 1);
    gtk_text_buffer_remove_tag(buffer, tag, &first, &last);
    gtk_widget_remove_css_class(entry, "error");
    gtk_widget_set_tooltip_text(entry, NULL);
    gtk_label_set_text(feedback, "");
    if (!query || !*query) return FALSE;
    const char *failure = NULL;
    if (strlen(query) > 4096) failure = _("Search pattern is too long");
    if (gtk_text_buffer_get_char_count(buffer) > 8 * 1024 * 1024)
        failure = _("Search text is too large");
    if (failure) {
        gtk_label_set_text(feedback, failure);
        gtk_widget_add_css_class(entry, "error");
        return FALSE;
    }
    g_autofree gchar *pattern = regex ? g_strdup(query) : g_regex_escape_string(query, -1);
    int error; PCRE2_SIZE offset;
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
        PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE | PCRE2_AUTO_CALLOUT | PCRE2_NEVER_BACKSLASH_C |
        (match_case ? 0 : PCRE2_CASELESS), &error, &offset, NULL);
    if (!code) {
        PCRE2_UCHAR message[256]; pcre2_get_error_message(error, message, sizeof message);
        gtk_label_set_text(feedback, _("Invalid search pattern"));
        gtk_widget_set_tooltip_text(entry, (const char *)message);
        gtk_widget_add_css_class(entry, "error");
        return FALSE;
    }
    g_autofree gchar *text = gtk_text_buffer_get_text(buffer, &first, &last, TRUE);
    g_autoptr(GArray) matches = g_array_new(FALSE, FALSE, sizeof(TioSearchMatch));
    pcre2_match_data *match = pcre2_match_data_create_from_pattern(code, NULL);
    pcre2_match_context *context = pcre2_match_context_create(NULL);
    gint64 deadline = g_get_monotonic_time() + 50000;
    pcre2_set_match_limit(context, 10000); pcre2_set_depth_limit(context, 100);
    pcre2_set_heap_limit(context, 1024); pcre2_set_callout(context, tio_search_deadline, &deadline);
    gsize length = strlen(text), cursor = 0, previous_byte = 0;
    gint previous_char = 0;
    gboolean limited = FALSE;
    while (cursor <= length) {
        int rc = pcre2_match(code, (PCRE2_SPTR)text, length, cursor, 0, match, context);
        if (rc == PCRE2_ERROR_NOMATCH) break;
        if (rc < 0 || g_get_monotonic_time() > deadline || matches->len >= 10000) { limited = TRUE; break; }
        PCRE2_SIZE *positions = pcre2_get_ovector_pointer(match);
        /* Ignore zero-width results: there are no visible bytes to select. */
        if (positions[1] > positions[0]) {
            gint start = previous_char + (gint)g_utf8_strlen(text + previous_byte, (gssize)(positions[0] - previous_byte));
            gint end = start + (gint)g_utf8_strlen(text + positions[0], (gssize)(positions[1] - positions[0]));
            TioSearchMatch item = {start, end}; g_array_append_val(matches, item);
            previous_byte = positions[1]; previous_char = end;
        }
        cursor = positions[1];
        if (positions[0] == positions[1]) {
            if (cursor == length) break;
            cursor = (gsize)(g_utf8_next_char(text + cursor) - text);
        }
    }
    pcre2_match_context_free(context); pcre2_match_data_free(match); pcre2_code_free(code);
    if (limited) {
        gtk_label_set_text(feedback, _("Search limit reached; refine the pattern"));
        gtk_widget_add_css_class(entry, "error");
        return FALSE;
    }
    if (!matches->len) { gtk_label_set_text(feedback, _("No matches")); return FALSE; }
    gboolean selected = gtk_text_buffer_get_selection_bounds(buffer, &a, &b);
    gint anchor = reset == TIO_SEARCH_FIRST || !selected ? (forward ? 0 : G_MAXINT)
                                    : gtk_text_iter_get_offset(forward ? &b : &a);
    gint index = forward ? 0 : (gint)matches->len - 1;
    if (forward) {
        for (guint i = 0; i < matches->len; i++)
            if (g_array_index(matches, TioSearchMatch, i).start >= anchor) { index = (gint)i; break; }
    } else {
        for (gint i = (gint)matches->len - 1; i >= 0; i--)
            if (g_array_index(matches, TioSearchMatch, i).end <= anchor) { index = i; break; }
    }
    for (guint i = 0; i < matches->len; i++) {
        TioSearchMatch item = g_array_index(matches, TioSearchMatch, i);
        gtk_text_buffer_get_iter_at_offset(buffer, &a, item.start);
        gtk_text_buffer_get_iter_at_offset(buffer, &b, item.end);
        gtk_text_buffer_apply_tag(buffer, tag, &a, &b);
    }
    TioSearchMatch item = g_array_index(matches, TioSearchMatch, index);
    gtk_text_buffer_get_iter_at_offset(buffer, &a, item.start);
    gtk_text_buffer_get_iter_at_offset(buffer, &b, item.end);
    if (reset == TIO_SEARCH_REFRESH) {
        GtkTextIter selected_start, selected_end;
        index = -1;
        if (gtk_text_buffer_get_selection_bounds(buffer, &selected_start, &selected_end)) {
            for (guint i = 0; i < matches->len; i++) {
                TioSearchMatch candidate = g_array_index(matches, TioSearchMatch, i);
                if (candidate.start == gtk_text_iter_get_offset(&selected_start) &&
                    candidate.end == gtk_text_iter_get_offset(&selected_end)) { index = (gint)i; break; }
            }
        }
    } else {
        gtk_text_buffer_select_range(buffer, &a, &b);
        gtk_text_view_scroll_to_iter(view, &a, 0.15, FALSE, 0, 0);
    }
    g_autofree gchar *count = g_strdup_printf("%d / %u", index + 1, matches->len);
    gtk_label_set_text(feedback, count);
    return TRUE;
}

static void tio_console_font(GtkWidget *widget, guint points)
{
    GdkDisplay *display = gtk_widget_get_display(widget);
    GtkCssProvider *provider = g_object_get_data(G_OBJECT(display), "tio-font-provider");
    gtk_widget_add_css_class(widget, "tio-console-font");
    if (!provider) {
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(display,
            GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        g_object_set_data_full(G_OBJECT(display), "tio-font-provider", provider, g_object_unref);
    }
    g_autofree gchar *css = g_strdup_printf(
        ".tio-console-font, .tio-console-font text { font-size: %upt; }", points);
    gtk_css_provider_load_from_string(provider, css);
}
