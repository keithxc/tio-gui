/* SPDX-License-Identifier: GPL-3.0-only */

#include "highlighter.h"

static void assert_tag_at(GtkTextBuffer *buffer, const char *text,
                          const char *needle, const char *tag_name) {
  const char *match = g_strstr_len(text, -1, needle);
  g_assert_nonnull(match);
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_offset(buffer, &iter, (gint)(match - text));
  GtkTextTag *tag = gtk_text_tag_table_lookup(
      gtk_text_buffer_get_tag_table(buffer), tag_name);
  g_assert_nonnull(tag);
  g_assert_true(gtk_text_iter_has_tag(&iter, tag));
}

static void test_serial_log_rules(void) {
  GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
  TioHighlighter *highlighter = tio_highlighter_new(buffer);
  const guint8 first[] = "\x1b[31m[2026-09-08 11:07:45.206820][QHLib][327:904]";
  const guint8 second[] =
      "[RemoteControl.cpp:164] ERROR addr=0x29300 timeout=400 ms\x1b[0m\r\n";
  tio_highlighter_feed(highlighter, first, sizeof(first) - 1);
  g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), >, 0);
  tio_highlighter_feed(highlighter, second, sizeof(second) - 1);

  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(
      text, ==,
      "[2026-09-08 11:07:45.206820][QHLib][327:904]"
      "[RemoteControl.cpp:164] ERROR addr=0x29300 timeout=400 ms\n");
  assert_tag_at(buffer, text, "2026-09-08", "datetime");
  assert_tag_at(buffer, text, "QHLib", "bracketed");
  assert_tag_at(buffer, text, "RemoteControl.cpp:164", "source-location");
  assert_tag_at(buffer, text, "ERROR", "error");
  assert_tag_at(buffer, text, "addr", "key");
  assert_tag_at(buffer, text, "0x29300", "hex");
  assert_tag_at(buffer, text, "400 ms", "duration");

  tio_highlighter_free(highlighter);
  g_object_unref(buffer);
}

static void test_custom_rules(void)
{
  g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);
  TioHighlighter *highlighter = tio_highlighter_new(buffer);
  const char *ini = "[rule:board]\npattern=WATCHDOG\ncolor=#ff6699\nbold=true\n";
  g_assert_true(tio_highlighter_rules(highlighter, ini, NULL));
  g_assert_false(tio_highlighter_rules(highlighter, "[rule:bad]\npattern=(\ncolor=red\n", NULL));
  const char *line = "WATCHDOG reboot\n";
  tio_highlighter_feed(highlighter, (const guint8 *)line, strlen(line));
  assert_tag_at(buffer, line, "WATCHDOG", "rule:board");
  g_assert_true(tio_highlighter_rules(highlighter, "[rule:expensive]\npattern=(a+)+$\ncolor=red\n", NULL));
  g_autofree gchar *long_line = g_strnfill(16000, 'a');
  long_line[15998] = '!'; long_line[15999] = '\n';
  gint64 before = g_get_monotonic_time();
  tio_highlighter_feed(highlighter, (const guint8 *)long_line, 16000);
  g_assert_cmpint(g_get_monotonic_time() - before, <, 1000000);
  g_assert_true(tio_highlighter_rules(highlighter, "", NULL));
  g_assert_null(gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "rule:expensive"));
  tio_highlighter_free(highlighter);
}

static void deleted(GtkTextBuffer *buffer, GtkTextIter *start,
                    GtkTextIter *end, gpointer data) {
  (void)buffer;
  *(guint *)data += gtk_text_iter_get_offset(end) - gtk_text_iter_get_offset(start);
}

static void test_incremental_tail(void) {
  g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);
  TioHighlighter *h = tio_highlighter_new(buffer);
  guint removed = 0;
  g_signal_connect(buffer, "delete-range", G_CALLBACK(deleted), &removed);
  const char *text = "ERROR extended prompt ready\r\n";
  for (gsize i = 0; i < strlen(text); ++i)
    tio_highlighter_feed(h, (const guint8 *)text + i, 1);
  g_assert_cmpuint(removed, ==, 0);
  tio_highlighter_feed(h, (const guint8 *)"ERROR", 5);
  tio_highlighter_feed(h, (const guint8 *)"LESS", 4);
  GtkTextIter start, end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  start = end;
  gtk_text_iter_backward_chars(&start, 9);
  GtkTextTag *tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "error");
  g_assert_false(gtk_text_iter_has_tag(&start, tag));
  /* Split multibyte input replaces only the temporary invalid suffix. */
  tio_highlighter_feed(h, (const guint8 *)"\xe4", 1);
  tio_highlighter_feed(h, (const guint8 *)"\xb8\xad\n", 3);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *actual = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(actual, ==, "ERROR extended prompt ready\nERRORLESS中\n");
  g_assert_cmpuint(removed, ==, 1);
  tio_highlighter_clear(h);
  tio_highlighter_feed(h, (const guint8 *)"new", 3);
  g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 3);
  tio_highlighter_free(h);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/highlighter/serial-log-rules", test_serial_log_rules);
  g_test_add_func("/highlighter/custom-atomic-and-bounded", test_custom_rules);
  g_test_add_func("/highlighter/incremental-tail", test_incremental_tail);
  return g_test_run();
}
