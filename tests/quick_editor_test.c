/* SPDX-License-Identifier: GPL-3.0-only */
#define main tio_gui_application_main
#include "../src/main.c"
#undef main
#include <sys/socket.h>
#include <unistd.h>

static void check_editor(void)
{
    TioApp app = {0};
    TioTab tab = {0};
    tio_settings_init(&app.settings);
    tio_session_config_init(&tab.config);
    tab.app = &app;
    app.window = gtk_window_new();
    on_customize_quick_buttons(NULL, &tab);
    GListModel *windows = gtk_window_get_toplevels();
    QuickButtonEditor *editor = NULL;
    GtkWindow *dialog = NULL;
    for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        GtkWindow *candidate = g_list_model_get_item(windows, i);
        editor = g_object_get_data(G_OBJECT(candidate), "quick-editor");
        if (editor) { dialog = candidate; break; }
        g_object_unref(candidate);
    }
    g_assert_nonnull(editor);
    gtk_editable_set_text(GTK_EDITABLE(editor->payload_entries[0]), "01 03 00 00 00 0A");
    gtk_drop_down_set_selected(editor->modes[0], 1);
    gtk_drop_down_set_selected(editor->crcs[0], 2);
    g_assert_cmpstr(gtk_label_get_text(editor->previews[0]), ==,
                    "01 03 00 00 00 0A C5 CD (8 bytes)");
    g_assert_true(gtk_widget_get_sensitive(editor->save));
    gtk_editable_set_text(GTK_EDITABLE(editor->payload_entries[0]), "01 0");
    g_assert_false(gtk_widget_get_sensitive(editor->save));
    gtk_editable_set_text(GTK_EDITABLE(editor->payload_entries[0]), "00 14 FF");
    g_assert_true(gtk_widget_get_sensitive(editor->save));
    if (g_getenv("TIO_TEST_SCREENSHOT")) {
        gint64 until = g_get_monotonic_time() + 300000;
        while (g_get_monotonic_time() < until) {
            g_main_context_iteration(NULL, FALSE);
            g_usleep(1000);
        }
        g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(GTK_WIDGET(dialog));
        GtkSnapshot *snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot),
            gtk_widget_get_width(GTK_WIDGET(dialog)), gtk_widget_get_height(GTK_WIDGET(dialog)));
        g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
        GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(dialog));
        g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, NULL);
        g_assert_true(gdk_texture_save_to_png(texture, g_getenv("TIO_TEST_SCREENSHOT")));
    }
    gtk_window_destroy(dialog);
    g_object_unref(dialog);
    gtk_window_destroy(GTK_WINDOW(app.window));
    tio_session_config_clear(&tab.config);
    tio_settings_clear(&app.settings);
}

static void check_send(void)
{
    int pair[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
    TioTab tab = {0};
    tab.child_pid = 1;
    tab.status_label = GTK_LABEL(g_object_ref_sink(gtk_label_new("")));
    TioRawTap *tap = g_new0(TioRawTap, 1);
    tap->reference_count = 1;
    tap->tab = &tab;
    tap->cancellable = g_cancellable_new();
    g_autoptr(GSocket) socket = g_socket_new_from_fd(pair[0], NULL);
    tap->connection = g_socket_connection_factory_create_connection(socket);
    tab.raw = tap;
    g_autoptr(GByteArray) bytes = g_byte_array_new();
    for (guint i = 0; i < 256; ++i) {
        guint8 byte = (guint8)i;
        g_byte_array_append(bytes, &byte, 1);
    }
    g_assert_true(send_payload(&tab, bytes));
    g_assert_false(send_payload(&tab, bytes));
    gint64 until = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (tap->sending && g_get_monotonic_time() < until) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    g_assert_null(tap->sending);
    guint8 received[256];
    g_assert_cmpint(recv(pair[1], received, sizeof received, MSG_DONTWAIT), ==, 256);
    g_assert_cmpmem(received, sizeof received, bytes->data, bytes->len);
    tab.quick_pending = g_byte_array_ref(bytes);
    tab.quick_send_timer = g_timeout_add(60000, quick_send_delayed, &tab);
    raw_tap_stop(&tab);
    g_assert_cmpuint(tab.quick_send_timer, ==, 0);
    g_assert_null(tab.quick_pending);
    g_assert_null(tab.raw);
    g_object_unref(tab.status_label);
    close(pair[1]);
}

static void editor_action(SequenceEditor *editor, const char *action)
{
    GtkWidget *button = g_object_ref_sink(sequence_button(editor, action, action));
    g_signal_emit_by_name(button, "clicked");
    g_object_unref(button);
}

static void check_sequence_editor(void)
{
    TioApp app = {0};
    TioTab tab = {0};
    tio_settings_init(&app.settings);
    tab.app = &app;
    app.window = gtk_window_new();
    on_sequences_clicked(NULL, &tab);
    SequenceEditor *editor = g_object_get_data(G_OBJECT(tab.sequence_window), "sequence-editor");
    g_assert_nonnull(editor);
    gtk_editable_set_text(GTK_EDITABLE(editor->name), "Boot test");
    SequenceRow *row = g_ptr_array_index(editor->rows, 0);
    gtk_editable_set_text(GTK_EDITABLE(row->payload), "AT");
    gtk_drop_down_set_selected(row->ending, 3);
    editor_action(editor, "add");
    row = g_ptr_array_index(editor->rows, 1);
    gtk_editable_set_text(GTK_EDITABLE(row->payload), "00 14 FF");
    gtk_drop_down_set_selected(row->mode, 1);
    editor_action(editor, "save");
    g_assert_cmpuint(app.settings.sequences->len, ==, 1);
    TioSettings restored;
    tio_settings_init(&restored);
    tio_settings_load(&restored);
    g_assert_cmpuint(restored.sequences->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(restored.sequences, 0), ==,
                   g_ptr_array_index(app.settings.sequences, 0));
    tio_settings_clear(&restored);
    editor_action(editor, "new");
    g_assert_cmpuint(editor->rows->len, ==, 1);
    editor_action(editor, "load");
    g_assert_cmpuint(editor->rows->len, ==, 2);
    row = g_ptr_array_index(editor->rows, 1);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(row->payload)), ==, "00 14 FF");
    gtk_editable_set_text(GTK_EDITABLE(row->payload), "0");
    editor_action(editor, "save");
    g_assert_cmpuint(app.settings.sequences->len, ==, 1);
    g_assert_nonnull(strstr(gtk_label_get_text(editor->status), "Step 2"));
    gtk_window_destroy(GTK_WINDOW(tab.sequence_window));
    g_assert_null(tab.sequence_window);
    on_sequences_clicked(NULL, &tab);
    gtk_window_destroy(GTK_WINDOW(tab.sequence_window));
    gtk_window_destroy(GTK_WINDOW(app.window));
    tio_settings_clear(&app.settings);
}

static void check_sequence_real_tio(void)
{
    const char *path = g_getenv("TIO_TEST_SOCKET");
    if (!path) { g_test_skip("Set TIO_TEST_SOCKET using socket_acceptance.py"); return; }
    TioTab tab = {0};
    tab.child_pid = 1;
    tab.status_label = GTK_LABEL(g_object_ref_sink(gtk_label_new("")));
    TioRawTap *tap = g_new0(TioRawTap, 1);
    tap->reference_count = 1;
    tap->tab = &tab;
    tap->cancellable = g_cancellable_new();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(path);
    g_autoptr(GSocketClient) client = g_socket_client_new();
    tap->connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
    g_assert_nonnull(tap->connection);
    tab.raw = tap;
    TioSequence *sequence = tio_sequence_new("Real tio acceptance");
    tio_sequence_add(sequence, "00 14 FF", 1, 0, 0, 100);
    tio_sequence_add(sequence, "01 03 00 00 00 0A", 1, 0, 2, 0);
    tab.sequence_runner = tio_sequence_runner_new(sequence, FALSE, sequence_send, &tab, NULL);
    gint64 until = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (tio_sequence_runner_active(tab.sequence_runner) && g_get_monotonic_time() < until) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    g_assert_false(tio_sequence_runner_active(tab.sequence_runner));
    g_assert_false(tio_sequence_runner_failed(tab.sequence_runner));
    raw_tap_stop(&tab);
    tio_sequence_free(sequence);
    g_object_unref(tab.status_label);
}

static void check_line_controls_real_tio(void)
{
    const char *fd_text = g_getenv("TIO_TEST_CONTROL_FD");
    if (!fd_text) { g_test_skip("Run serial_line_acceptance.py for the isolated tio PTY"); return; }
    TioTab tab = {0};
    tab.child_pid = 1;
    tab.status_label = GTK_LABEL(g_object_ref_sink(gtk_label_new("")));
    tab.terminal = VTE_TERMINAL(vte_terminal_new());
    g_autoptr(VtePty) pty = vte_pty_new_foreign_sync(dup(atoi(fd_text)), NULL, NULL);
    g_assert_nonnull(pty);
    vte_terminal_set_pty(tab.terminal, pty);
    GtkWidget *window = gtk_window_new();
    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(tab.terminal));
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 400);
    gtk_window_present(GTK_WINDOW(window));
    const char *actions[] = {"dtr-high", "dtr-low", "rts-high", "rts-low", "dtr-pulse", "rts-pulse", "break"};
    for (guint i = 0; i < G_N_ELEMENTS(actions); ++i) {
        GtkWidget *button = g_object_ref_sink(gtk_button_new());
        g_object_set_data(G_OBJECT(button), "line-action", (gpointer)actions[i]);
        on_line_control(GTK_BUTTON(button), &tab);
        gint64 until = g_get_monotonic_time() + 6 * G_TIME_SPAN_SECOND;
        while (tab.line_command_timer && g_get_monotonic_time() < until) {
            g_main_context_iteration(NULL, FALSE); g_usleep(1000);
        }
        g_assert_cmpuint(tab.line_command_timer, ==, 0);
        if (i < 4) g_assert_cmpstr(gtk_label_get_text(tab.status_label), ==,
                                  "Line command submitted; inspect tio response");
        until = g_get_monotonic_time() + 200000;
        while (g_get_monotonic_time() < until) {
            g_main_context_iteration(NULL, FALSE); g_usleep(1000);
        }
        g_object_unref(button);
    }
    raw_tap_stop(&tab);
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(tab.status_label);
}

static void check_reconnect_observer(void)
{
    const char *pid_text = g_getenv("TIO_TEST_TIO_PID");
    const char *device = g_getenv("TIO_TEST_TIO_DEVICE");
    if (!pid_text || !device) { g_test_skip("Run reconnect_acceptance.py"); return; }
    TioTab tab = {0};
    tio_session_config_init(&tab.config);
    tab.config.device = g_strdup(device);
    tab.child_pid = atoi(pid_text);
    tab.status_label = GTK_LABEL(g_object_ref_sink(gtk_label_new("")));
    tab.send_entry = GTK_ENTRY(g_object_ref_sink(gtk_entry_new()));
    tab.send_button = GTK_BUTTON(g_object_ref_sink(gtk_button_new()));
    for (guint stage = 0; stage < 3; ++stage) {
        gint64 until = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
        gboolean expected = stage != 1;
        do {
            observe_connection(&tab);
            if (tab.observed_connected == expected) break;
            g_usleep(10000);
        } while (g_get_monotonic_time() < until);
        g_assert_cmpint(tab.observed_connected, ==, expected);
        g_assert_cmpint(gtk_widget_get_sensitive(GTK_WIDGET(tab.send_button)), ==, expected);
        g_print("WATCH_%u\n", stage);
        fflush(stdout);
    }
    g_assert_cmpuint(tab.reconnect_count, ==, 1);
    g_assert_nonnull(tab.disconnect_reason);
    g_free(tab.disconnect_reason);
    g_free(tab.observed_device);
    g_object_unref(tab.status_label);
    g_object_unref(tab.send_entry);
    g_object_unref(tab.send_button);
    tio_session_config_clear(&tab.config);
}

static void check_about(void)
{
    TioApp app = {0};
    app.window = gtk_window_new();
    on_about_clicked(NULL, &app);
    GListModel *windows = gtk_window_get_toplevels();
    GtkWindow *about = NULL;
    GtkTextBuffer *buffer = NULL;
    for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        GtkWindow *candidate = g_list_model_get_item(windows, i);
        buffer = g_object_get_data(G_OBJECT(candidate), "diagnostic-buffer");
        if (buffer) { about = candidate; break; }
        g_object_unref(candidate);
    }
    g_assert_nonnull(about);
    gint64 until = g_get_monotonic_time() + 6 * G_TIME_SPAN_SECOND;
    g_autofree gchar *text = NULL;
    do {
        g_main_context_iteration(NULL, FALSE);
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        g_free(text);
        text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        if (!strstr(text, "Checking")) break;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_assert_nonnull(strstr(text, "tio-gui " TIO_GUI_VERSION));
    g_assert_nonnull(strstr(text, "tio "));
    g_assert_nonnull(strstr(text, "GPL-3.0-only"));
    g_assert_null(strstr(text, g_get_home_dir()));
    gtk_window_destroy(about);
    g_object_unref(about);
    gtk_window_destroy(GTK_WINDOW(app.window));
}

int main(int argc, char **argv)
{
    g_autofree gchar *config_root = g_dir_make_tmp("tio-gui-ui-test-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", config_root, TRUE);
    g_test_init(&argc, &argv, NULL);
    gtk_init();
    g_test_add_func("/quick-editor/preview-validation", check_editor);
    g_test_add_func("/quick-editor/binary-send-and-cancel", check_send);
    g_test_add_func("/sequences/editor-persistence", check_sequence_editor);
    g_test_add_func("/sequences/real-tio", check_sequence_real_tio);
    g_test_add_func("/serial-lines/real-tio", check_line_controls_real_tio);
    g_test_add_func("/connection/real-reconnect", check_reconnect_observer);
    g_test_add_func("/about/version-diagnostics", check_about);
    int result = g_test_run();
    g_autofree gchar *file = g_build_filename(config_root, "tio-gui", "config.ini", NULL);
    g_autofree gchar *directory = g_path_get_dirname(file);
    g_unlink(file); g_rmdir(directory); g_rmdir(config_root);
    return result;
}
