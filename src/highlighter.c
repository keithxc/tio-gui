/* SPDX-License-Identifier: GPL-3.0-only */

#include "highlighter.h"
#include "text_line.h"

#define TIO_HIGHLIGHT_MAX_LINES 10000
/* Also bound long-line histories (at most 8 MiB of UTF-8 text). */
#define TIO_HIGHLIGHT_MAX_CHARS (2 * 1024 * 1024)
#define TIO_HIGHLIGHT_MAX_LINE_BYTES 16384
#define TIO_HIGHLIGHT_MAX_MATCHES_PER_RULE 512

typedef struct {
  const char *name;
  const char *pattern;
  const char *foreground;
  gboolean bold;
} HighlightRule;

/* These are deliberately small, bounded semantic rules rather than copied
   editor syntax. They cover the useful classes exposed by terminal tools such
   as WindTerm while remaining predictable for an untrusted serial stream. */
static const HighlightRule rules[] = {
    {"punctuation", "[][(){}]", "#8b949e", FALSE},
    {"bracketed", "\\[[^]\\r\\n]{1,64}\\]", "#d2a8ff", FALSE},
    {"option", "(?<![[:alnum:]_])--?[[:alpha:]][[:alnum:]_-]*", "#d2a8ff",
     FALSE},
    {"path",
     "(?<![[:alnum:]_])(?:[A-Za-z]:[\\\\/]|/)(?:[^[:space:]<>:|?*]+[\\\\/]?)+",
     "#a5d6ff", FALSE},
    {"url", "\\b(?:https?|ftps?|file)://[^[:space:]<>\\\"]+", "#58a6ff", FALSE},
    {"email", "\\b[[:alnum:]._%+-]+@[[:alnum:].-]+\\.[[:alpha:]]{2,}\\b",
     "#58a6ff", FALSE},
    {"ipv4", "\\b(?:[0-9]{1,3}\\.){3}[0-9]{1,3}(?::[0-9]{1,5})?\\b", "#79c0ff",
     FALSE},
    {"ipv6",
     "(?<![[:xdigit:]:])(?:[[:xdigit:]]{1,4}:){2,7}[[:xdigit:]]{0,4}(?![[:"
     "xdigit:]:])",
     "#79c0ff", FALSE},
    {"datetime",
     "\\b(?:[0-9]{4}[-/][0-9]{1,2}[-/][0-9]{1,2}(?:[T "
     "]|\\s+))?[0-9]{1,2}:[0-9]{2}(?::[0-9]{2}(?:[.,][0-9]+)?)?(?:Z|[+-][0-9]{"
     "2}:?[0-9]{2})?\\b",
     "#ffa657", FALSE},
    {"duration",
     "\\b[0-9]+(?:\\.[0-9]+)?\\s*(?:ns|us|µs|ms|sec(?:ond)?s?|min(?:ute)?s?|"
     "hours?|days?)\\b",
     "#ffa657", FALSE},
    {"key", "\\b[[:alpha:]_][[:alnum:]_.-]*(?=\\s*[:=])", "#7ee787", FALSE},
    {"hex", "\\b(?:0[xX][[:xdigit:]]+|[[:xdigit:]]{4,8}[hH])\\b", "#f2cc60",
     FALSE},
    {"source-location",
     "\\b[[:alnum:]_.+-]+\\.(?:c|cc|cpp|cxx|h|hh|hpp):[0-9]+\\b", "#76e3ea",
     FALSE},
    {"number",
     "(?<![[:alnum:]_.])[-+]?[0-9]+(?:\\.[0-9]+)?(?:[eE][-+]?[0-9]+)?(?![[:"
     "alnum:]_.])",
     "#f2cc60", FALSE},
    {"literal", "\\b(?:true|false|null|none|nil|yes|no|on|off)\\b", "#d2a8ff",
     TRUE},
    {"info", "\\b(?:INFO|NOTICE|DEBUG|TRACE)\\b", "#58a6ff", TRUE},
    {"success", "\\b(?:PASS(?:ED)?|OK|SUCCESS|READY|DONE)\\b", "#3fb950", TRUE},
    {"warning", "\\b(?:WARN(?:ING)?|TIMEOUT|RETRY|DEPRECATED)\\b", "#d29922",
     TRUE},
    {"error",
     "\\b(?:ERROR|FAIL(?:ED|URE)?|FATAL|PANIC|CRITICAL|ASSERT(?:ION)?)\\b",
     "#f85149", TRUE},
};

typedef struct {
  GRegex *regex;
  GtkTextTag *tag;
} CompiledRule;

struct _TioHighlighter {
  GtkTextBuffer *buffer;
  GArray *compiled;
  GByteArray *line;
  TioTextLine editing;
  gint partial_chars;
  gchar *rendered;
};

static void trim_scrollback(TioHighlighter *highlighter) {
  gint lines = gtk_text_buffer_get_line_count(highlighter->buffer);
  gint chars = gtk_text_buffer_get_char_count(highlighter->buffer);
  if (lines <= TIO_HIGHLIGHT_MAX_LINES && chars <= TIO_HIGHLIGHT_MAX_CHARS)
    return;
  GtkTextIter start, keep;
  gtk_text_buffer_get_start_iter(highlighter->buffer, &start);
  gtk_text_buffer_get_iter_at_line(highlighter->buffer, &keep,
                                   MAX(0, lines - TIO_HIGHLIGHT_MAX_LINES));
  if (chars - gtk_text_iter_get_offset(&keep) > TIO_HIGHLIGHT_MAX_CHARS) {
    gtk_text_buffer_get_iter_at_offset(highlighter->buffer, &keep,
                                       chars - TIO_HIGHLIGHT_MAX_CHARS);
    /* Retain whole lines, including the editable partial tail. */
    if (!gtk_text_iter_starts_line(&keep)) gtk_text_iter_forward_line(&keep);
  }
  /* Delete plain text: deleting thousands of tag toggles is much more costly. */
  gtk_text_buffer_remove_all_tags(highlighter->buffer, &start, &keep);
  gtk_text_buffer_delete(highlighter->buffer, &start, &keep);
}

static void apply_rules(TioHighlighter *highlighter, gint line_start,
                        const char *text) {
  for (guint rule_index = 0; rule_index < highlighter->compiled->len;
       ++rule_index) {
    CompiledRule *rule =
        &g_array_index(highlighter->compiled, CompiledRule, rule_index);
    g_autoptr(GMatchInfo) match = NULL;
    g_regex_match(rule->regex, text, 0, &match);
    guint matches = 0;
    gint previous_byte = 0, previous_char = 0;
    while (g_match_info_matches(match) &&
           matches++ < TIO_HIGHLIGHT_MAX_MATCHES_PER_RULE) {
      gint byte_start = 0;
      gint byte_end = 0;
      if (g_match_info_fetch_pos(match, 0, &byte_start, &byte_end) &&
          byte_end > byte_start) {
        gint char_start =
            previous_char + (gint)g_utf8_strlen(text + previous_byte, byte_start - previous_byte);
        gint char_end = char_start + (gint)g_utf8_strlen(text + byte_start, byte_end - byte_start);
        previous_byte = byte_end;
        previous_char = char_end;
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_offset(highlighter->buffer, &start,
                                           line_start + char_start);
        gtk_text_buffer_get_iter_at_offset(highlighter->buffer, &end,
                                           line_start + char_end);
        gtk_text_buffer_apply_tag(highlighter->buffer, rule->tag, &start, &end);
      }
      if (!g_match_info_next(match, NULL)) {
        break;
      }
    }
  }
}

/* Keep the unchanged prefix in the buffer, including across UTF-8 fragments.
   Replacing only the differing suffix avoids deleting/reinserting a whole
   visible prompt on every small serial read. */
static void render_line(TioHighlighter *highlighter) {
  g_autofree char *valid = g_utf8_make_valid(
      (const char *)highlighter->line->data, highlighter->line->len);
  const char *old = highlighter->rendered ? highlighter->rendered : "";
  if (strcmp(old, valid) == 0) return;
  gint common = 0;
  const char *a = old, *b = valid;
  while (*a && *b && g_utf8_get_char(a) == g_utf8_get_char(b)) {
    ++common;
    a = g_utf8_next_char(a);
    b = g_utf8_next_char(b);
  }
  GtkTextIter start, end;
  gtk_text_buffer_get_end_iter(highlighter->buffer, &end);
  gint offset = gtk_text_iter_get_offset(&end) - highlighter->partial_chars;
  gtk_text_buffer_get_iter_at_offset(highlighter->buffer, &start, offset + common);
  if (common < highlighter->partial_chars)
    gtk_text_buffer_delete(highlighter->buffer, &start, &end);
  gtk_text_buffer_get_end_iter(highlighter->buffer, &end);
  if (*b) gtk_text_buffer_insert(highlighter->buffer, &end, b, -1);
  gtk_text_buffer_get_iter_at_offset(highlighter->buffer, &start, offset);
  gtk_text_buffer_get_end_iter(highlighter->buffer, &end);
  /* A previously complete token can stop matching when its suffix arrives. */
  gtk_text_buffer_remove_all_tags(highlighter->buffer, &start, &end);
  apply_rules(highlighter, offset, valid);
  highlighter->partial_chars = (gint)g_utf8_strlen(valid, -1);
  g_free(highlighter->rendered);
  highlighter->rendered = g_steal_pointer(&valid);
}

static void flush_line(TioHighlighter *highlighter) {
  render_line(highlighter);
  GtkTextIter end, start;
  gtk_text_buffer_get_end_iter(highlighter->buffer, &end);
  gint offset = gtk_text_iter_get_offset(&end);
  gtk_text_buffer_insert(highlighter->buffer, &end, "\n", 1);
  gtk_text_buffer_get_iter_at_offset(highlighter->buffer, &start, offset);
  gtk_text_buffer_remove_all_tags(highlighter->buffer, &start, &end);
  highlighter->partial_chars = 0;
  g_clear_pointer(&highlighter->rendered, g_free);
  g_byte_array_set_size(highlighter->line, 0);
}

TioHighlighter *tio_highlighter_new(GtkTextBuffer *buffer) {
  g_return_val_if_fail(GTK_IS_TEXT_BUFFER(buffer), NULL);
  TioHighlighter *highlighter = g_new0(TioHighlighter, 1);
  highlighter->buffer = g_object_ref(buffer);
  highlighter->compiled = g_array_new(FALSE, FALSE, sizeof(CompiledRule));
  highlighter->line = g_byte_array_sized_new(256);

  for (guint index = 0; index < G_N_ELEMENTS(rules); ++index) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar *bounded = g_strconcat("(*LIMIT_MATCH=1000)(*LIMIT_DEPTH=100)(*LIMIT_HEAP=1024)", rules[index].pattern, NULL);
    GRegex *regex = g_regex_new(bounded,
                                G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, &error);
    if (regex == NULL) {
      g_warning("Could not compile highlight rule %s: %s", rules[index].name,
                error->message);
      continue;
    }
    GtkTextTag *tag = gtk_text_buffer_create_tag(
        buffer, rules[index].name, "foreground", rules[index].foreground,
        "weight", rules[index].bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
        NULL);
    CompiledRule compiled = {.regex = regex, .tag = tag};
    g_array_append_val(highlighter->compiled, compiled);
  }
  return highlighter;
}

gboolean tio_highlighter_rules(TioHighlighter *highlighter, const char *ini, GError **error)
{
  if (!ini || strlen(ini) > 65536) {
    g_set_error_literal(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE, "Rules must fit in 64 KiB"); return FALSE;
  }
  g_autoptr(GKeyFile) file = g_key_file_new();
  if (*ini && !g_key_file_load_from_data(file, ini, strlen(ini), G_KEY_FILE_NONE, error)) return FALSE;
  gsize count; g_auto(GStrv) groups = g_key_file_get_groups(file, &count);
  if (count > 32) { g_set_error_literal(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE, "At most 32 highlight rules"); return FALSE; }
  g_autoptr(GPtrArray) patterns = g_ptr_array_new_with_free_func((GDestroyNotify)g_regex_unref);
  g_autoptr(GPtrArray) colors = g_ptr_array_new_with_free_func(g_free);
  gboolean bold[32] = {FALSE};
  for (guint i = 0; i < count; ++i) {
    g_autofree gchar *pattern = g_key_file_get_string(file, groups[i], "pattern", error);
    if (!pattern) return FALSE;
    g_autofree gchar *color = g_key_file_get_string(file, groups[i], "color", error);
    if (!color) return FALSE;
    GdkRGBA rgba;
    if (!g_str_has_prefix(groups[i], "rule:") || strlen(pattern) > 1024 || !*pattern || !gdk_rgba_parse(&rgba, color)) {
      g_set_error_literal(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE, "Each [rule:name] needs a pattern up to 1024 bytes and a valid color"); return FALSE;
    }
    bold[i] = g_key_file_get_boolean(file, groups[i], "bold", NULL);
    g_autofree gchar *bounded = g_strconcat("(*LIMIT_MATCH=1000)(*LIMIT_DEPTH=100)(*LIMIT_HEAP=1024)", pattern, NULL);
    GRegex *regex = g_regex_new(bounded, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, error);
    if (!regex) return FALSE;
    g_ptr_array_add(patterns, regex); g_ptr_array_add(colors, g_steal_pointer(&color));
  }
  /* Validate the entire replacement before removing the previous custom rules. */
  while (highlighter->compiled->len > G_N_ELEMENTS(rules)) {
    CompiledRule *rule = &g_array_index(highlighter->compiled, CompiledRule, highlighter->compiled->len - 1);
    gtk_text_tag_table_remove(gtk_text_buffer_get_tag_table(highlighter->buffer), rule->tag);
    g_regex_unref(rule->regex); g_array_set_size(highlighter->compiled, highlighter->compiled->len - 1);
  }
  for (guint i = 0; i < count; ++i) {
    GtkTextTag *tag = gtk_text_buffer_create_tag(highlighter->buffer, groups[i], "foreground", g_ptr_array_index(colors, i),
        "weight", bold[i] ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, NULL);
    CompiledRule compiled = {g_regex_ref(g_ptr_array_index(patterns, i)), tag};
    g_array_append_val(highlighter->compiled, compiled);
  }
  return TRUE;
}

void tio_highlighter_feed(TioHighlighter *highlighter, const guint8 *data,
                          gsize length) {
  g_return_if_fail(highlighter != NULL);
  guint completed = 0;
  for (gsize index = 0; index < length; ++index) {
    if (tio_text_line_feed(&highlighter->editing, highlighter->line,
                           data[index], TIO_HIGHLIGHT_MAX_LINE_BYTES)) {
      flush_line(highlighter);
      /* Amortize history deletion, even when a caller supplies a huge burst. */
      if (++completed % 64 == 0) trim_scrollback(highlighter);
    }
  }
  render_line(highlighter);
  trim_scrollback(highlighter);
}

gboolean tio_highlighter_cursor(TioHighlighter *highlighter, GtkTextIter *iter) {
  guint bytes = MIN(highlighter->editing.cursor, highlighter->line->len);
  g_autofree char *prefix = g_utf8_make_valid((const char *)highlighter->line->data, bytes);
  gint chars = (gint)g_utf8_strlen(prefix, -1);
  gint offset = gtk_text_buffer_get_char_count(highlighter->buffer) -
                highlighter->partial_chars + chars;
  gtk_text_buffer_get_iter_at_offset(highlighter->buffer, iter, offset);
  return !highlighter->editing.cursor_hidden;
}

static void remove_buffer_tag(GtkTextTag *tag, gpointer data) {
  GtkTextBuffer *buffer = data;
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_remove_tag(buffer, tag, &start, &end);
}

void tio_highlighter_clear(TioHighlighter *highlighter) {
  g_return_if_fail(highlighter != NULL);
  /* Visit each tag once, including search tags, before deleting its text.
     Direct deletion repeatedly rebalances the tagged text tree. Keep the
     table intact so custom rules and search continue to work after clear. */
  gtk_text_tag_table_foreach(gtk_text_buffer_get_tag_table(highlighter->buffer),
                             remove_buffer_tag, highlighter->buffer);
  gtk_text_buffer_set_text(highlighter->buffer, "", 0);
  g_byte_array_set_size(highlighter->line, 0);
  highlighter->editing = (TioTextLine){0};
  highlighter->partial_chars = 0;
  g_clear_pointer(&highlighter->rendered, g_free);
}

void tio_highlighter_free(TioHighlighter *highlighter) {
  if (highlighter == NULL) {
    return;
  }
  for (guint index = 0; index < highlighter->compiled->len; ++index) {
    CompiledRule *rule =
        &g_array_index(highlighter->compiled, CompiledRule, index);
    g_regex_unref(rule->regex);
  }
  g_array_unref(highlighter->compiled);
  g_byte_array_unref(highlighter->line);
  g_object_unref(highlighter->buffer);
  g_free(highlighter->rendered);
  g_free(highlighter);
}
