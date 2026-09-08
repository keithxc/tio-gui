/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/analyzer.c"
static void exercise(void)
{
    TioLogModel *model = tio_log_model_new();
    const char *logs = "INFO temp=20 voltage=3.3\nERROR temp=25 voltage=3.2\n{\"temp\":22,\"voltage\":3.1,\"nested\":{\"name\":\"board\"}}\n";
    tio_log_model_feed(model, (const guint8 *)logs, strlen(logs), 1000000);
    GtkWidget *parent = gtk_window_new();
    GtkWidget *window = tio_analyzer_new(GTK_WINDOW(parent), model);
    Analyzer *view = g_object_get_data(G_OBJECT(window), "analyzer");
    gtk_editable_set_text(GTK_EDITABLE(view->fields_entry), "temp,voltage");
    apply_filter(NULL, view);
    tick(view);
    g_assert_cmpuint(view->ids->len, ==, 3);
    g_assert_cmpuint(view->points[0]->len, ==, 3);
    g_assert_cmpuint(view->points[1]->len, ==, 3);
    gtk_check_button_set_active(view->follow, FALSE);
    tio_log_model_feed(model, (const guint8 *)"DEBUG temp=23\n", 14, 2000000);
    tick(view);
    g_assert_cmpuint(view->ids->len, ==, 3);
    gtk_check_button_set_active(view->follow, TRUE);
    tick(view);
    g_assert_cmpuint(view->ids->len, ==, 4);
    gtk_editable_set_text(GTK_EDITABLE(view->filter_entry), "ERROR");
    apply_filter(NULL, view); tick(view);
    g_assert_cmpuint(view->ids->len, ==, 1);
    g_assert_cmpuint(view->points[0]->len, ==, 1);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->list);
    GtkTextIter start; gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_place_cursor(buffer, &start);
    select_entry(NULL, 1, 0, 0, view);
    g_assert_nonnull(gtk_widget_get_first_child(GTK_WIDGET(view->details)));
    gtk_editable_set_text(GTK_EDITABLE(view->extract_entry), "temp=(?<temp>[0-9]+)");
    apply_filter(NULL, view); tick(view);
    g_assert_cmpuint(view->points[0]->len, ==, 1);
    g_assert_cmpfloat(g_array_index(view->points[0], Point, 0).y, ==, 25);
    gtk_editable_set_text(GTK_EDITABLE(view->filter_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(view->extract_entry), "");
    apply_filter(NULL, view); tick(view);
    if (g_getenv("TIO_TEST_SCREENSHOT")) {
        gint64 until = g_get_monotonic_time() + 300000;
        while (g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
        g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(window);
        GtkSnapshot *snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(window), gtk_widget_get_height(window));
        g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
        g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node, NULL);
        g_assert_true(gdk_texture_save_to_png(texture, g_getenv("TIO_TEST_SCREENSHOT")));
    }
    gtk_window_destroy(GTK_WINDOW(window)); gtk_window_destroy(GTK_WINDOW(parent));
    tio_log_model_free(model);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); gtk_init();
    g_test_add_func("/analyzer/filter-fields-plot-pause", exercise);
    return g_test_run();
}
