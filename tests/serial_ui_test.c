/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#define main serial_application_main
#include "../src/serial_main.c"
#undef main
#include <fcntl.h>
#include <unistd.h>
static void spin(guint milliseconds)
{
    gint64 end = g_get_monotonic_time() + milliseconds * 1000;
    while (g_get_monotonic_time() < end) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); gtk_init();
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    g_assert_cmpint(master, >=, 0); g_assert_cmpint(grantpt(master), ==, 0); g_assert_cmpint(unlockpt(master), ==, 0);
    g_autofree gchar *name = g_strdup(ptsname(master));
    g_autofree gchar *directory = g_dir_make_tmp("tio-ui-XXXXXX", NULL);
    App app = {0}; app.tabs = g_ptr_array_new(); tio_settings_init(&app.settings);
    app.settings_path = g_build_filename(directory, "serial.ini", NULL);
    app.application = gtk_application_new("io.github.keithxc.tio_gui.serial.test", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app.application), NULL, NULL));
    activate(app.application, &app); Tab *tab = g_ptr_array_index(app.tabs, 0);
    text(tab->device, name); gtk_check_button_set_active(tab->reconnect, FALSE);
    g_autofree gchar *log = g_build_filename(directory, "bytes.log", NULL);
    text(tab->log_path, log); gtk_check_button_set_active(tab->logging, TRUE);
    g_signal_emit_by_name(tab->connect, "clicked");
    for (guint i = 0; i < 100 && !tab->connected; i++) spin(10);
    g_assert_true(tab->connected);
    const guint8 incoming[] = {'O', 'K', '\r', '\n', 0, 0x11, 0x13, 0x14, 0x80, 0xff};
    g_assert_cmpint(write(master, incoming, sizeof incoming), ==, sizeof incoming);
    for (guint i = 0; i < 100 && tab->rx < sizeof incoming; i++) spin(10);
    g_assert_cmpuint(tab->rx, ==, sizeof incoming);
    gtk_check_button_set_active(tab->hex_tx, TRUE); gtk_drop_down_set_selected(tab->ending, 3);
    text(tab->send, "00 11 13 14 FF"); g_signal_emit_by_name(tab->send, "activate");
    for (guint i = 0; i < 100 && tab->tx < 7; i++) spin(10);
    guint8 output[32]; ssize_t length = read(master, output, sizeof output);
    const guint8 expected[] = {0, 0x11, 0x13, 0x14, 0xff, '\r', '\n'};
    g_assert_cmpmem(output, length, expected, sizeof expected); g_assert_cmpuint(tab->tx, ==, 7);
    /* Unhandled printable keys and IM commits must reach the serial peer. */
    g_assert_true(key_pressed(NULL, GDK_KEY_a, 0, 0, tab));
    g_assert_true(key_pressed(NULL, GDK_KEY_C, 0, GDK_SHIFT_MASK, tab));
    g_assert_true(key_pressed(NULL, GDK_KEY_c, 0, GDK_CONTROL_MASK, tab));
    committed(NULL, "中", tab);
    const guint8 typed[] = {'a', 'C', 3, 0xe4, 0xb8, 0xad};
    for (guint i = 0; i < 100 && tab->tx < 7 + sizeof typed; i++) spin(10);
    length = read(master, output, sizeof output);
    g_assert_cmpmem(output, length, typed, sizeof typed);
    /* Logging is independent of presentation and drains on explicit disconnect. */
    gtk_check_button_set_active(tab->hex_rx, TRUE);
    g_assert_cmpint(write(master, "tail", 4), ==, 4); spin(60);
    g_signal_emit_by_name(tab->connect, "clicked"); g_assert_null(tab->serial);
    g_autofree gchar *logged = NULL; gsize log_length;
    g_assert_true(g_file_get_contents(log, &logged, &log_length, NULL));
    g_assert_cmpuint(log_length, ==, sizeof incoming + 4);
    g_assert_cmpmem(logged, sizeof incoming, incoming, sizeof incoming);
    g_assert_cmpmem(logged + sizeof incoming, 4, "tail", 4);
    text(tab->profile, "test"); profile_save(NULL, tab);
    g_assert_nonnull(tio_settings_find_profile(&app.settings, "test"));
    profile_load(NULL, tab); g_assert_cmpuint(app.tabs->len, ==, 2);
    Tab *second = g_ptr_array_index(app.tabs, 1); g_assert_cmpstr(entry(second->device), ==, name);
    g_assert_null(second->serial); remove_tab(second);
    /* Failed opens must unlock controls without an extra Disconnect click. */
    text(tab->device, "/nonexistent-tio-ui-port");
    g_signal_emit_by_name(tab->connect, "clicked");
    for (guint i = 0; i < 100 && tab->serial; i++) spin(10);
    g_assert_null(tab->serial); g_assert_false(tab->connected);
    g_assert_true(gtk_widget_get_sensitive(tab->controls));
    g_assert_cmpstr(gtk_button_get_label(tab->connect), ==, "Connect");
    g_assert_nonnull(strstr(gtk_label_get_text(tab->status), "/nonexistent-tio-ui-port"));
    GtkTextBuffer *search_buffer = gtk_text_view_get_buffer(tab->view);
    gtk_text_buffer_set_text(search_buffer, "中文 ERROR\nerror\nERROR\n", -1);
    gtk_editable_set_text(GTK_EDITABLE(tab->search), "error"); spin(200);
    g_assert_cmpstr(gtk_label_get_text(tab->search_feedback), ==, "1 / 3");
    search_next(NULL, tab);
    g_assert_cmpstr(gtk_label_get_text(tab->search_feedback), ==, "2 / 3");
    search_key(NULL, GDK_KEY_Return, 0, GDK_SHIFT_MASK, tab);
    g_assert_cmpstr(gtk_label_get_text(tab->search_feedback), ==, "1 / 3");
    gtk_check_button_set_active(tab->search_case, TRUE);
    g_assert_cmpstr(gtk_label_get_text(tab->search_feedback), ==, "1 / 1");
    g_assert_false(checked(tab->follow));
    gtk_editable_set_text(GTK_EDITABLE(tab->search), "pending");
    search_key(NULL, GDK_KEY_Escape, 0, 0, tab); spin(300);
    g_assert_false(tab->searching);
    g_assert_cmpstr(gtk_label_get_text(tab->search_feedback), ==, "");
    app.settings.font_size = 18; tio_console_font(GTK_WIDGET(tab->view), 18);
    g_assert_true(save(&app, NULL));
    TioSettings restored; tio_settings_init(&restored);
    g_assert_true(tio_settings_load_from_file(&restored, app.settings_path, NULL));
    g_assert_cmpuint(restored.font_size, ==, 18); tio_settings_clear(&restored);
    quit(&app); spin(30);
    tio_settings_clear(&app.settings); g_object_unref(app.application); g_ptr_array_unref(app.tabs);
    unlink(app.settings_path); g_free(app.settings_path); unlink(log); rmdir(directory); close(master);
    g_print("Serial GUI transport, logging, profiles and lifecycle passed\n"); return 0;
}
