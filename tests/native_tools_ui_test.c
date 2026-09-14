/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#define main serial_application_main
#include "../src/serial_main.c"
#undef main
#include <fcntl.h>
#include <unistd.h>
#include "modbus.h"
static void spin(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end) { while (g_get_monotonic_time() < end && g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
}
static void expect(int fd, const guint8 *expected, gsize length)
{
    guint8 actual[4096]; gsize total = 0;
    for (guint i = 0; i < 400 && total < length; i++) {
        spin(5); gssize n = read(fd, actual + total, sizeof actual - total); if (n > 0) total += n;
    }
    g_assert_cmpmem(actual, total, expected, length);
}
static void screenshot(GtkWindow *window, const char *name)
{
    const char *directory = g_getenv("TIO_TEST_LAYOUT_DIR"); if (!directory) return;
    spin(350); g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(GTK_WIDGET(window));
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(GTK_WIDGET(window)), gtk_widget_get_height(GTK_WIDGET(window)));
    g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
    g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node, NULL);
    g_autofree gchar *path = g_build_filename(directory, name, NULL); g_assert_true(gdk_texture_save_to_png(texture, path));
}
typedef struct { gboolean done; gchar *text; GError *error; } Reply;
static void modbus_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; Reply *reply = data; reply->text = tio_modbus_request_finish(result, &reply->error); reply->done = TRUE;
}
static void replay_event(TioCaptureKind kind, const guint8 *bytes, gsize length, gint64 time_us, gpointer data)
{ (void)time_us; if (kind == TIO_CAPTURE_RX) g_byte_array_append(data, bytes, length); }
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); gtk_init();
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK); g_assert_cmpint(master, >=, 0);
    g_assert_cmpint(grantpt(master), ==, 0); g_assert_cmpint(unlockpt(master), ==, 0);
    g_autofree gchar *device = g_strdup(ptsname(master)); g_autofree gchar *directory = g_dir_make_tmp("tio-tools-ui-XXXXXX", NULL);
    App app = {0}; app.tabs = g_ptr_array_new(); tio_settings_init(&app.settings);
    app.settings_path = g_build_filename(directory, "serial.ini", NULL);
    app.application = gtk_application_new("io.github.keithxc.tio_gui.native.tools.test", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app.application), NULL, NULL));
    activate(app.application, &app); spin(300); Tab *tab = g_ptr_array_index(app.tabs, 0);
    text(tab->device, device); gtk_check_button_set_active(tab->reconnect, FALSE); connect_clicked(NULL, tab);
    for (guint i = 0; i < 200 && !tab->connected; i++) spin(5); g_assert_true(tab->connected);
    g_test_message("quick CRC, delay, cancel on disconnect");
    replace(&tab->config.quick_payloads[0], "01 03 00 00 00 01"); tab->config.quick_modes[0] = 1;
    tab->config.quick_endings[0] = 0; tab->config.quick_crcs[0] = 2; tab->config.quick_delays[0] = 120;
    quick_clicked(tab->quick.buttons[0], tab); spin(40); guint8 buffer[32]; g_assert_cmpint(read(master, buffer, sizeof buffer), <=, 0);
    const guint8 expected[] = {1,3,0,0,0,1,0x84,0x0a}; expect(master, expected, sizeof expected);
    quick_clicked(tab->quick.buttons[0], tab); stop_tab(tab); spin(180); g_assert_cmpint(read(master, buffer, sizeof buffer), <=, 0);
    connect_clicked(NULL, tab); for (guint i = 0; i < 200 && !tab->connected; i++) spin(5); g_assert_true(tab->connected);
    g_assert_cmpint(read(master, buffer, sizeof buffer), <=, 0);
    committed(NULL, "\\", tab); key_pressed(NULL, GDK_KEY_backslash, 0, 0, tab);
    expect(master, (const guint8 *)"\\\\", 2);
    key_pressed(NULL, GDK_KEY_A, 0, GDK_CONTROL_MASK | GDK_LOCK_MASK, tab);
    expect(master, (const guint8 *)"\001", 1);
    g_test_message("sequence pause/resume and exact bytes");
    TioSequence *sequence = tio_sequence_new("two steps"); tio_sequence_add(sequence, "00 FF", 1, 0, 0, 50); tio_sequence_add(sequence, "OK", 0, 1, 0, 20);
    tab->sequence_runner = tio_sequence_runner_new(sequence, FALSE, sequence_send, tab, NULL);
    tio_sequence_runner_pause(tab->sequence_runner, TRUE); spin(80); g_assert_cmpint(read(master, buffer, sizeof buffer), <=, 0);
    tio_sequence_runner_pause(tab->sequence_runner, FALSE); const guint8 sequenced[] = {0,255,'O','K','\n'}; expect(master, sequenced, sizeof sequenced);
    spin(150); g_assert_false(tio_sequence_runner_active(tab->sequence_runner));
    g_ptr_array_add(app.settings.sequences, tio_sequence_encode(sequence, NULL)); tio_sequence_free(sequence);
    g_test_message("Modbus RTU bridge supports repeated requests");
    modbus_rtu(NULL, tab); g_assert_nonnull(tab->modbus_window);
    screenshot(GTK_WINDOW(tab->modbus_window), "macos-modbus.png");
    for (guint pass = 0; pass < 3; pass++) {
        Reply reply = {0}; TioModbusRequest request = {.endpoint=tio_native_bridge_path(tab->bridge), .unit=1, .function=3, .address=pass, .quantity=1};
        tio_modbus_request_async(&request, NULL, modbus_done, &reply);
        g_autoptr(GBytes) frame = tio_modbus_frame(&request, NULL); gsize length; const guint8 *bytes = g_bytes_get_data(frame, &length); expect(master, bytes, length);
        g_autoptr(GByteArray) response = tio_payload_build("01 03 02 00 2A", TRUE, 0, 2, NULL);
        g_assert_cmpint(write(master, response->data, response->len), ==, response->len);
        for (guint i = 0; i < 300 && !reply.done; i++) spin(5);
        g_assert_true(reply.done); g_assert_no_error(reply.error); g_assert_nonnull(strstr(reply.text, "42")); g_free(reply.text); spin(60);
    }
    gtk_window_destroy(GTK_WINDOW(tab->modbus_window)); spin(100); g_assert_null(tab->bridge);
    g_test_message("format streams preserve split ANSI and raw binary");
    TioSerialFormat format; tio_serial_format_init(&format, 1000000);
    g_autoptr(GBytes) prefix = tio_serial_format(&format, (const guint8 *)"\033[3", 3, FALSE, "epoch", TRUE, 2000000);
    g_assert_cmpuint(g_bytes_get_size(prefix), ==, 0);
    g_autoptr(GBytes) plain = tio_serial_format(&format, (const guint8 *)"1mOK\033[0m\n", 9, TRUE, "epoch", TRUE, 2000000);
    gsize size; const char *text_ = g_bytes_get_data(plain, &size); g_assert_cmpmem(text_, size, "[2.000000] OK\n", 14);
    g_test_message("analyzer, sequence, quick editor and advanced layout");
    g_assert_cmpint(write(master, "INFO temp=23.5\nERROR test fault\n", 32), ==, 32); spin(100);
    on_analyzer_clicked(NULL, tab); screenshot(GTK_WINDOW(tab->analyzer_window), "macos-analyzer.png");
    gtk_window_destroy(GTK_WINDOW(tab->analyzer_window)); spin(300);
    on_sequences_clicked(NULL, tab); screenshot(GTK_WINDOW(tab->sequence_window), "macos-sequences.png");
    gtk_window_destroy(GTK_WINDOW(tab->sequence_window)); spin(300);
    quick_edit(NULL, tab); screenshot(tab->quick_window, "macos-quick.png"); gtk_widget_set_visible(GTK_WIDGET(tab->quick_window), FALSE); spin(300);
    gtk_widget_set_visible(tab->workspace.advanced, TRUE); screenshot(app.window, "macos-advanced.png");
    gtk_widget_set_visible(tab->workspace.advanced, FALSE); screenshot(app.window, "macos-main.png");
    gtk_check_button_set_active(tab->highlight_toggle, FALSE);
    const char *screen = "\033[?1049h\033[2J\033[HEmbedded Linux console\033[3;4H\033[32mCamera pipeline: running\033[0m\033[5;4H1920 x 1080  |  60 fps\033[7;4H中文终端 / UTF-8\033[?1h";
    g_assert_cmpint(write(master, screen, strlen(screen)), ==, strlen(screen)); spin(200); screenshot(app.window, "macos-terminal.png");
    key_pressed(NULL, GDK_KEY_Up, 0, 0, tab); expect(master, (const guint8 *)"\033OA", 3);
    gtk_check_button_set_active(tab->follow, FALSE);
    key_pressed(NULL, GDK_KEY_Return, 0, 0, tab); expect(master, (const guint8 *)"\r", 1);
    g_assert_true(checked(tab->follow));
    show_search(NULL, tab); g_assert_true(checked(tab->highlight_toggle));
    g_assert_true(gtk_search_bar_get_search_mode(tab->search_bar));
    gtk_search_bar_set_search_mode(tab->search_bar, FALSE);
    g_assert_cmpint(write(master, "\033[?1049l", 8), ==, 8); spin(80); gtk_check_button_set_active(tab->highlight_toggle, TRUE);
    g_test_message("portable settings import refuses live sessions and preserves previous settings backup");
    text(tab->profile, "board"); profile_save(NULL, tab);
    g_autofree gchar *portable = g_build_filename(directory, "portable.ini", NULL);
    g_assert_true(tio_settings_export_portable(&app.settings, portable, NULL));
    g_autoptr(GError) busy = NULL; g_assert_false(import_settings(&app, portable, &busy)); g_assert_error(busy, G_IO_ERROR, G_IO_ERROR_BUSY);
    stop_tab(tab); g_assert_true(import_settings(&app, portable, NULL)); tab = g_ptr_array_index(app.tabs, 0);
    g_assert_cmpuint(app.settings.profiles->len, ==, 1); g_assert_cmpuint(app.settings.sequences->len, ==, 1);
    g_autofree gchar *backup = g_strconcat(app.settings_path, ".before-import", NULL); g_assert_true(g_file_test(backup, G_FILE_TEST_EXISTS));
    g_test_message("quit drains asynchronous recording before freeing tabs");
    text(tab->device, device); gtk_check_button_set_active(tab->reconnect, FALSE); connect_clicked(NULL, tab);
    for (guint i = 0; i < 200 && !tab->connected; i++) spin(5); g_assert_true(tab->connected);
    g_autofree gchar *capture = g_build_filename(directory, "capture.tiocap", NULL); g_assert_true(start_capture(tab, capture, NULL));
    const guint8 binary[] = {0,1,17,19,128,255,'\n'}; guint64 before = tab->rx;
    g_assert_cmpint(write(master, binary, sizeof binary), ==, sizeof binary);
    for (guint i = 0; i < 200 && tab->rx == before; i++) spin(5); g_assert_cmpuint(tab->rx - before, ==, sizeof binary);
    quit(&app); for (guint i = 0; i < 400 && app.tabs->len; i++) spin(5); g_assert_cmpuint(app.tabs->len, ==, 0);
    g_autoptr(GByteArray) replayed = g_byte_array_new(); TioReplay *replay = tio_replay_new(capture, replay_event, replayed, NULL); g_assert_nonnull(replay);
    for (guint i = 0; i < 400 && !tio_replay_finished(replay); i++) spin(5); g_assert_true(tio_replay_finished(replay));
    g_assert_cmpmem(replayed->data, replayed->len, binary, sizeof binary); tio_replay_free(replay);
    close(master); g_object_unref(app.application); tio_settings_clear(&app.settings); g_ptr_array_unref(app.tabs); g_free(app.settings_path);
    g_test_message("Native serial tools acceptance passed"); return 0;
}
