/* SPDX-License-Identifier: GPL-3.0-only */
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_serial.h"
#include "settings.h"
#include "highlighter.h"
#include "payload.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(G_OS_WIN32)
#include <windows.h>
#endif

static void bundled_runtime(void)
{
    g_autofree gchar *prefix = NULL;
#ifdef __APPLE__
    uint32_t size = 0; _NSGetExecutablePath(NULL, &size);
    g_autofree gchar *path = g_malloc(size);
    if (_NSGetExecutablePath(path, &size)) return;
    g_autofree gchar *directory = g_path_get_dirname(path);
    prefix = g_canonicalize_filename("../Resources", directory);
#elif defined(G_OS_WIN32)
    wchar_t path[32768]; DWORD length = GetModuleFileNameW(NULL, path, G_N_ELEMENTS(path));
    if (!length || length >= G_N_ELEMENTS(path)) return;
    g_autofree gchar *utf8 = g_utf16_to_utf8((gunichar2 *)path, length, NULL, NULL, NULL);
    g_autofree gchar *directory = g_path_get_dirname(utf8);
    prefix = g_canonicalize_filename("..", directory);
#else
    return;
#endif
    g_autofree gchar *share = g_build_filename(prefix, "share", NULL);
    g_autofree gchar *schemas = g_build_filename(share, "glib-2.0", "schemas", NULL);
    if (!g_file_test(schemas, G_FILE_TEST_IS_DIR)) return;
    g_setenv("XDG_DATA_DIRS", share, TRUE); g_setenv("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_autofree gchar *template_path = g_build_filename(prefix, "loaders.cache.in", NULL);
    g_autofree gchar *template = NULL;
    g_file_get_contents(template_path, &template, NULL, NULL);
#ifdef G_OS_WIN32
    if (!template) {
        g_autofree gchar *helper = g_build_filename(prefix, "bin", "gdk-pixbuf-query-loaders.exe", NULL);
        g_autofree gchar *modules = g_build_filename(prefix, "lib", "gdk-pixbuf-2.0", "2.10.0", "loaders", NULL);
        g_setenv("GDK_PIXBUF_MODULEDIR", modules, TRUE);
        g_autoptr(GSubprocess) query = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, helper, NULL);
        if (query) g_subprocess_communicate_utf8(query, NULL, NULL, &template, NULL, NULL);
    }
#endif
    if (template) {
        g_auto(GStrv) pieces = g_strsplit(template, "@BUNDLE_RESOURCES@", -1);
        g_autofree gchar *cache = g_strjoinv(prefix, pieces);
        g_autofree gchar *directory = g_build_filename(g_get_user_cache_dir(), "tio-gui", NULL);
        g_mkdir_with_parents(directory, 0700);
        g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, prefix, -1);
        g_autofree gchar *path = g_build_filename(directory, digest, NULL);
        if (g_file_set_contents(path, cache, -1, NULL)) g_setenv("GDK_PIXBUF_MODULE_FILE", path, TRUE);
    }
}

typedef struct _App App;
typedef struct {
    App *app;
    GtkWidget *page, *controls;
    GtkLabel *title, *status, *counts;
    GtkEntry *device, *baud, *send, *log_path, *profile;
    GtkDropDown *ports, *bits, *stops, *parity, *flow, *ending;
    GtkCheckButton *reconnect, *echo, *hex_rx, *hex_tx, *logging, *follow;
    GtkButton *connect;
    GtkTextView *view;
    GtkSearchEntry *search;
    GtkScrolledWindow *scroll;
    GtkEntry *quick_text[4];
    GtkCheckButton *quick_hex[4];
    GtkDropDown *quick_ending[4];
    TioHighlighter *highlighter;
    TioNativeSerial *serial;
    TioSessionConfig config;
    guint timer, hex_column;
    guint64 rx, tx;
    gboolean connected, closing, stopping, refreshing;
    FILE *log;
} Tab;
struct _App {
    GtkApplication *application;
    GtkWindow *window;
    GtkNotebook *notebook;
    GPtrArray *tabs;
    TioSettings settings;
    gchar *settings_path;
    gboolean closing;
};
static void new_tab(App *app, const TioSessionConfig *config);
static void stop_tab(Tab *tab);
static void set_status(Tab *tab, const char *text) { gtk_label_set_text(tab->status, text); }
static const char *entry(GtkEntry *e) { return gtk_editable_get_text(GTK_EDITABLE(e)); }
static void text(GtkEntry *e, const char *value) { gtk_editable_set_text(GTK_EDITABLE(e), value ? value : ""); }
static guint choice(GtkDropDown *d) { return gtk_drop_down_get_selected(d); }
static gboolean checked(GtkCheckButton *b) { return gtk_check_button_get_active(b); }
static void replace(gchar **destination, const char *value) { g_free(*destination); *destination = g_strdup(value); }
static void append(GtkWidget *box, GtkWidget *widget) { gtk_box_append(GTK_BOX(box), widget); }
static GtkWidget *row(void) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); }
static GtkWidget *button(GtkWidget *box, const char *label, GCallback callback, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label); append(box, b);
    g_signal_connect(b, "clicked", callback, data); return b;
}
static GtkEntry *field(GtkWidget *box, const char *label, const char *value, int width)
{
    if (label) append(box, gtk_label_new(label));
    GtkEntry *e = GTK_ENTRY(gtk_entry_new()); text(e, value);
    gtk_editable_set_width_chars(GTK_EDITABLE(e), width); append(box, GTK_WIDGET(e)); return e;
}
static GtkCheckButton *check(GtkWidget *box, const char *label, gboolean active)
{
    GtkCheckButton *b = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(label));
    gtk_check_button_set_active(b, active); append(box, GTK_WIDGET(b)); return b;
}
static GtkDropDown *dropdown(GtkWidget *box, const char *label, const char *const *items, guint selected)
{
    if (label) append(box, gtk_label_new(label));
    GtkDropDown *d = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(items));
    gtk_drop_down_set_selected(d, selected); append(box, GTK_WIDGET(d)); return d;
}
static void snapshot(Tab *tab)
{
    TioSessionConfig *c = &tab->config;
    replace(&c->device, entry(tab->device)); replace(&c->baud, entry(tab->baud));
    const char *bits[] = {"5", "6", "7", "8"}, *stops[] = {"1", "2"};
    const char *parity[] = {"none", "odd", "even"}, *flow[] = {"none", "hard", "soft"};
    const char *endings[] = {"none", "lf", "cr", "crlf"};
    replace(&c->data_bits, bits[MIN(choice(tab->bits), 3)]);
    replace(&c->stop_bits, stops[MIN(choice(tab->stops), 1)]);
    replace(&c->parity, parity[MIN(choice(tab->parity), 2)]);
    replace(&c->flow, flow[MIN(choice(tab->flow), 2)]);
    replace(&c->line_ending, endings[MIN(choice(tab->ending), 3)]);
    replace(&c->log_file, entry(tab->log_path));
    c->local_echo = checked(tab->echo); c->hex_output = checked(tab->hex_rx);
    c->reconnect = checked(tab->reconnect); c->logging = checked(tab->logging);
    for (guint i = 0; i < 4; i++) {
        replace(&c->quick_payloads[i], entry(tab->quick_text[i]));
        c->quick_modes[i] = checked(tab->quick_hex[i]);
        c->quick_endings[i] = choice(tab->quick_ending[i]);
    }
}
static gboolean save(App *app, GError **error)
{
    tio_settings_clear_tabs(&app->settings);
    for (guint i = 0; i < app->tabs->len; i++) {
        Tab *tab = g_ptr_array_index(app->tabs, i); snapshot(tab);
        tio_settings_add_tab(&app->settings, &tab->config);
    }
    g_autofree gchar *directory = g_path_get_dirname(app->settings_path);
    if (g_mkdir_with_parents(directory, 0700) < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "%s", g_strerror(errno)); return FALSE;
    }
    return tio_settings_save_to_file(&app->settings, app->settings_path, error);
}
static void follow_bottom(Tab *tab)
{
    if (!checked(tab->follow)) return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(tab->view);
    if (gtk_text_buffer_get_has_selection(buffer)) return;
    GtkTextIter end; gtk_text_buffer_get_end_iter(buffer, &end);
    GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "serial-bottom");
    if (!mark) mark = gtk_text_buffer_create_mark(buffer, "serial-bottom", &end, FALSE);
    else gtk_text_buffer_move_mark(buffer, mark, &end);
    gtk_text_view_scroll_to_mark(tab->view, mark, 0, FALSE, 0, 1);
}
static void display(Tab *tab, const guint8 *bytes, gsize length, gboolean received)
{
    if (received && checked(tab->hex_rx)) {
        GString *hex = g_string_sized_new(length * 3 + 1);
        for (gsize i = 0; i < length; i++) {
            g_string_append_printf(hex, "%02X%s", bytes[i], ++tab->hex_column == 16 ? "\n" : " ");
            tab->hex_column %= 16;
        }
        tio_highlighter_feed(tab->highlighter, (const guint8 *)hex->str, hex->len); g_string_free(hex, TRUE);
    } else tio_highlighter_feed(tab->highlighter, bytes, length);
    follow_bottom(tab);
}
static gboolean tick(gpointer data)
{
    Tab *tab = data;
    if (!tab->serial) return G_SOURCE_CONTINUE;
    TioNativeEvent *event;
    /* Bound work per frame; backend has an explicit queue-overflow disconnect. */
    gsize budget = 0; gboolean log_failed = FALSE, ended = FALSE;
    while (budget < 256 * 1024 && (event = tio_native_poll(tab->serial))) {
        if (event->kind == TIO_NATIVE_STATUS) {
            tab->connected = event->connected;
            if (!event->connected && !tab->config.reconnect) ended = TRUE;
            g_autofree gchar *message = g_strdup_printf("%s — %s", entry(tab->device), event->message);
            set_status(tab, message);
        } else {
            gsize length; const guint8 *bytes = g_bytes_get_data(event->bytes, &length); budget += length;
            if (event->kind == TIO_NATIVE_RX) {
                tab->rx += length;
                if (tab->log && fwrite(bytes, 1, length, tab->log) != length) log_failed = TRUE;
                display(tab, bytes, length, TRUE);
            } else { tab->tx += length; if (checked(tab->echo)) display(tab, bytes, length, FALSE); }
        }
        tio_native_event_free(event);
    }
    if (tab->log && fflush(tab->log) != 0) log_failed = TRUE;
    g_autofree gchar *counts = g_strdup_printf("RX %" G_GUINT64_FORMAT " bytes   TX %" G_GUINT64_FORMAT " bytes", tab->rx, tab->tx);
    gtk_label_set_text(tab->counts, counts);
    if (log_failed && !tab->stopping) { stop_tab(tab); set_status(tab, "Log write failed; session stopped. Check free space and permissions."); }
    else if (ended && !tab->stopping) {
        g_autofree gchar *message = g_strdup(gtk_label_get_text(tab->status));
        stop_tab(tab); set_status(tab, message);
    }
    return G_SOURCE_CONTINUE;
}
static void stop_tab(Tab *tab)
{
    tab->stopping = TRUE;
    /* Join first, then drain all confirmed RX/TX events before closing the log. */
    if (tab->serial) {
        TioNativeSerial *s = tab->serial;
        tio_native_finish(s);
        while (tio_native_pending(s)) tick(tab);
        tab->serial = NULL; tio_native_stop(s);
    }
    if (tab->log) { if (fclose(tab->log)) set_status(tab, "Could not finish writing the log"); tab->log = NULL; }
    tab->connected = FALSE;
    gtk_widget_set_sensitive(tab->controls, TRUE);
    gtk_button_set_label(tab->connect, "Connect");
    tab->stopping = FALSE;
}
static void connect_clicked(GtkButton *button_, gpointer data)
{
    (void)button_; Tab *tab = data;
    if (tab->serial) { stop_tab(tab); set_status(tab, "Disconnected"); return; }
    snapshot(tab);
    for (guint i = 0; i < tab->app->tabs->len; i++) {
        Tab *other = g_ptr_array_index(tab->app->tabs, i);
        if (other != tab && other->serial && !g_ascii_strcasecmp(entry(other->device), entry(tab->device))) {
            set_status(tab, "This port is already open in another tab"); return;
        }
    }
    char *end = NULL; guint64 baud = g_ascii_strtoull(entry(tab->baud), &end, 10);
    if (!*entry(tab->baud) || *end || baud < 1 || baud > 12000000) { set_status(tab, "Enter a baud rate between 1 and 12000000"); return; }
    TioNativeConfig config = {entry(tab->device), (guint)baud, choice(tab->bits) + 5,
        choice(tab->stops) + 1, choice(tab->parity), choice(tab->flow), checked(tab->reconnect)};
    g_autoptr(GError) error = NULL;
    if (checked(tab->logging)) {
        if (!*entry(tab->log_path)) { set_status(tab, "Choose a log file first"); return; }
        g_autofree gchar *directory = g_path_get_dirname(entry(tab->log_path));
        g_mkdir_with_parents(directory, 0700);
        tab->log = g_fopen(entry(tab->log_path), "ab");
        if (!tab->log) { set_status(tab, g_strerror(errno)); return; }
    }
    tab->serial = tio_native_start(&config, &error);
    if (!tab->serial) { if (tab->log) fclose(tab->log); tab->log = NULL; set_status(tab, error->message); return; }
    gtk_widget_set_sensitive(tab->controls, FALSE); gtk_button_set_label(tab->connect, "Disconnect");
    gtk_label_set_text(tab->title, entry(tab->device)); set_status(tab, "Connecting…");
    if (!save(tab->app, &error)) set_status(tab, error->message);
}
static void send_bytes(Tab *tab, GBytes *bytes)
{
    g_autoptr(GError) error = NULL;
    if (!tio_native_send(tab->serial, bytes, &error)) set_status(tab, error->message);
}
static void send_text(Tab *tab, const char *value, gboolean hex, guint ending)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) payload = tio_payload_build(value, hex, ending, 0, &error);
    if (!payload) { set_status(tab, error->message); return; }
    g_autoptr(GBytes) bytes = g_bytes_new(payload->data, payload->len); send_bytes(tab, bytes);
}
static void send_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget; Tab *tab = data;
    send_text(tab, entry(tab->send), checked(tab->hex_tx), choice(tab->ending));
    if (tab->connected) tio_settings_push_history(&tab->app->settings, entry(tab->send));
}
static void quick_clicked(GtkButton *b, gpointer data)
{
    Tab *tab = data; guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "index"));
    send_text(tab, entry(tab->quick_text[i]), checked(tab->quick_hex[i]), choice(tab->quick_ending[i]));
}
static void line_clicked(GtkButton *b, gpointer data)
{
    Tab *tab = data; guint action = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "line"));
    g_autoptr(GError) error = NULL;
    if (!tio_native_line(tab->serial, action / 2, action % 2, &error)) set_status(tab, error->message);
}
static void clear_clicked(GtkButton *b, gpointer data) { (void)b; Tab *tab = data; tio_highlighter_clear(tab->highlighter); tab->hex_column = 0; }
static void port_selected(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)pspec; Tab *tab = data;
    GtkStringObject *item = gtk_drop_down_get_selected_item(GTK_DROP_DOWN(object));
    if (item && !tab->serial && !tab->refreshing) text(tab->device, gtk_string_object_get_string(item));
}
static void refresh_clicked(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data; g_auto(GStrv) devices = tio_native_devices();
    GtkStringList *list = gtk_string_list_new((const char *const *)devices);
    tab->refreshing = TRUE;
    gtk_drop_down_set_model(tab->ports, G_LIST_MODEL(list));
    gtk_drop_down_set_selected(tab->ports, GTK_INVALID_LIST_POSITION);
    for (guint i = 0; devices[i]; i++) if (g_str_equal(entry(tab->device), devices[i])) gtk_drop_down_set_selected(tab->ports, i);
    tab->refreshing = FALSE; g_object_unref(list);
    if (!*entry(tab->device) && devices[0]) {
        text(tab->device, devices[0]); gtk_drop_down_set_selected(tab->ports, 0);
    }
}
static void search_next(GtkWidget *widget, gpointer data)
{
    (void)widget; Tab *tab = data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(tab->search));
    if (!*query) return;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(tab->view);
    GtkTextIter from, start, end;
    if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &from)) gtk_text_buffer_get_start_iter(buffer, &from);
    gboolean found = gtk_text_iter_forward_search(&from, query, GTK_TEXT_SEARCH_TEXT_ONLY | GTK_TEXT_SEARCH_CASE_INSENSITIVE, &start, &end, NULL);
    if (!found) { gtk_text_buffer_get_start_iter(buffer, &from); found = gtk_text_iter_forward_search(&from, query, GTK_TEXT_SEARCH_TEXT_ONLY | GTK_TEXT_SEARCH_CASE_INSENSITIVE, &start, &end, NULL); }
    if (found) { gtk_check_button_set_active(tab->follow, FALSE); gtk_text_buffer_select_range(buffer, &start, &end); gtk_text_view_scroll_to_iter(tab->view, &start, 0.1, FALSE, 0, 0); }
    else set_status(tab, "Text not found");
}
static void paste_done(GObject *source, GAsyncResult *result, gpointer data)
{
    /* Own the page until completion; closing marks it unusable before unref. */
    GtkWidget *page = data; Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    g_autoptr(GError) error = NULL; g_autofree gchar *value = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
    if (tab && !tab->closing && value) send_text(tab, value, FALSE, 0);
    g_object_unref(page);
}
static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer data)
{
    (void)controller; (void)code; Tab *tab = data;
    gboolean control = (state & GDK_CONTROL_MASK) != 0;
    gboolean copy_modifier = control && (state & GDK_SHIFT_MASK);
#ifdef __APPLE__
    copy_modifier = copy_modifier || (state & GDK_META_MASK);
#endif
    if (copy_modifier && (key == GDK_KEY_c || key == GDK_KEY_C)) {
        gtk_text_buffer_copy_clipboard(gtk_text_view_get_buffer(tab->view), gtk_widget_get_clipboard(GTK_WIDGET(tab->view))); return TRUE;
    }
    if (copy_modifier && (key == GDK_KEY_v || key == GDK_KEY_V)) {
        gdk_clipboard_read_text_async(gtk_widget_get_clipboard(GTK_WIDGET(tab->view)), NULL, paste_done, g_object_ref(tab->page)); return TRUE;
    }
    const char *sequence = NULL;
    switch (key) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: sequence = "\r"; break;
    case GDK_KEY_BackSpace: sequence = "\177"; break;
    case GDK_KEY_Tab: sequence = "\t"; break;
    case GDK_KEY_Escape: sequence = "\033"; break;
    case GDK_KEY_Up: sequence = "\033[A"; break;
    case GDK_KEY_Down: sequence = "\033[B"; break;
    case GDK_KEY_Right: sequence = "\033[C"; break;
    case GDK_KEY_Left: sequence = "\033[D"; break;
    case GDK_KEY_Home: sequence = "\033[H"; break;
    case GDK_KEY_End: sequence = "\033[F"; break;
    case GDK_KEY_Delete: sequence = "\033[3~"; break;
    default: break;
    }
    char byte;
    if (control && key >= GDK_KEY_a && key <= GDK_KEY_z) { byte = (char)(key - GDK_KEY_a + 1); g_autoptr(GBytes) bytes = g_bytes_new(&byte, 1); send_bytes(tab, bytes); return TRUE; }
    if (sequence) { send_text(tab, sequence, FALSE, 0); return TRUE; }
    /* Some native IM contexts leave ordinary keys unhandled. The controller
     * emits key-pressed only after IM filtering, so this cannot double-send a
     * committed character. Keep application shortcuts out of the serial stream. */
    if (!(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK | GDK_SUPER_MASK))) {
        gunichar character = gdk_keyval_to_unicode(key);
        if (character && g_unichar_isprint(character)) {
            char utf8[7] = {0}; g_unichar_to_utf8(character, utf8);
            send_text(tab, utf8, FALSE, 0); return TRUE;
        }
    }
    return FALSE;
}
static void input_focus_enter(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller; gtk_im_context_focus_in(GTK_IM_CONTEXT(data));
}
static void input_focus_leave(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller; gtk_im_context_focus_out(GTK_IM_CONTEXT(data));
}
static void committed(GtkIMContext *context, const char *value, gpointer data) { (void)context; send_text(data, value, FALSE, 0); }
static gboolean scroll_console(GtkEventControllerScroll *controller, double dx, double dy, gpointer data)
{
    (void)controller; (void)dx; Tab *tab = data;
    if (dy < 0) gtk_check_button_set_active(tab->follow, FALSE);
    return FALSE;
}
static void profile_save(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data; if (!*entry(tab->profile)) { set_status(tab, "Enter a profile name"); return; }
    snapshot(tab); tio_settings_store_profile(&tab->app->settings, entry(tab->profile), &tab->config);
    g_autoptr(GError) error = NULL; set_status(tab, save(tab->app, &error) ? "Profile saved" : error->message);
}
static void profile_load(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data; TioProfile *profile = tio_settings_find_profile(&tab->app->settings, entry(tab->profile));
    if (profile) new_tab(tab->app, &profile->session); else set_status(tab, "Profile not found; enter its saved name");
}
static void log_selected(GObject *dialog, GAsyncResult *result, gpointer data)
{
    GtkWidget *page = data; Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(dialog), result, NULL);
    if (file && tab && !tab->closing && !tab->serial) {
        g_autofree gchar *path = g_file_get_path(file); text(tab->log_path, path);
    }
    g_object_unref(page);
}
static void log_choose(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Receive log (raw bytes, append)");
    gtk_file_dialog_set_initial_name(dialog, "serial.log");
    gtk_file_dialog_save(dialog, tab->app->window, NULL, log_selected, g_object_ref(tab->page)); g_object_unref(dialog);
}
static void tab_free(Tab *tab)
{
    tab->closing = TRUE; g_object_set_data(G_OBJECT(tab->page), "tab", NULL);
    if (tab->timer) g_source_remove(tab->timer);
    stop_tab(tab); tio_highlighter_free(tab->highlighter); tio_session_config_clear(&tab->config); g_free(tab);
}
static void remove_tab(Tab *tab)
{
    App *app = tab->app; GtkWidget *page = tab->page; int index = gtk_notebook_page_num(app->notebook, page);
    g_ptr_array_remove(app->tabs, tab); tab_free(tab); gtk_notebook_remove_page(app->notebook, index);
    if (!app->tabs->len) new_tab(app, &app->settings.defaults);
}
static void close_tab_done(GObject *source, GAsyncResult *result, gpointer data)
{
    GtkWidget *page = data; Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL) == 1 && tab && !tab->closing) remove_tab(tab);
    g_object_unref(page);
}
static void close_tab_clicked(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data;
    if (!tab->serial) { remove_tab(tab); return; }
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Disconnect and close this session?");
    const char *buttons[] = {"Cancel", "Disconnect and close", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons); gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, tab->app->window, NULL, close_tab_done, g_object_ref(tab->page)); g_object_unref(dialog);
}
static void new_tab(App *app, const TioSessionConfig *config)
{
    Tab *tab = g_new0(Tab, 1); tab->app = app;
    tio_session_config_init(&tab->config); tio_session_config_copy(&tab->config, config);
    g_ptr_array_add(app->tabs, tab);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); tab->page = root;
    g_object_set_data(G_OBJECT(root), "tab", tab);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 10); gtk_widget_set_margin_bottom(root, 10);
    GtkWidget *top = row(); append(root, top);
    tab->controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); append(root, tab->controls);
    GtkWidget *ports = row(); append(tab->controls, ports);
    tab->device = field(ports, "Port", config->device, 18); gtk_widget_set_hexpand(GTK_WIDGET(tab->device), TRUE);
    const char *empty[] = {NULL}; tab->ports = dropdown(ports, NULL, empty, GTK_INVALID_LIST_POSITION);
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->ports), "Detected serial ports");
    g_signal_connect(tab->ports, "notify::selected", G_CALLBACK(port_selected), tab);
    button(ports, "Refresh", G_CALLBACK(refresh_clicked), tab);
    tab->baud = field(ports, "Baud", config->baud, 9);
    tab->connect = GTK_BUTTON(button(top, "Connect", G_CALLBACK(connect_clicked), tab));
    tab->status = GTK_LABEL(gtk_label_new("Disconnected")); gtk_label_set_xalign(tab->status, 0);
    gtk_label_set_ellipsize(tab->status, PANGO_ELLIPSIZE_END); gtk_widget_set_hexpand(GTK_WIDGET(tab->status), TRUE); append(top, GTK_WIDGET(tab->status));
    GtkWidget *framing = row(); append(tab->controls, framing);
    const char *bits[] = {"5", "6", "7", "8", NULL}, *stops[] = {"1", "2", NULL};
    const char *parity[] = {"None", "Odd", "Even", NULL}, *flow[] = {"None", "RTS/CTS", "XON/XOFF", NULL};
    tab->bits = dropdown(framing, "Data bits", bits, (guint)CLAMP(atoi(config->data_bits) - 5, 0, 3));
    tab->stops = dropdown(framing, "Stop bits", stops, g_str_equal(config->stop_bits, "2"));
    tab->parity = dropdown(framing, "Parity", parity, g_str_equal(config->parity, "odd") ? 1 : g_str_equal(config->parity, "even") ? 2 : 0);
    tab->flow = dropdown(framing, "Flow", flow, g_str_equal(config->flow, "hard") ? 1 : g_str_equal(config->flow, "soft") ? 2 : 0);
    tab->reconnect = check(framing, "Auto reconnect", config->reconnect);
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->reconnect), "Retry the same port once per second; pending sends are discarded after disconnect");
    GtkWidget *logrow = row(); append(tab->controls, logrow);
    tab->logging = check(logrow, "Log received bytes", config->logging);
    g_autoptr(GDateTime) now = g_date_time_new_now_local(); g_autofree gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S-%f");
    g_autofree gchar *name = g_strdup_printf("serial-%s.log", stamp);
    g_autofree gchar *default_log = g_build_filename(config->log_directory, name, NULL);
    tab->log_path = field(logrow, NULL, *config->log_file ? config->log_file : default_log, 30);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->log_path), TRUE); button(logrow, "Browse…", G_CALLBACK(log_choose), tab);
    GtkWidget *options = row(); append(root, options);
    tab->echo = check(options, "Local echo", config->local_echo); tab->hex_rx = check(options, "HEX receive", config->hex_output);
    tab->follow = check(options, "Follow", TRUE);
    button(options, "Clear", G_CALLBACK(clear_clicked), tab);
    const char *lines[] = {"DTR low", "DTR high", "RTS low", "RTS high", "Break"};
    for (guint i = 0; i < 5; i++) { GtkWidget *b = button(options, lines[i], G_CALLBACK(line_clicked), tab); g_object_set_data(G_OBJECT(b), "line", GUINT_TO_POINTER(i)); }
    GtkWidget *searchrow = row(); append(root, searchrow);
    tab->search = GTK_SEARCH_ENTRY(gtk_search_entry_new()); gtk_widget_set_hexpand(GTK_WIDGET(tab->search), TRUE); append(searchrow, GTK_WIDGET(tab->search));
    g_signal_connect(tab->search, "activate", G_CALLBACK(search_next), tab); button(searchrow, "Find next", G_CALLBACK(search_next), tab);
    tab->view = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_editable(tab->view, FALSE);
    gtk_text_view_set_monospace(tab->view, TRUE); gtk_text_view_set_cursor_visible(tab->view, FALSE);
    gtk_text_view_set_wrap_mode(tab->view, GTK_WRAP_CHAR); gtk_widget_add_css_class(GTK_WIDGET(tab->view), "serial-console");
    tab->highlighter = tio_highlighter_new(gtk_text_view_get_buffer(tab->view));
    tab->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new()); gtk_widget_set_vexpand(GTK_WIDGET(tab->scroll), TRUE);
    gtk_scrolled_window_set_child(tab->scroll, GTK_WIDGET(tab->view)); append(root, GTK_WIDGET(tab->scroll));
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    GtkIMContext *im = gtk_im_multicontext_new(); gtk_im_context_set_client_widget(im, GTK_WIDGET(tab->view));
    gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(keys), im);
    g_signal_connect(im, "commit", G_CALLBACK(committed), tab);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_object_set_data_full(G_OBJECT(focus), "im-context", g_object_ref(im), g_object_unref);
    g_signal_connect(focus, "enter", G_CALLBACK(input_focus_enter), im);
    g_signal_connect(focus, "leave", G_CALLBACK(input_focus_leave), im);
    gtk_widget_add_controller(GTK_WIDGET(tab->view), focus);
    g_object_unref(im);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), tab); gtk_widget_add_controller(GTK_WIDGET(tab->view), keys);
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_console), tab); gtk_widget_add_controller(GTK_WIDGET(tab->view), scroll);
    GtkWidget *sendrow = row(); append(root, sendrow);
    tab->send = field(sendrow, NULL, "", 28); gtk_widget_set_hexpand(GTK_WIDGET(tab->send), TRUE);
    gtk_entry_set_placeholder_text(tab->send, "Command or HEX bytes");
    g_signal_connect(tab->send, "activate", G_CALLBACK(send_clicked), tab);
    tab->hex_tx = check(sendrow, "HEX send", FALSE);
    const char *endings[] = {"None", "LF", "CR", "CR+LF", NULL};
    guint ending = g_str_equal(config->line_ending, "lf") ? 1 : g_str_equal(config->line_ending, "cr") ? 2 : g_str_equal(config->line_ending, "crlf") ? 3 : 0;
    tab->ending = dropdown(sendrow, "Ending", endings, ending); button(sendrow, "Send", G_CALLBACK(send_clicked), tab);
    GtkWidget *expander = gtk_expander_new("Quick sends and profiles"); append(root, expander);
    GtkWidget *quick = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_expander_set_child(GTK_EXPANDER(expander), quick);
    for (guint i = 0; i < 4; i++) {
        GtkWidget *r = row(); append(quick, r);
        tab->quick_text[i] = field(r, config->quick_labels[i], config->quick_payloads[i], 30); gtk_widget_set_hexpand(GTK_WIDGET(tab->quick_text[i]), TRUE);
        tab->quick_hex[i] = check(r, "HEX", config->quick_modes[i]);
        tab->quick_ending[i] = dropdown(r, "Ending", endings, MIN(config->quick_endings[i], 3));
        GtkWidget *b = button(r, "Send", G_CALLBACK(quick_clicked), tab); g_object_set_data(G_OBJECT(b), "index", GUINT_TO_POINTER(i));
    }
    GtkWidget *profiles = row(); append(quick, profiles); tab->profile = field(profiles, "Profile", "", 24);
    button(profiles, "Save", G_CALLBACK(profile_save), tab); button(profiles, "Open in new tab", G_CALLBACK(profile_load), tab);
    tab->counts = GTK_LABEL(gtk_label_new("RX 0 bytes   TX 0 bytes")); gtk_label_set_xalign(tab->counts, 0); append(root, GTK_WIDGET(tab->counts));
    GtkWidget *label = row(); tab->title = GTK_LABEL(gtk_label_new(config->device && *config->device ? config->device : "Serial")); append(label, GTK_WIDGET(tab->title));
    button(label, "×", G_CALLBACK(close_tab_clicked), tab);
    int page = gtk_notebook_append_page(app->notebook, root, label); gtk_notebook_set_current_page(app->notebook, page);
    tab->timer = g_timeout_add(30, tick, tab); refresh_clicked(NULL, tab);
}
static void add_clicked(GtkButton *b, gpointer data) { (void)b; App *app = data; new_tab(app, &app->settings.defaults); }
static void quit(App *app)
{
    g_autoptr(GError) error = NULL;
    if (!save(app, &error)) {
        GtkAlertDialog *dialog = gtk_alert_dialog_new("Could not save settings: %s", error->message); gtk_alert_dialog_show(dialog, app->window); g_object_unref(dialog); return;
    }
    app->closing = TRUE;
    for (guint i = 0; i < app->tabs->len; i++) tab_free(g_ptr_array_index(app->tabs, i));
    g_ptr_array_set_size(app->tabs, 0); gtk_window_destroy(app->window);
}
static void quit_done(GObject *source, GAsyncResult *result, gpointer data)
{
    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL) == 1) quit(data);
}
static gboolean close_requested(GtkWindow *window, gpointer data)
{
    (void)window; App *app = data; if (app->closing) return FALSE;
    for (guint i = 0; i < app->tabs->len; i++) if (((Tab *)g_ptr_array_index(app->tabs, i))->serial) {
        GtkAlertDialog *dialog = gtk_alert_dialog_new("Disconnect all serial sessions and quit?");
        const char *buttons[] = {"Cancel", "Disconnect and quit", NULL}; gtk_alert_dialog_set_buttons(dialog, buttons); gtk_alert_dialog_set_cancel_button(dialog, 0);
        gtk_alert_dialog_choose(dialog, app->window, NULL, quit_done, app); g_object_unref(dialog); return TRUE;
    }
    quit(app); return TRUE;
}
static void activate(GtkApplication *application, gpointer data)
{
    App *app = data; if (app->window) { gtk_window_present(app->window); return; }
    app->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(app->window, "tio-gui " TIO_GUI_VERSION " — Serial"); gtk_window_set_default_size(app->window, 1100, 760);
    g_signal_connect(app->window, "close-request", G_CALLBACK(close_requested), app);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_window_set_child(app->window, root);
    GtkWidget *header = gtk_header_bar_new(); gtk_window_set_titlebar(app->window, header);
    GtkWidget *add = gtk_button_new_with_label("New session"); gtk_header_bar_pack_start(GTK_HEADER_BAR(header), add); g_signal_connect(add, "clicked", G_CALLBACK(add_clicked), app);
    GtkWidget *note = gtk_label_new("Serial console · UTF-8 / HEX"); gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), note);
    app->notebook = GTK_NOTEBOOK(gtk_notebook_new()); gtk_notebook_set_scrollable(app->notebook, TRUE); gtk_widget_set_vexpand(GTK_WIDGET(app->notebook), TRUE); append(root, GTK_WIDGET(app->notebook));
    GtkCssProvider *css = gtk_css_provider_new(); gtk_css_provider_load_from_string(css, ".serial-console text { background: #10151d; color: #e6edf3; } .serial-console { color: #e6edf3; padding: 8px; font-size: 13px; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(app->window)), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(css);
    for (guint i = 0; i < app->settings.tab_configs->len; i++) new_tab(app, g_ptr_array_index(app->settings.tab_configs, i));
    if (!app->tabs->len) new_tab(app, &app->settings.defaults);
    gtk_window_present(app->window);
}
int main(int argc, char **argv)
{
    if (argc == 2 && g_str_equal(argv[1], "--version")) { g_print("tio-gui %s (native serial)\n", TIO_GUI_VERSION); return 0; }
    bundled_runtime();
    App app = {0}; app.tabs = g_ptr_array_new(); tio_settings_init(&app.settings);
    app.settings_path = g_build_filename(g_get_user_config_dir(), "tio-gui", "serial.ini", NULL);
    if (g_file_test(app.settings_path, G_FILE_TEST_EXISTS)) {
        g_autoptr(GError) error = NULL;
        if (!tio_settings_load_from_file(&app.settings, app.settings_path, &error)) g_printerr("Settings: %s\n", error->message);
    }
    app.application = gtk_application_new("io.github.keithxc.tio_gui.serial", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    int result = g_application_run(G_APPLICATION(app.application), argc, argv);
    g_object_unref(app.application); tio_settings_clear(&app.settings); g_ptr_array_unref(app.tabs); g_free(app.settings_path); return result;
}
