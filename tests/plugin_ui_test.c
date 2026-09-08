/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/plugin_ui.c"
static void exercise(void)
{
    GtkWidget *parent = gtk_window_new();
    GtkWidget *window = tio_plugin_window(GTK_WINDOW(parent), "temp=42");
    Editor *editor = g_object_get_data(G_OBJECT(window), "plugin-editor");
    run(NULL, editor);
    gint64 deadline = g_get_monotonic_time() + 4000000;
    while (editor->cancel && g_get_monotonic_time() < deadline) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    g_assert_null(editor->cancel);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(editor->output);
    GtkTextIter begin, end; gtk_text_buffer_get_bounds(buffer, &begin, &end);
    g_autofree gchar *text = gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
    g_assert_nonnull(strstr(text, "temp=42"));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(editor->source), "function transform(){while(true){}}", -1);
    run(NULL, editor);
    gtk_window_destroy(GTK_WINDOW(window));
    deadline = g_get_monotonic_time() + 2500000;
    while (g_get_monotonic_time() < deadline) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    gtk_window_destroy(GTK_WINDOW(parent));
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); gtk_init();
    g_test_add_func("/plugin-ui/result-close-cancel", exercise); return g_test_run();
}
