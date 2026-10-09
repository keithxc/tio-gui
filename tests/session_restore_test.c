/* SPDX-License-Identifier: GPL-3.0-only */
#define main tio_gui_application_main
#include "../src/main.c"
#undef main
#include <glib/gstdio.h>

static void drain_events(void)
{
    gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
}

static TioApp *open_workspace(GtkApplication **current)
{
    static guint launch = 0;
    g_clear_object(current);
    g_autofree gchar *id = g_strdup_printf("io.github.keithxc.tio_gui.SessionRestoreTest.Run%u", ++launch);
    *current = gtk_application_new(id, G_APPLICATION_NON_UNIQUE);
    GtkApplication *application = *current;
    g_autoptr(GError) error = NULL;
    gboolean registered = g_application_register(G_APPLICATION(application), NULL, &error);
    g_assert_no_error(error);
    g_assert_true(registered);
    activate(application, NULL);
    drain_events();
    GtkWidget *window = g_object_get_data(G_OBJECT(application), "tio-gui-window");
    g_assert_nonnull(window);
    return g_object_get_data(G_OBJECT(window), "tio-gui");
}

static void close_workspace(GtkApplication *application, TioApp *app)
{
    /* Exercise the real close-request handler, including configuration capture. */
    gtk_window_close(GTK_WINDOW(app->window));
    drain_events();
    g_assert_null(g_object_get_data(G_OBJECT(application), "tio-gui-window"));
}

static void configure_tab(TioTab *tab, const char *name, const char *device,
                          const char *baud, const char *payload)
{
    g_free(tab->config.tab_name); tab->config.tab_name = g_strdup(name);
    g_free(tab->config.device); tab->config.device = g_strdup(device);
    g_free(tab->config.baud); tab->config.baud = g_strdup(baud);
    g_free(tab->config.quick_payloads[0]); tab->config.quick_payloads[0] = g_strdup(payload);
    tab->config.reconnect = TRUE;
    apply_session_config(tab, &tab->config);
    update_tab_label(tab);
}

static void assert_tab(TioApp *app, guint index, const char *name,
                      const char *device, const char *baud, const char *payload)
{
    TioTab *tab = g_ptr_array_index(app->tabs, index);
    g_assert_cmpstr(tab->config.tab_name, ==, name);
    g_assert_cmpstr(selected_string(tab->device_dropdown), ==, device);
    g_assert_cmpstr(selected_baud(tab), ==, baud);
    g_assert_cmpstr(tab->config.quick_payloads[0], ==, payload);
    g_assert_true(gtk_check_button_get_active(tab->reconnect_check));
    /* Restoring a tab must not spawn tio, connect, or start recording. */
    g_assert_cmpint(tab->child_pid, <=, 0);
    g_assert_false(tab->spawn_pending);
    g_assert_null(tab->raw);
    g_assert_null(tab->log_path);
    if (g_str_equal(name, "Board A")) {
        g_assert_true(gtk_check_button_get_active(tab->log_check));
        g_assert_false(gtk_check_button_get_active(tab->timestamp_check));
    }
}

int main(void)
{
    g_autofree gchar *directory = g_dir_make_tmp("tio-session-restore-XXXXXX", NULL);
    g_assert_nonnull(directory);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    g_setenv("TIO_GUI_LANGUAGE", "en", TRUE);
    g_autofree gchar *config_directory = g_build_filename(directory, "tio-gui", NULL);
    g_autofree gchar *config_path = g_build_filename(config_directory, "config.ini", NULL);
    g_autofree gchar *config_backup = g_strconcat(config_path, ".bk", NULL);
    g_autofree gchar *config_lock = g_strconcat(config_path, ".lock", NULL);
    if (!gtk_init_check()) {
        g_rmdir(directory);
        g_print("Session restore requires a display; use Xvfb.\n");
        return 77;
    }
    g_autoptr(GtkApplication) application = NULL;

    TioApp *app = open_workspace(&application);
    g_assert_cmpuint(app->tabs->len, ==, 1);
    configure_tab(g_ptr_array_index(app->tabs, 0), "Board A", "/dev/tio-test-a", "9600", "first");
    TioTab *first = g_ptr_array_index(app->tabs, 0);
    gtk_check_button_set_active(first->log_check, TRUE);
    gtk_check_button_set_active(first->timestamp_check, FALSE);
    configure_tab(tio_app_add_tab(app), "Board B", "/dev/tio-test-b", "250000", "second");
    TioTab *last = tio_app_add_tab(app);
    configure_tab(last, "Board C", "/dev/tio-test-c", "115200", "third");
    gtk_notebook_reorder_child(app->notebook, last->content, 0);

    /* A settings failure must be detected before close tears down any tab or
       live session. Use a directory at the lock path to force a real open
       failure without replacing either settings snapshot. */
    g_assert_cmpint(g_mkdir_with_parents(config_directory, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(config_lock, 0700), ==, 0);
    GtkWidget *original_window = app->window;
    gtk_window_close(GTK_WINDOW(original_window));
    drain_events();
    g_assert_true(g_object_get_data(G_OBJECT(application), "tio-gui-window") == original_window);
    g_assert_cmpuint(app->tabs->len, ==, 3);
    for (guint index = 0; index < app->tabs->len; ++index) {
        TioTab *tab = g_ptr_array_index(app->tabs, index);
        g_assert_false(tab->close_requested);
        g_assert_cmpint(tab->child_pid, <=, 0);
    }
    g_assert_cmpint(g_rmdir(config_lock), ==, 0);
    close_workspace(application, app);

    app = open_workspace(&application);
    g_assert_cmpuint(app->tabs->len, ==, 3);
    assert_tab(app, 0, "Board C", "/dev/tio-test-c", "115200", "third");
    assert_tab(app, 1, "Board A", "/dev/tio-test-a", "9600", "first");
    assert_tab(app, 2, "Board B", "/dev/tio-test-b", "250000", "second");

    /* Explicitly closing a tab removes only that tab from the next workspace. */
    tio_app_close_tab(app, g_ptr_array_index(app->tabs, 1));
    close_workspace(application, app);
    app = open_workspace(&application);
    g_assert_cmpuint(app->tabs->len, ==, 2);
    assert_tab(app, 0, "Board C", "/dev/tio-test-c", "115200", "third");
    assert_tab(app, 1, "Board B", "/dev/tio-test-b", "250000", "second");

    /* The opt-out controls startup, not whether the last workspace is saved. */
    gtk_check_button_set_active(app->restore_tabs_check, FALSE);
    close_workspace(application, app);
    TioSettings saved;
    tio_settings_init(&saved); tio_settings_load(&saved);
    g_assert_false(saved.restore_tabs);
    g_assert_cmpuint(saved.tab_configs->len, ==, 2);
    tio_settings_clear(&saved);
    app = open_workspace(&application);
    g_assert_cmpuint(app->tabs->len, ==, 1);
    g_assert_false(gtk_check_button_get_active(app->restore_tabs_check));
    /* Closing the last tab must persist a just-changed startup preference. */
    gtk_check_button_set_active(app->restore_tabs_check, TRUE);
    tio_app_close_tab(app, g_ptr_array_index(app->tabs, 0));
    drain_events();
    g_assert_null(g_object_get_data(G_OBJECT(application), "tio-gui-window"));
    tio_settings_init(&saved); tio_settings_load(&saved);
    g_assert_true(saved.restore_tabs);
    g_assert_cmpuint(saved.tab_configs->len, ==, 0);
    tio_settings_clear(&saved);

    g_unlink(config_path); g_unlink(config_backup); g_unlink(config_lock);
    g_rmdir(config_directory); g_rmdir(directory);
    g_print("Workspace save failure preserves live tabs; restart restores order and per-tab settings without connecting.\n");
    return 0;
}
