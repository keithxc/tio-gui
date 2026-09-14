/* SPDX-License-Identifier: GPL-3.0-only */
#include "native_terminal.h"
static void output(const char *bytes, gsize length, gpointer data) { g_string_append_len(data, bytes, length); }
static void spin(void) { gint64 end = g_get_monotonic_time()+100000; while(g_get_monotonic_time()<end) { while(g_main_context_iteration(NULL,FALSE)) {} g_usleep(1000); } }
static gchar *contents(TioNativeTerminal *t)
{
    tio_terminal_render(t, TRUE); spin(); GtkTextBuffer *b=gtk_text_view_get_buffer(tio_terminal_view(t)); GtkTextIter a,z;
    gtk_text_buffer_get_bounds(b,&a,&z); return gtk_text_buffer_get_text(b,&a,&z,FALSE);
}
static void feed(TioNativeTerminal *t,const char *text) { tio_terminal_feed(t,(const guint8 *)text,strlen(text)); }
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL); gtk_init();
    GString *sent=g_string_new(NULL); TioNativeTerminal *t=tio_terminal_new(output,sent);
    GtkWindow *window=GTK_WINDOW(gtk_window_new()); gtk_window_set_default_size(window,800,400);
    GtkWidget *scroll=gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),GTK_WIDGET(tio_terminal_view(t)));
    gtk_window_set_child(window,scroll); gtk_window_present(window); spin(); tio_terminal_render(t,TRUE);
    feed(t,"\033[2J\033[Hmain\033[2;4H\033[31mRED\033[0m");
    g_autofree gchar *normal=contents(t); g_assert_true(g_str_has_prefix(normal,"main"));g_assert_nonnull(strstr(normal,"   RED"));
    feed(t,"\033[?1049h\033[2J\033[HALT 中文");
    g_autofree gchar *alternate=contents(t);g_assert_true(g_str_has_prefix(alternate,"ALT 中文"));g_assert_null(strstr(alternate,"main"));
    feed(t,"\033[?1049l");g_autofree gchar *restored=contents(t);g_assert_true(g_str_has_prefix(restored,"main"));g_assert_nonnull(strstr(restored,"RED"));
    g_string_truncate(sent,0);feed(t,"\033[?1h");tio_terminal_key(t,GDK_KEY_Up,0);g_assert_cmpstr(sent->str,==,"\033OA");
    g_string_truncate(sent,0);feed(t,"\033[?2004h");tio_terminal_paste(t,"hello 中文");g_assert_cmpstr(sent->str,==,"\033[200~hello 中文\033[201~");
    feed(t,"\033[2J\033[H");for(guint i=0;i<100;i++)feed(t,"scrollback\r\n");
    g_autofree gchar *history=contents(t);g_assert_nonnull(strstr(history,"scrollback"));
    tio_terminal_clear(t);g_autofree gchar *clear=contents(t);g_assert_null(strstr(clear,"scrollback"));
    gtk_window_destroy(window);tio_terminal_free(t);g_string_free(sent,TRUE);
    g_test_message("Terminal cursor, color, alternate screen, UTF-8, scrollback, application keys and bracketed paste passed");return 0;
}
