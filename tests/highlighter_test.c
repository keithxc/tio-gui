/* SPDX-License-Identifier: GPL-3.0-only */

#include "highlighter.h"
#include "text_line_cases.h"

static void test_progress(void) {
  g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);
  TioHighlighter *h = tio_highlighter_new(buffer);
  for (guint i = 0; i < G_N_ELEMENTS(text_line_cases); ++i) {
    const char *input = text_line_cases[i].input;
    for (guint chunk = 1; chunk <= strlen(input); ++chunk) {
      tio_highlighter_clear(h);
      for (gsize offset = 0; offset < strlen(input); offset += chunk)
        tio_highlighter_feed(h, (const guint8 *)input + offset,
                             MIN(chunk, strlen(input) - offset));
      GtkTextIter start, end;
      gtk_text_buffer_get_bounds(buffer, &start, &end);
      g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
      g_assert_cmpstr(text, ==, text_line_cases[i].expected);
    }
  }
  tio_highlighter_clear(h);
  const char *partial = "10%\r20%";
  tio_highlighter_feed(h, (const guint8 *)partial, strlen(partial));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(text, ==, "20%");
  g_assert_cmpint(gtk_text_buffer_get_line_count(buffer), ==, 1);
  /* Clearing discards both an incomplete CSI and the overwrite cursor. */
  tio_highlighter_feed(h, (const guint8 *)"\r\x1b[", 3);
  tio_highlighter_clear(h);
  tio_highlighter_feed(h, (const guint8 *)"fresh", 5);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *fresh = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(fresh, ==, "fresh");
  /* A saturated line can still be overwritten and erased. */
  tio_highlighter_clear(h);
  g_autofree char *long_line = g_strnfill(20000, 'x');
  tio_highlighter_feed(h, (const guint8 *)long_line, 20000);
  g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 16384);
  const char *replace = "\rOK\x1b[K\n";
  tio_highlighter_feed(h, (const guint8 *)replace, strlen(replace));
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *short_line = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(short_line, ==, "OK\n");
  /* Untrusted cursor parameters remain bounded. */
  tio_highlighter_clear(h);
  const char *huge = "\x1b[999999999999999999999CX\rOK\x1b[K\n";
  tio_highlighter_feed(h, (const guint8 *)huge, strlen(huge));
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *bounded = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(bounded, ==, "OK\n");
  /* Inserting at capacity drops a whole UTF-8 suffix, never half a character. */
  tio_highlighter_clear(h);
  g_autofree gchar *prefix = g_strnfill(16380, 'x');
  tio_highlighter_feed(h, (const guint8 *)prefix, 16380);
  tio_highlighter_feed(h, (const guint8 *)"中", strlen("中"));
  tio_highlighter_feed(h, (const guint8 *)"\r\x1b[2@", 5);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree gchar *inserted = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpuint(strlen(inserted), ==, 16382);
  g_assert_true(g_str_has_prefix(inserted, "  xx"));
  g_assert_null(strstr(inserted, "\xef\xbf\xbd"));
  tio_highlighter_feed(h, (const guint8 *)"\r\x1b[999999999999@", strlen("\r\x1b[999999999999@"));
  g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 16384);
  tio_highlighter_free(h);
}

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

static void test_shell_editing(void) {
  const char *path = g_getenv("TIO_TEST_SHELL_CAPTURE");
  if (!path) { g_test_skip("Run shell_edit_acceptance.py"); return; }
  g_autofree gchar *bytes = NULL;
  gsize length = 0;
  g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
  g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);
  TioHighlighter *h = tio_highlighter_new(buffer);
  /* Fragmenting each byte covers ESC/CSI and UTF-8 across socket reads. */
  for (gsize i = 0; i < length; i++) tio_highlighter_feed(h, (const guint8 *)bytes + i, 1);
  GtkTextIter start, end, cursor;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(g_strchomp(text), ==, g_getenv("TIO_TEST_SHELL_EXPECTED"));
  tio_highlighter_cursor(h, &cursor);
  g_assert_cmpint(gtk_text_iter_get_line_offset(&cursor), ==,
                  (gint)g_ascii_strtoll(g_getenv("TIO_TEST_SHELL_CURSOR"), NULL, 10));
  tio_highlighter_free(h);
}

static void test_flood_clear(void) {
  g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);
  TioHighlighter *h = tio_highlighter_new(buffer);
  const char *line = "[12:34:56.789] INFO temperature=24.5 voltage=3.3 /dev/ttyUSB0 0x1234 ready\n";
  g_autoptr(GString) burst = g_string_new(NULL);
  for (guint i = 0; i < 12000; ++i) g_string_append(burst, line);
  tio_highlighter_feed(h, (const guint8 *)burst->str, burst->len);
  g_assert_cmpint(gtk_text_buffer_get_line_count(buffer), ==, 10000);
  GtkTextTag *search = gtk_text_buffer_create_tag(buffer, "search-match", "background", "yellow", NULL);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_apply_tag(buffer, search, &start, &end);
  g_assert_true(tio_highlighter_rules(h, "[rule:custom]\npattern=custom\ncolor=red\n", NULL));
  gint64 before = g_get_monotonic_time();
  tio_highlighter_clear(h);
  gint64 elapsed = g_get_monotonic_time() - before;
  g_test_message("Clear 10,000 highlighted lines: %.1f ms", elapsed / 1000.0);
  /* Generous regression guard: direct tagged deletion took about ten seconds. */
  g_assert_cmpint(elapsed, <, 2 * G_TIME_SPAN_SECOND);
  g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 0);
  g_assert_true(gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), "search-match") == search);
  const char *fresh = "中 INFO custom 123\n";
  tio_highlighter_feed(h, (const guint8 *)fresh, strlen(fresh));
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(text, ==, fresh);
  gtk_text_buffer_get_iter_at_offset(buffer, &start, 7);
  g_assert_true(gtk_text_iter_has_tag(&start, gtk_text_tag_table_lookup(
      gtk_text_buffer_get_tag_table(buffer), "rule:custom")));
  g_assert_false(gtk_text_iter_has_tag(&start, search));
  tio_highlighter_free(h);
}

static void test_long_history_bound(void) {
  g_autoptr(GtkTextBuffer) buffer = gtk_text_buffer_new(NULL);
  TioHighlighter *h = tio_highlighter_new(buffer);
  g_autofree char *line = g_strnfill(16384, 'x');
  for (guint i = 0; i < 140; ++i) {
    tio_highlighter_feed(h, (const guint8 *)line, 16384);
    tio_highlighter_feed(h, (const guint8 *)"\n", 1);
  }
  g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), <=, 2 * 1024 * 1024);
  g_assert_cmpint(gtk_text_buffer_get_line_count(buffer), >, 100);
  const char *tail = "中 INFO 123 456";
  tio_highlighter_feed(h, (const guint8 *)tail, strlen(tail));
  GtkTextIter cursor;
  g_assert_true(tio_highlighter_cursor(h, &cursor));
  g_assert_cmpint(gtk_text_iter_get_line_offset(&cursor), ==, 14);
  gtk_text_iter_backward_chars(&cursor, 3);
  g_assert_true(gtk_text_iter_has_tag(&cursor, gtk_text_tag_table_lookup(
      gtk_text_buffer_get_tag_table(buffer), "number")));
  tio_highlighter_free(h);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/highlighter/flood-clear", test_flood_clear);
  g_test_add_func("/highlighter/long-history-bound", test_long_history_bound);
  g_test_add_func("/highlighter/serial-log-rules", test_serial_log_rules);
  g_test_add_func("/highlighter/custom-atomic-and-bounded", test_custom_rules);
  g_test_add_func("/highlighter/incremental-tail", test_incremental_tail);
  g_test_add_func("/highlighter/progress-line-editing", test_progress);
  g_test_add_func("/highlighter/real-shell-editing", test_shell_editing);
  return g_test_run();
}
