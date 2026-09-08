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

static gboolean check_cli_window(gpointer data)
{
    GtkApplication *application = data;
    GtkWidget *window = g_object_get_data(G_OBJECT(application), "tio-gui-window");
    if (!window) return G_SOURCE_CONTINUE;
    TioApp *app = g_object_get_data(G_OBJECT(window), "tio-gui");
    g_assert_cmpstr(selected_string(app->active->device_dropdown), ==, "/dev/tio-cli-test");
    g_assert_cmpstr(selected_baud(app->active), ==, "250000");
    g_assert_cmpint(app->active->child_pid, <=, 0);
    gtk_window_close(GTK_WINDOW(window));
    return G_SOURCE_REMOVE;
}

static void check_cli(void)
{
    g_autoptr(GtkApplication) application = gtk_application_new("io.github.keithxc.tio_gui.cli.tests",
        G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_COMMAND_LINE);
    const GOptionEntry options[] = {
        {"device", 'd', 0, G_OPTION_ARG_STRING, NULL, "Device", "DEVICE"},
        {"baud", 'b', 0, G_OPTION_ARG_STRING, NULL, "Baud", "BAUD"},
        {"no-connect", 0, 0, G_OPTION_ARG_NONE, NULL, "No connect", NULL},
        {NULL}
    };
    g_application_add_main_option_entries(G_APPLICATION(application), options);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(application, "command-line", G_CALLBACK(on_command_line), NULL);
    char *args[] = {"test", "--device", "/dev/tio-cli-test", "--baud", "250000", "--no-connect", NULL};
    g_timeout_add(20, check_cli_window, application);
    g_assert_cmpint(g_application_run(G_APPLICATION(application), 6, args), ==, 0);
}

static void check_session_tools(void)
{
    g_autoptr(GtkApplication) application = gtk_application_new("io.github.keithxc.tio_gui.tools.tests", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, NULL));
    activate(application, NULL);
    GtkWidget *window = g_object_get_data(G_OBJECT(application), "tio-gui-window");
    TioApp *app = g_object_get_data(G_OBJECT(window), "tio-gui");
    TioTab *first = app->active, *second = tio_app_add_tab(app);
    g_autoptr(TabRequest) request = tab_request_new(second, "snapshot");
    g_assert_true(tab_request_resolve(request, GTK_WINDOW(window)) == second);
    g_free(first->config.device); first->config.device = g_strdup("/dev/tio-missing-test");
    apply_session_config(first, &first->config);
    refresh_devices(first);
    g_assert_cmpstr(selected_string(first->device_dropdown), ==, "/dev/tio-missing-test");
    TioSessionConfig profile; tio_session_config_init(&profile);
    g_free(profile.quick_payloads[0]); profile.quick_payloads[0] = g_strdup("00 FF");
    profile.quick_modes[0] = 1;
    apply_session_config(first, &profile);
    g_assert_cmpstr(first->config.quick_payloads[0], ==, "00 FF");
    tio_session_config_clear(&profile);
    g_free(first->config.tab_name); first->config.tab_name = g_strdup("Board A");
    update_tab_label(first);
    g_assert_cmpstr(gtk_label_get_text(first->tab_label), ==, "Board A");
    tio_settings_clear_tabs(&app->settings);
    tio_settings_add_tab(&app->settings, &first->config);
    g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "tabs.ini", NULL);
    g_assert_true(tio_settings_save_to_file(&app->settings, path, NULL));
    TioSettings restored; tio_settings_init(&restored);
    g_assert_true(tio_settings_load_from_file(&restored, path, NULL));
    g_assert_cmpstr(((TioSessionConfig *)g_ptr_array_index(restored.tab_configs, 0))->tab_name, ==, "Board A");
    tio_settings_clear(&restored); g_unlink(path);
    g_action_group_activate_action(G_ACTION_GROUP(window), "select-tab", g_variant_new_int32(0));
    g_assert_true(app->active == first);
    tio_log_model_feed(first->log_model, (const guint8 *)"board-a=1\n", 10, 1000000);
    tio_log_model_feed(second->log_model, (const guint8 *)"board-b=2\n", 10, 2000000);
    on_compare_sessions(NULL, app);
    GListModel *windows = gtk_window_get_toplevels();
    GtkWindow *comparison = NULL;
    SessionCompare *compare = NULL;
    for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        GtkWindow *candidate = g_list_model_get_item(windows, i);
        compare = g_object_get_data(G_OBJECT(candidate), "session-compare");
        if (compare) { comparison = candidate; break; }
        g_object_unref(candidate);
    }
    g_assert_nonnull(compare);
    gtk_notebook_reorder_child(app->notebook, second->content, 0);
    compare_refresh(compare);
    GtkTextIter begin, end;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(compare->views[0]);
    gtk_text_buffer_get_bounds(buffer, &begin, &end);
    g_autofree gchar *text = gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
    g_assert_nonnull(strstr(text, "board-a=1"));
    tio_app_finish_close_tab(app, second);
    g_assert_null(tab_request_resolve(request, GTK_WINDOW(window)));
    compare_refresh(compare);
    buffer = gtk_text_view_get_buffer(compare->views[1]);
    gtk_text_buffer_get_bounds(buffer, &begin, &end);
    g_free(text); text = gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
    g_assert_nonnull(strstr(text, "Session closed"));
    gtk_window_destroy(comparison); g_object_unref(comparison);
    gtk_window_close(GTK_WINDOW(window));
    g_assert_cmpint(compare_versions("1", "1.0.1"), <, 0);
    g_assert_cmpint(compare_versions("v0.10.0", "0.9.9"), >, 0);
    g_assert_cmpint(compare_versions("0.3", "0.3.0"), ==, 0);
}

static void check_application_lifecycle(void)
{
    g_autoptr(GtkApplication) application = gtk_application_new("io.github.keithxc.tio_gui.tests", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, NULL));
    activate(application, NULL);
    GtkWidget *window = g_object_get_data(G_OBJECT(application), "tio-gui-window");
    g_assert_nonnull(window);
    g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);
    TioApp *app = g_object_get_data(G_OBJECT(window), "tio-gui");
    TioTab *first = app->active;
    g_assert_nonnull(first->capture_button);
    TioTab *second = tio_app_add_tab(app);
    gtk_notebook_reorder_child(app->notebook, second->content, 0);
    g_assert_true(g_ptr_array_index(app->tabs, 0) == second);
    gtk_notebook_set_current_page(app->notebook, 1);
    g_assert_true(app->active == first);
    tio_app_finish_close_tab(app, second);
    gtk_check_button_set_active(app->show_all_ttys_check, TRUE);
    gtk_check_button_set_active(app->show_all_ttys_check, FALSE);
    g_autofree gchar *path = g_build_filename(g_get_user_config_dir(), "close-test.tiocap", NULL);
    first->capture = tio_capture_new(path, 4096, 0, 2, 8192, "close test", NULL);
    g_assert_nonnull(first->capture);
    TioCapture *capture = tio_capture_ref(first->capture);
    const guint8 bytes[] = {0, 0x14, 0xff};
    g_assert_true(tio_capture_record(capture, TIO_CAPTURE_RX, bytes, sizeof bytes, g_get_real_time()));
    g_assert_true(app_has_live_sessions(app));
    app->close_confirmed = TRUE;
    gtk_window_close(GTK_WINDOW(window));
    gint64 until = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (window && g_get_monotonic_time() < until) {
        g_main_context_iteration(NULL, FALSE); g_usleep(1000);
    }
    g_assert_null(window);
    g_assert_true(tio_capture_finished(capture));
    g_assert_null(tio_capture_error(capture));
    tio_capture_unref(capture);
    g_autofree gchar *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "ABT/"));
    g_unlink(path);
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
    g_test_add_func("/application/command-line", check_cli);
    g_test_add_func("/application/session-tools", check_session_tools);
    g_test_add_func("/application/reorder-close-drain", check_application_lifecycle);
    int result = g_test_run();
    g_autofree gchar *file = g_build_filename(config_root, "tio-gui", "config.ini", NULL);
    g_autofree gchar *directory = g_path_get_dirname(file);
    g_unlink(file); g_rmdir(directory); g_rmdir(config_root);
    return result;
}
