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

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/highlighter/serial-log-rules", test_serial_log_rules);
  return g_test_run();
}
