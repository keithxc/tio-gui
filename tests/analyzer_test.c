/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/analyzer.c"
#include <glib/gstdio.h>
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
    g_autofree gchar *preset = rules_export(view);
    gtk_editable_set_text(GTK_EDITABLE(view->fields_entry), "wrong");
    g_assert_true(rules_import(view, preset, strlen(preset), NULL));
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(view->fields_entry)), ==, "temp,voltage");
    g_assert_false(rules_import(view, "[parser]\nversion=99\n", 20, NULL));
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(view->filter_entry)), ==, "ERROR");
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
static void playback(void)
{
    g_autofree gchar *directory = g_dir_make_tmp("tio-playback-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(directory, "test.tiocap", NULL);
    TioCapture *capture = tio_capture_new(path, 4096, 0, 2, 8192, "115200", NULL);
    const char *line = "INFO temp=42\n";
    g_assert_true(tio_capture_record(capture, TIO_CAPTURE_RX, (const guint8 *)line, strlen(line), 1000000));
    const guint8 bytes[] = {0, 0x14, 0xff};
    g_assert_true(tio_capture_record(capture, TIO_CAPTURE_TX, bytes, sizeof bytes, 1100000));
    tio_capture_stop(capture);
    while (!tio_capture_finished(capture)) g_main_context_iteration(NULL, TRUE);
    tio_capture_unref(capture);
    GtkWidget *parent = gtk_window_new();
    TioLogModel *model = tio_log_model_new();
    GtkWidget *window = tio_analyzer_new(GTK_WINDOW(parent), model);
    Analyzer *view = g_object_get_data(G_OBJECT(window), "analyzer");
    ReplayLoad *load = g_new0(ReplayLoad, 1);
    load->path = g_strdup(path); load->view = view;
    g_weak_ref_init(&load->window, window);
    GTask *task = g_task_new(NULL, NULL, replay_loaded, NULL);
    g_task_set_task_data(task, load, replay_load_free);
    g_task_run_in_thread(task, replay_load_worker); g_object_unref(task);
    gint64 until = g_get_monotonic_time() + 3000000;
    while (!view->replay && g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    g_assert_nonnull(view->replay);
    g_assert_cmpuint(tio_log_model_entries(model)->length, ==, 0);
    gtk_spin_button_set_value(view->replay_speed, 10);
    gtk_check_button_set_active(view->replay_pause, FALSE);
    while (!tio_replay_finished(view->replay) && g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    g_assert_true(tio_replay_finished(view->replay));
    g_assert_cmpuint(tio_log_model_entries(model)->length, ==, 2);
    TioLogEntry *last = g_queue_peek_tail((GQueue *)tio_log_model_entries(model));
    g_assert_nonnull(strstr(last->text, "TX 00 14 FF"));
    gtk_window_destroy(GTK_WINDOW(window)); gtk_window_destroy(GTK_WINDOW(parent));
    tio_log_model_free(model);
    g_remove(path); g_rmdir(directory);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); gtk_init();
    g_test_add_func("/analyzer/filter-fields-plot-pause", exercise);
    g_test_add_func("/analyzer/async-offline-playback", playback);
    return g_test_run();
}
