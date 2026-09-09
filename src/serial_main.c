/* SPDX-License-Identifier: GPL-3.0-only */
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>
#include "native_catalog.h"
static gboolean native_chinese = TRUE;
static const char *native_translate(const char *id) G_GNUC_FORMAT(1);
static const char *native_translate(const char *id)
{
    if (!native_chinese) return id;
    gsize low = 0, high = G_N_ELEMENTS(native_catalog);
    while (low < high) {
        gsize mid = low + (high - low) / 2;
        int order = strcmp(id, native_catalog[mid].id);
        if (!order) return native_catalog[mid].value;
        if (order < 0) high = mid; else low = mid + 1;
    }
    return id;
}
#undef _
#define _(id) native_translate(id)
#include "native_serial.h"
#include "settings.h"
#include "highlighter.h"
#include "payload.h"
#include "console_search.h"
#include "workspace_ui.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(G_OS_WIN32)
#include <windows.h>
#endif

static gchar *native_font_path;
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
#ifdef G_OS_WIN32
    native_font_path = g_build_filename(share, "fonts", "NotoSansSC-Regular.otf", NULL);
#endif
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
    TioWorkspaceUi workspace;
    TioConnectionUi connection;
    TioQuickUi quick;
    GtkSearchBar *search_bar;
    GtkWindow *quick_window;
    GtkButton *send_button;
    GtkWidget *bottom_button;
    GtkWidget *page, *controls, *advanced, *search_row;
    GtkLabel *title, *status, *counts;
    GtkEntry *device, *baud, *send, *log_path, *profile;
    GtkDropDown *ports, *bits, *stops, *parity, *flow, *ending;
    GtkCheckButton *reconnect, *echo, *hex_tx, *logging, *follow;
    GtkButton *connect;
    GtkTextView *view;
    GtkSearchEntry *search;
    GtkToggleButton *search_case, *search_regex, *hex_rx;
    GtkLabel *search_feedback;
    guint search_refresh;
    gboolean searching;
    gchar *search_query_key;
    GtkScrolledWindow *scroll;
    GtkEntry *quick_text[4], *quick_label[4];
    GtkDropDown *quick_mode[4];
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
    gboolean closing, translating;
    GtkDropDown *theme, *language;
    guint system_theme_timer;
};
static void new_tab(App *app, const TioSessionConfig *config);
static void stop_tab(Tab *tab);
static void connection_sensitive(Tab *tab, gboolean sensitive);
static void show_search(GtkButton *button, gpointer data);
static void set_status(Tab *tab, const char *text) { gtk_label_set_text(tab->status, text); }
static const char *entry(GtkEntry *e) { return gtk_editable_get_text(GTK_EDITABLE(e)); }
static void text(GtkEntry *e, const char *value) { gtk_editable_set_text(GTK_EDITABLE(e), value ? value : ""); }
static guint choice(GtkDropDown *d) { return gtk_drop_down_get_selected(d); }
static gboolean checked(GtkCheckButton *b) { return gtk_check_button_get_active(b); }
static void replace(gchar **destination, const char *value) { g_free(*destination); *destination = g_strdup(value); }
static void append(GtkWidget *box, GtkWidget *widget) { gtk_box_append(GTK_BOX(box), widget); }
static GtkWidget *row(void) { return tio_ui_row(8); }
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
    c->local_echo = checked(tab->echo); c->hex_output = gtk_toggle_button_get_active(tab->hex_rx);
    c->reconnect = checked(tab->reconnect); c->logging = checked(tab->logging);

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
static void native_language(const char *language)
{
    native_chinese = g_strcmp0(language, "en") != 0;
}
static void native_theme(App *app)
{
    GtkSettings *settings = gtk_settings_get_default();
    if (!g_strcmp0(app->settings.theme, "system")) {
#ifdef G_OS_WIN32
        /* GTK's Win32 backend does not consistently expose the Windows app
         * color preference through GtkSettings. Read the user preference. */
        DWORD light = 1, size = sizeof light;
        if (RegGetValueW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size) == ERROR_SUCCESS) {
            gboolean current = FALSE;
            g_object_get(settings, "gtk-application-prefer-dark-theme", &current, NULL);
            if (current != (light == 0))
                g_object_set(settings, "gtk-application-prefer-dark-theme", light == 0, NULL);
            return;
        }
#endif
        gtk_settings_reset_property(settings, "gtk-application-prefer-dark-theme");
    }
    else g_object_set(settings, "gtk-application-prefer-dark-theme",
                      !g_strcmp0(app->settings.theme, "dark"), NULL);
}
#ifdef G_OS_WIN32
static gboolean poll_system_theme(gpointer data)
{
    App *app = data;
    if (!g_strcmp0(app->settings.theme, "system")) native_theme(app);
    return G_SOURCE_CONTINUE;
}
#endif
static const char *translated_label(const char *value)
{
    if (!value) return NULL;
    for (guint i = 0; i < G_N_ELEMENTS(native_catalog); i++)
        if (!g_strcmp0(value, native_catalog[i].id) || !g_strcmp0(value, native_catalog[i].value))
            return native_translate(native_catalog[i].id);
    return value;
}
static void translate_widgets(GtkWidget *widget)
{
    if (g_object_get_data(G_OBJECT(widget), "user-content")) return;
    const char *tooltip = gtk_widget_get_tooltip_text(widget);
    if (tooltip) { g_autofree gchar *copy = g_strdup(translated_label(tooltip)); gtk_widget_set_tooltip_text(widget, copy); }
    if (GTK_IS_DROP_DOWN(widget)) {
        GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(widget));
        if (GTK_IS_STRING_LIST(model)) {
            guint selected = choice(GTK_DROP_DOWN(widget));
            guint count = g_list_model_get_n_items(model); gchar **items = g_new0(gchar *, count + 1);
            for (guint i = 0; i < count; i++) items[i] = g_strdup(translated_label(gtk_string_list_get_string(GTK_STRING_LIST(model), i)));
            GtkStringList *replacement = gtk_string_list_new((const char *const *)items);
            gtk_drop_down_set_model(GTK_DROP_DOWN(widget), G_LIST_MODEL(replacement));
            gtk_drop_down_set_selected(GTK_DROP_DOWN(widget), selected); g_object_unref(replacement); g_strfreev(items);
        }
        return;
    }
    if (GTK_IS_ENTRY(widget)) {
        const char *placeholder = gtk_entry_get_placeholder_text(GTK_ENTRY(widget));
        if (placeholder) { g_autofree gchar *copy = g_strdup(translated_label(placeholder)); gtk_entry_set_placeholder_text(GTK_ENTRY(widget), copy); }
        return;
    }
    if (GTK_IS_SEARCH_ENTRY(widget)) {
        gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(widget), _("Search terminal…")); return;
    }
    if (GTK_IS_LABEL(widget)) {
        g_autofree gchar *copy = g_strdup(translated_label(gtk_label_get_text(GTK_LABEL(widget))));
        gtk_label_set_text(GTK_LABEL(widget), copy); return;
    }
    if (GTK_IS_BUTTON(widget) && gtk_button_get_label(GTK_BUTTON(widget))) {
        g_autofree gchar *copy = g_strdup(translated_label(gtk_button_get_label(GTK_BUTTON(widget))));
        gtk_button_set_label(GTK_BUTTON(widget), copy); return;
    }
    if (GTK_IS_CHECK_BUTTON(widget)) {
        g_autofree gchar *copy = g_strdup(translated_label(gtk_check_button_get_label(GTK_CHECK_BUTTON(widget))));
        gtk_check_button_set_label(GTK_CHECK_BUTTON(widget), copy); return;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) translate_widgets(child);
}
static void preferences_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer data)
{
    (void)pspec; App *app = data;
    if (app->translating) return;
    const char *themes[] = {"system", "light", "dark"};
    const char *languages[] = {"zh_CN", "en"};
    guint selected = choice(dropdown);
    if (dropdown == app->theme && selected < G_N_ELEMENTS(themes)) {
        replace(&app->settings.theme, themes[selected]); native_theme(app);
    } else if (dropdown == app->language && selected < G_N_ELEMENTS(languages)) {
        if (!g_strcmp0(app->settings.language, languages[selected])) return;
        replace(&app->settings.language, languages[selected]); native_language(app->settings.language);
        app->translating = TRUE;
        translate_widgets(GTK_WIDGET(app->window));
        for (guint i = 0; i < app->tabs->len; i++) {
            Tab *tab = g_ptr_array_index(app->tabs, i);
            translate_widgets(GTK_WIDGET(tab->quick_window));
            gtk_window_set_title(tab->quick_window, _("Customize quick buttons"));
        }
        app->translating = FALSE;
    }
    g_autoptr(GError) error = NULL;
    if (!save(app, &error)) {
        GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", error->message);
        gtk_alert_dialog_show(dialog, app->window); g_object_unref(dialog);
    }
}
static GtkWidget *preferences(App *app)
{
    const char *languages[] = {"简体中文", "English", NULL};
    TioAppearanceUi ui = tio_appearance_ui_new(languages, !g_strcmp0(app->settings.language, "en"),
        !g_strcmp0(app->settings.theme, "dark") ? 2 : !g_strcmp0(app->settings.theme, "light") ? 1 : 0);
    app->theme = ui.theme; app->language = ui.language;
    g_object_set_data(G_OBJECT(app->language), "user-content", GINT_TO_POINTER(1));
    g_signal_connect(app->theme, "notify::selected", G_CALLBACK(preferences_changed), app);
    g_signal_connect(app->language, "notify::selected", G_CALLBACK(preferences_changed), app);
    return ui.root;
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
    if (received && gtk_toggle_button_get_active(tab->hex_rx)) {
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
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send), tab->connected);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_button), tab->connected);
    for (guint i = 0; i < 4; i++) gtk_widget_set_sensitive(GTK_WIDGET(tab->quick.buttons[i]), tab->connected);
    if (!tab->serial) return G_SOURCE_CONTINUE;
    TioNativeEvent *event;
    /* Bound work per frame; backend has an explicit queue-overflow disconnect. */
    gsize budget = 0; gboolean log_failed = FALSE, ended = FALSE;
    while (budget < 256 * 1024 && (event = tio_native_poll(tab->serial))) {
        if (event->kind == TIO_NATIVE_STATUS) {
            tab->connected = event->connected;
            if (!event->connected && !tab->config.reconnect) ended = TRUE;
            g_autofree gchar *message = g_strdup_printf("%s — %s", entry(tab->device), !g_strcmp0(event->message, "Connected") ? _("Connected") : !g_strcmp0(event->message, "Disconnected") ? _("Disconnected") : event->message);
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
    g_autofree gchar *rx = g_strdup_printf("%" G_GUINT64_FORMAT, tab->rx);
    g_autofree gchar *tx = g_strdup_printf("%" G_GUINT64_FORMAT, tab->tx);
    g_autofree gchar *counts = g_strdup_printf(_("RX %s bytes   TX %s bytes"), rx, tx);
    gtk_label_set_text(tab->counts, counts);
    if (log_failed && !tab->stopping) { stop_tab(tab); set_status(tab, _("Log write failed; session stopped. Check free space and permissions.")); }
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
    if (tab->log) { if (fclose(tab->log)) set_status(tab, _("Could not finish writing the log")); tab->log = NULL; }
    tab->connected = FALSE;
    connection_sensitive(tab, TRUE);
    gtk_button_set_label(tab->connect, _("Connect"));
    tab->stopping = FALSE;
}
static void connect_clicked(GtkButton *button_, gpointer data)
{
    (void)button_; Tab *tab = data;
    if (tab->serial) { stop_tab(tab); set_status(tab, _("Disconnected")); return; }
    snapshot(tab);
    for (guint i = 0; i < tab->app->tabs->len; i++) {
        Tab *other = g_ptr_array_index(tab->app->tabs, i);
        if (other != tab && other->serial && !g_ascii_strcasecmp(entry(other->device), entry(tab->device))) {
            set_status(tab, _("This port is already open in another tab")); return;
        }
    }
    char *end = NULL; guint64 baud = g_ascii_strtoull(entry(tab->baud), &end, 10);
    if (!*entry(tab->baud) || *end || baud < 1 || baud > 12000000) { set_status(tab, _("Enter a baud rate between 1 and 12000000")); return; }
    TioNativeConfig config = {entry(tab->device), (guint)baud, choice(tab->bits) + 5,
        choice(tab->stops) + 1, choice(tab->parity), choice(tab->flow), checked(tab->reconnect)};
    g_autoptr(GError) error = NULL;
    if (checked(tab->logging)) {
        if (!*entry(tab->log_path)) { set_status(tab, _("Choose a log file first")); return; }
        g_autofree gchar *directory = g_path_get_dirname(entry(tab->log_path));
        g_mkdir_with_parents(directory, 0700);
        tab->log = g_fopen(entry(tab->log_path), "ab");
        if (!tab->log) { set_status(tab, g_strerror(errno)); return; }
    }
    tab->serial = tio_native_start(&config, &error);
    if (!tab->serial) { if (tab->log) fclose(tab->log); tab->log = NULL; set_status(tab, error->message); return; }
    connection_sensitive(tab, FALSE); gtk_button_set_label(tab->connect, _("Disconnect"));
    gtk_label_set_text(tab->title, entry(tab->device)); set_status(tab, _("Connecting…"));
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
    send_text(tab, tab->config.quick_payloads[i], tab->config.quick_modes[i], tab->config.quick_endings[i]);
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
    gboolean found = FALSE;
    for (guint i = 0; devices[i]; i++) if (!g_strcmp0(devices[i], entry(tab->device))) found = TRUE;
    if (!found && *entry(tab->device)) gtk_string_list_append(list, entry(tab->device));
    tab->refreshing = TRUE;
    gtk_drop_down_set_model(tab->ports, G_LIST_MODEL(list));
    gtk_drop_down_set_selected(tab->ports, GTK_INVALID_LIST_POSITION);
    for (guint i = 0; devices[i]; i++) if (g_str_equal(entry(tab->device), devices[i])) gtk_drop_down_set_selected(tab->ports, i);
    if (!found && *entry(tab->device)) gtk_drop_down_set_selected(tab->ports, g_list_model_get_n_items(G_LIST_MODEL(list)) - 1);
    tab->refreshing = FALSE; g_object_unref(list);
    if (!devices[0] && !*entry(tab->device)) set_status(tab, _("No serial devices found"));
    if (!*entry(tab->device) && devices[0]) {
        text(tab->device, devices[0]); gtk_drop_down_set_selected(tab->ports, 0);
    }
}
static void serial_search(Tab *tab, gboolean forward, gboolean reset)
{
    if (!tab->view) return;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(tab->search));
    g_autofree gchar *key = g_strdup_printf("%d:%d:%s", gtk_toggle_button_get_active(tab->search_case), gtk_toggle_button_get_active(tab->search_regex), query);
    if (!tab->searching || (!reset && g_strcmp0(key, tab->search_query_key))) reset = TRUE;
    g_free(tab->search_query_key); tab->search_query_key = g_strdup(key);
    tab->searching = *query != 0;
    if (*query) gtk_check_button_set_active(tab->follow, FALSE);
    tio_console_search(tab->view, query, gtk_toggle_button_get_active(tab->search_case), gtk_toggle_button_get_active(tab->search_regex),
                       forward, reset, tab->search_feedback, GTK_WIDGET(tab->search));
}
static gboolean refresh_serial_search(gpointer data)
{
    Tab *tab = data; tab->search_refresh = 0;
    if (tab->searching) tio_console_search(tab->view,
        gtk_editable_get_text(GTK_EDITABLE(tab->search)), gtk_toggle_button_get_active(tab->search_case), gtk_toggle_button_get_active(tab->search_regex),
        TRUE, TIO_SEARCH_REFRESH, tab->search_feedback, GTK_WIDGET(tab->search));
    return G_SOURCE_REMOVE;
}
static void serial_search_buffer_changed(GtkTextBuffer *buffer, gpointer data)
{
    (void)buffer; Tab *tab = data;
    if (!tab->closing && tab->searching && !tab->search_refresh)
        tab->search_refresh = g_timeout_add(250, refresh_serial_search, tab);
}
static void search_next(GtkWidget *widget, gpointer data)
{ (void)widget; serial_search(data, TRUE, FALSE); }
static void search_previous(GtkWidget *widget, gpointer data)
{ (void)widget; serial_search(data, FALSE, FALSE); }
static void search_changed(GtkWidget *widget, gpointer data)
{
    (void)widget; Tab *tab = data;
    g_autofree gchar *key = g_strdup_printf("%d:%d:%s", gtk_toggle_button_get_active(tab->search_case), gtk_toggle_button_get_active(tab->search_regex),
        gtk_editable_get_text(GTK_EDITABLE(tab->search)));
    if (g_strcmp0(key, tab->search_query_key)) serial_search(tab, TRUE, TRUE);
}
static gboolean search_key(GtkEventControllerKey *controller, guint key, guint code,
                           GdkModifierType state, gpointer data)
{
    (void)controller; (void)code; Tab *tab = data;
    if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter || key == GDK_KEY_F3) {
        serial_search(tab, !(state & GDK_SHIFT_MASK), FALSE); return TRUE;
    }
    if (key == GDK_KEY_Escape) {
        tab->searching = FALSE;
        gtk_search_bar_set_search_mode(tab->search_bar, FALSE);
        g_clear_handle_id(&tab->search_refresh, g_source_remove);
        g_free(tab->search_query_key);
        tab->search_query_key = g_strdup_printf("%d:%d:%s", gtk_toggle_button_get_active(tab->search_case), gtk_toggle_button_get_active(tab->search_regex),
            gtk_editable_get_text(GTK_EDITABLE(tab->search)));
        tio_console_search(tab->view, "", FALSE, FALSE, TRUE, TRUE,
                           tab->search_feedback, GTK_WIDGET(tab->search));
        gtk_widget_grab_focus(GTK_WIDGET(tab->view)); return TRUE;
    }
    return FALSE;
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
    if (key == GDK_KEY_F3) { gtk_search_bar_set_search_mode(tab->search_bar, TRUE); serial_search(tab, !(state & GDK_SHIFT_MASK), FALSE); return TRUE; }
    gboolean control = (state & GDK_CONTROL_MASK) != 0;
    if (control && (state & GDK_SHIFT_MASK) && (key == GDK_KEY_f || key == GDK_KEY_F)) {
        show_search(NULL, tab);
        gtk_editable_select_region(GTK_EDITABLE(tab->search), 0, -1); return TRUE;
    }
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
    (void)dx; Tab *tab = data;
    if (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)) & GDK_CONTROL_MASK) {
        if (dy == 0) return TRUE;
        tab->app->settings.font_size = (guint)CLAMP((gint)tab->app->settings.font_size + (dy < 0 ? 1 : -1), 6, 40);
        for (guint i = 0; i < tab->app->tabs->len; i++) {
            Tab *other = g_ptr_array_index(tab->app->tabs, i);
            tio_console_font(GTK_WIDGET(other->view), tab->app->settings.font_size);
        }
        g_autoptr(GError) error = NULL;
        if (!save(tab->app, &error)) set_status(tab, error->message);
        return TRUE;
    }
    if (dy < 0) gtk_check_button_set_active(tab->follow, FALSE);
    return FALSE;
}
static void profile_save(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data; if (!*entry(tab->profile)) { set_status(tab, _("Enter a profile name")); return; }
    snapshot(tab); tio_settings_store_profile(&tab->app->settings, entry(tab->profile), &tab->config);
    g_autoptr(GError) error = NULL; set_status(tab, save(tab->app, &error) ? _("Profile saved") : error->message);
}
static void profile_load(GtkButton *b, gpointer data)
{
    (void)b; Tab *tab = data; TioProfile *profile = tio_settings_find_profile(&tab->app->settings, entry(tab->profile));
    if (profile) new_tab(tab->app, &profile->session); else set_status(tab, _("Profile not found; enter its saved name"));
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
    gtk_file_dialog_set_title(dialog, _("Receive log (raw bytes, append)"));
    gtk_file_dialog_set_initial_name(dialog, "serial.log");
    gtk_file_dialog_save(dialog, tab->app->window, NULL, log_selected, g_object_ref(tab->page)); g_object_unref(dialog);
}
static void tab_free(Tab *tab)
{
    tab->closing = TRUE; g_object_set_data(G_OBJECT(tab->page), "tab", NULL);
    if (tab->timer) g_source_remove(tab->timer);
    g_clear_handle_id(&tab->search_refresh, g_source_remove);
    g_clear_pointer(&tab->search_query_key, g_free);
    stop_tab(tab); gtk_window_destroy(tab->quick_window); g_clear_object(&tab->follow); tio_highlighter_free(tab->highlighter); tio_session_config_clear(&tab->config); g_free(tab);
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
    GtkAlertDialog *dialog = gtk_alert_dialog_new(_("Disconnect and close this session?"));
    const char *buttons[] = {_("Cancel"), _("Disconnect and close"), NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons); gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, tab->app->window, NULL, close_tab_done, g_object_ref(tab->page)); g_object_unref(dialog);
}
static void show_search(GtkButton *button, gpointer data)
{
    (void)button; Tab *tab = data; gtk_search_bar_set_search_mode(tab->search_bar, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(tab->search));
}
static void connection_sensitive(Tab *tab, gboolean sensitive)
{
    GtkWidget *widgets[] = {GTK_WIDGET(tab->connection.device), GTK_WIDGET(tab->connection.baud),
        GTK_WIDGET(tab->connection.custom_baud), GTK_WIDGET(tab->connection.refresh),
        GTK_WIDGET(tab->device), GTK_WIDGET(tab->bits), GTK_WIDGET(tab->stops), GTK_WIDGET(tab->parity),
        GTK_WIDGET(tab->flow), GTK_WIDGET(tab->reconnect), GTK_WIDGET(tab->logging), GTK_WIDGET(tab->log_path)};
    for (guint i = 0; i < G_N_ELEMENTS(widgets); i++) gtk_widget_set_sensitive(widgets[i], sensitive);
}
static void baud_selected(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object; (void)pspec; Tab *tab = data; if (tab->app->translating) return; guint index = choice(tab->connection.baud);
    gtk_widget_set_visible(GTK_WIDGET(tab->baud), index == TIO_UI_CUSTOM_BAUD);
    if (index < TIO_UI_CUSTOM_BAUD) text(tab->baud, tio_ui_baud_rates[index]);
}
static gboolean workspace_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer data)
{
    (void)controller; (void)code; Tab *tab = data;
    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) && (key == GDK_KEY_f || key == GDK_KEY_F)) {
        show_search(NULL, tab); return TRUE;
    }
    if (key == GDK_KEY_F3) { show_search(NULL, tab); serial_search(tab, !(state & GDK_SHIFT_MASK), FALSE); return TRUE; }
    return FALSE;
}
static void manual_device_done(GtkEventControllerFocus *focus, gpointer data)
{
    (void)focus; Tab *tab = data; if (!tab->serial) refresh_clicked(NULL, tab);
}
static void search_mode_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object; (void)pspec; Tab *tab = data;
    if (!gtk_search_bar_get_search_mode(tab->search_bar)) search_key(NULL, GDK_KEY_Escape, 0, 0, tab);
}
static void quick_edit_save(GtkButton *button_, gpointer data)
{
    (void)button_; Tab *tab = data;
    for (guint i = 0; i < 4; i++) {
        replace(&tab->config.quick_payloads[i], entry(tab->quick_text[i]));
        tab->config.quick_modes[i] = choice(tab->quick_mode[i]);
        replace(&tab->config.quick_labels[i], entry(tab->quick_label[i]));
        gtk_button_set_label(tab->quick.buttons[i], tab->config.quick_labels[i]);
        tab->config.quick_endings[i] = choice(tab->quick_ending[i]);
    }
    g_autoptr(GError) error = NULL;
    if (!save(tab->app, &error)) { set_status(tab, error->message); return; }
    gtk_widget_set_visible(GTK_WIDGET(tab->quick_window), FALSE);
}
static void quick_edit(GtkButton *button_, gpointer data)
{
    (void)button_; Tab *tab = data;
    for (guint i = 0; i < 4; i++) {
        text(tab->quick_text[i], tab->config.quick_payloads[i]);
        gtk_drop_down_set_selected(tab->quick_mode[i], tab->config.quick_modes[i]);
        text(tab->quick_label[i], tab->config.quick_labels[i]);
        gtk_drop_down_set_selected(tab->quick_ending[i], tab->config.quick_endings[i]);
    }
    gtk_window_present(tab->quick_window);
}
static void follow_changed(GtkCheckButton *button_, gpointer data)
{
    Tab *tab = data; gtk_widget_set_visible(tab->bottom_button, !checked(button_));
    if (checked(button_)) follow_bottom(tab);
}
static void back_to_bottom(GtkButton *button_, gpointer data)
{
    (void)button_; Tab *tab = data; gtk_check_button_set_active(tab->follow, TRUE); follow_bottom(tab);
}
static void new_tab(App *app, const TioSessionConfig *config)
{
    Tab *tab = g_new0(Tab, 1); tab->app = app;
    tio_session_config_init(&tab->config); tio_session_config_copy(&tab->config, config);
    g_ptr_array_add(app->tabs, tab);
    tab->workspace = tio_workspace_ui_new(); GtkWidget *root = tab->workspace.root; tab->page = root;
    g_object_set_data(G_OBJECT(root), "tab", tab);
    tab->connection = tio_connection_ui_new(tab->workspace.toolbar);
    tab->ports = tab->connection.device; g_object_set_data(G_OBJECT(tab->ports), "user-content", GINT_TO_POINTER(1)); tab->baud = tab->connection.custom_baud;
    tab->controls = GTK_WIDGET(tab->ports); tab->connect = tab->connection.connect;
    text(tab->baud, config->baud);
    guint rate = TIO_UI_CUSTOM_BAUD;
    for (guint i = 0; i < TIO_UI_CUSTOM_BAUD; i++) if (!g_strcmp0(config->baud, tio_ui_baud_rates[i])) rate = i;
    gtk_drop_down_set_selected(tab->connection.baud, rate);
    gtk_widget_set_visible(GTK_WIDGET(tab->baud), rate == TIO_UI_CUSTOM_BAUD);
    g_signal_connect(tab->connection.baud, "notify::selected", G_CALLBACK(baud_selected), tab);
    g_signal_connect(tab->ports, "notify::selected", G_CALLBACK(port_selected), tab);
    g_signal_connect(tab->connection.refresh, "clicked", G_CALLBACK(refresh_clicked), tab);
    g_signal_connect(tab->connect, "clicked", G_CALLBACK(connect_clicked), tab);
    GtkWidget *options = tab->workspace.options;
    tab->logging = check(options, _("Log session"), config->logging);
    GtkWidget *exp = gtk_expander_new(_("Connection and logging")); append(options, exp);
    gtk_widget_set_valign(exp, GTK_ALIGN_CENTER);
    g_object_bind_property(exp, "expanded", tab->workspace.advanced, "visible", G_BINDING_SYNC_CREATE);
    tab->advanced = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_widget_add_css_class(tab->advanced, "settings-card");
    append(tab->workspace.advanced, tio_ui_advanced_scroll(tab->advanced));
    GtkWidget *profiles_menu = gtk_menu_button_new(); gtk_menu_button_set_label(GTK_MENU_BUTTON(profiles_menu), _("Profiles")); append(tab->advanced, profiles_menu);
    GtkWidget *profiles_pop = gtk_popover_new(); GtkWidget *profiles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); tio_ui_margins(profiles, 8);
    gtk_popover_set_child(GTK_POPOVER(profiles_pop), profiles); gtk_menu_button_set_popover(GTK_MENU_BUTTON(profiles_menu), profiles_pop);
    tab->profile = field(profiles, _("Profile"), "", 24);
    button(profiles, _("Save"), G_CALLBACK(profile_save), tab); button(profiles, _("Open in new tab"), G_CALLBACK(profile_load), tab);
    GtkWidget *manual = row(); append(tab->advanced, manual);
    tab->device = field(manual, _("Device"), config->device, 18); gtk_widget_set_hexpand(GTK_WIDGET(tab->device), TRUE);
    GtkWidget *framing = row(); append(tab->advanced, framing);
    const char *bits[] = {"5", "6", "7", "8", NULL}, *stops[] = {"1", "2", NULL};
    const char *parity[] = {_("None"), _("Odd"), _("Even"), NULL}, *flow[] = {_("None"), "RTS/CTS", "XON/XOFF", NULL};
    tab->bits = dropdown(framing, _("Data bits"), bits, (guint)CLAMP(atoi(config->data_bits) - 5, 0, 3));
    tab->stops = dropdown(framing, _("Stop bits"), stops, g_str_equal(config->stop_bits, "2"));
    framing = row(); append(tab->advanced, framing);
    tab->parity = dropdown(framing, _("Parity"), parity, g_str_equal(config->parity, "odd") ? 1 : g_str_equal(config->parity, "even") ? 2 : 0);
    tab->flow = dropdown(framing, _("Flow"), flow, g_str_equal(config->flow, "hard") ? 1 : g_str_equal(config->flow, "soft") ? 2 : 0);
    framing = row(); append(tab->advanced, framing);
    tab->echo = check(framing, _("Local echo"), config->local_echo);
    tab->hex_tx = check(framing, _("HEX send"), FALSE);
    GtkWidget *logrow = row(); append(tab->advanced, logrow);
    g_autoptr(GDateTime) now = g_date_time_new_now_local(); g_autofree gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S-%f");
    g_autofree gchar *name = g_strdup_printf("serial-%s.log", stamp);
    g_autofree gchar *default_log = g_build_filename(config->log_directory, name, NULL);
    tab->log_path = field(logrow, _("Log filename"), *config->log_file ? config->log_file : default_log, 30);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->log_path), TRUE); button(logrow, _("Browse…"), G_CALLBACK(log_choose), tab);
    GtkWidget *line_exp = gtk_expander_new(_("Serial lines")); append(tab->advanced, line_exp);
    GtkWidget *line_box = row(); gtk_expander_set_child(GTK_EXPANDER(line_exp), line_box);
    const char *lines[] = {_("DTR low"), _("DTR high"), _("RTS low"), _("RTS high"), _("Break")};
    for (guint i = 0; i < 5; i++) { GtkWidget *b = button(line_box, lines[i], G_CALLBACK(line_clicked), tab); g_object_set_data(G_OBJECT(b), "line", GUINT_TO_POINTER(i)); }
    tab->reconnect = check(tab->advanced, _("Auto reconnect"), config->reconnect);
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->reconnect), _("Retry the same port once per second; pending sends are discarded after disconnect"));
    tab->status = GTK_LABEL(gtk_label_new(_("Ready"))); gtk_label_set_xalign(tab->status, 0);
    gtk_label_set_ellipsize(tab->status, PANGO_ELLIPSIZE_END); gtk_widget_set_hexpand(GTK_WIDGET(tab->status), TRUE);
    tab->counts = GTK_LABEL(gtk_label_new("")); gtk_label_set_xalign(tab->counts, 1);
    gtk_label_set_ellipsize(tab->counts, PANGO_ELLIPSIZE_END); gtk_widget_set_hexpand(GTK_WIDGET(tab->counts), TRUE);
    GtkWidget *labels[] = {GTK_WIDGET(tab->status), GTK_WIDGET(tab->counts)};
    for (guint i = 0; i < 2; i++) { gtk_widget_add_css_class(labels[i], "dim-label"); gtk_widget_add_css_class(labels[i], "session-status"); append(options, labels[i]); }
    button(options, _("Clear"), G_CALLBACK(clear_clicked), tab);
    TioSearchUi search = tio_search_ui_new(); tab->search_bar = search.bar;
    tab->search_row = tab->workspace.search; append(tab->search_row, GTK_WIDGET(search.bar));
    g_object_bind_property(search.bar, "search-mode-enabled", tab->search_row, "visible", G_BINDING_SYNC_CREATE);
    tab->search = search.entry; tab->search_case = search.match_case; tab->search_regex = search.regex; tab->search_feedback = search.feedback;
    g_signal_connect(tab->search, "activate", G_CALLBACK(search_next), tab);
    g_signal_connect(tab->search, "search-changed", G_CALLBACK(search_changed), tab);
    g_signal_connect(search.previous, "clicked", G_CALLBACK(search_previous), tab);
    g_signal_connect(search.next, "clicked", G_CALLBACK(search_next), tab);
    g_signal_connect(tab->search_case, "toggled", G_CALLBACK(search_changed), tab);
    g_signal_connect(tab->search_regex, "toggled", G_CALLBACK(search_changed), tab);
    GtkEventController *search_keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(search_keys, GTK_PHASE_CAPTURE);
    g_signal_connect(search_keys, "key-pressed", G_CALLBACK(search_key), tab);
    gtk_widget_add_controller(GTK_WIDGET(tab->search), search_keys);
    tab->view = GTK_TEXT_VIEW(gtk_text_view_new()); tio_ui_console_text(tab->view);
    tio_console_font(GTK_WIDGET(tab->view), app->settings.font_size);
    tab->highlighter = tio_highlighter_new(gtk_text_view_get_buffer(tab->view));
    g_signal_connect(gtk_text_view_get_buffer(tab->view), "changed", G_CALLBACK(serial_search_buffer_changed), tab);
    tab->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new()); gtk_widget_set_vexpand(GTK_WIDGET(tab->scroll), TRUE);
    gtk_scrolled_window_set_child(tab->scroll, GTK_WIDGET(tab->view));
    GtkWidget *overlay = gtk_overlay_new(); gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(tab->scroll));
    tab->bottom_button = gtk_button_new_with_label(_("Back to bottom ↓"));
    gtk_widget_add_css_class(tab->bottom_button, "scroll-bottom");
    gtk_widget_set_halign(tab->bottom_button, GTK_ALIGN_END); gtk_widget_set_valign(tab->bottom_button, GTK_ALIGN_END);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), tab->bottom_button); gtk_widget_set_visible(tab->bottom_button, FALSE);
    g_signal_connect(tab->bottom_button, "clicked", G_CALLBACK(back_to_bottom), tab);
    tab->follow = GTK_CHECK_BUTTON(g_object_ref_sink(gtk_check_button_new())); gtk_check_button_set_active(tab->follow, TRUE);
    g_signal_connect(tab->follow, "toggled", G_CALLBACK(follow_changed), tab);
    append(tab->workspace.console, tio_ui_console_frame(overlay));
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
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_console), tab); gtk_widget_add_controller(GTK_WIDGET(tab->view), scroll);
    TioSendUi send = tio_send_ui_new(tab->workspace.send, FALSE);
    tab->send = send.entry; tab->ending = send.ending; tab->send_button = send.send;
    g_signal_connect(tab->send, "activate", G_CALLBACK(send_clicked), tab);
    g_signal_connect(send.send, "clicked", G_CALLBACK(send_clicked), tab);
    guint ending = g_str_equal(config->line_ending, "lf") ? 1 : g_str_equal(config->line_ending, "cr") ? 2 : g_str_equal(config->line_ending, "crlf") ? 3 : 0;
    gtk_drop_down_set_selected(tab->ending, ending);
    tab->quick = tio_quick_ui_new(tab->workspace.quick, FALSE, FALSE); tab->hex_rx = tab->quick.hex;
    gtk_toggle_button_set_active(tab->hex_rx, config->hex_output);
    tab->quick_window = GTK_WINDOW(gtk_window_new()); gtk_window_set_title(tab->quick_window, _("Customize quick buttons"));
    gtk_window_set_transient_for(tab->quick_window, app->window); gtk_window_set_modal(tab->quick_window, TRUE);
    gtk_window_set_hide_on_close(tab->quick_window, TRUE);
    gtk_window_set_default_size(tab->quick_window, 980, 620);
    GtkWidget *quick = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10); tio_ui_margins(quick, 12);
    GtkWidget *quick_scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(quick_scroll), quick); gtk_window_set_child(tab->quick_window, quick_scroll);
    GtkWidget *hint = gtk_label_new(_("Payload supports \\r, \\n, \\t, \\e, and \\xNN escapes.")); gtk_label_set_xalign(GTK_LABEL(hint), 0); append(quick, hint);
    GtkWidget *grid = tio_ui_quick_grid_new(FALSE); append(quick, grid);
    for (guint i = 0; i < 4; i++) {
        TioQuickFieldsUi fields = tio_ui_quick_fields_new(GTK_GRID(grid), i, config->quick_labels[i],
            config->quick_payloads[i], config->quick_modes[i], config->quick_endings[i]);
        tab->quick_text[i] = fields.payload; tab->quick_label[i] = fields.label;
        tab->quick_mode[i] = fields.mode; tab->quick_ending[i] = fields.ending;
        GtkButton *b = tab->quick.buttons[i]; g_object_set_data(G_OBJECT(b), "user-content", GINT_TO_POINTER(1)); gtk_button_set_label(b, config->quick_labels[i]);
        g_signal_connect(b, "clicked", G_CALLBACK(quick_clicked), tab); g_object_set_data(G_OBJECT(b), "index", GUINT_TO_POINTER(i));
    }
    GtkWidget *editor_actions = row(); gtk_widget_set_halign(editor_actions, GTK_ALIGN_END); append(quick, editor_actions);
    GtkWidget *cancel = gtk_button_new_with_label(_("Cancel")); append(editor_actions, cancel);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_widget_hide), tab->quick_window);
    button(editor_actions, _("Save"), G_CALLBACK(quick_edit_save), tab);
    g_signal_connect(tab->quick.customize, "clicked", G_CALLBACK(quick_edit), tab);
    GtkWidget *label = tio_ui_row(6); tab->title = GTK_LABEL(gtk_label_new(config->device && *config->device ? config->device : _("Serial"))); append(label, GTK_WIDGET(tab->title));
    gtk_label_set_ellipsize(tab->title, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_width_chars(tab->title, 16); gtk_label_set_max_width_chars(tab->title, 22);
    g_object_set_data(G_OBJECT(tab->title), "user-content", GINT_TO_POINTER(1));
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic"); gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    gtk_widget_set_tooltip_text(close, _("Close this session")); append(label, close);
    g_signal_connect(close, "clicked", G_CALLBACK(close_tab_clicked), tab);
    int page = gtk_notebook_append_page(app->notebook, root, label); gtk_notebook_set_current_page(app->notebook, page);
    GtkEventController *workspace_keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(workspace_keys, GTK_PHASE_CAPTURE);
    g_signal_connect(workspace_keys, "key-pressed", G_CALLBACK(workspace_key), tab); gtk_widget_add_controller(root, workspace_keys);
    GtkEventController *device_focus = gtk_event_controller_focus_new();
    g_signal_connect(device_focus, "leave", G_CALLBACK(manual_device_done), tab); gtk_widget_add_controller(GTK_WIDGET(tab->device), device_focus);
    g_signal_connect(tab->search_bar, "notify::search-mode-enabled", G_CALLBACK(search_mode_changed), tab);
    tab->timer = g_timeout_add(30, tick, tab); refresh_clicked(NULL, tab);
}
static void add_clicked(GtkButton *b, gpointer data) { (void)b; App *app = data; new_tab(app, &app->settings.defaults); }
static void quit(App *app)
{
    g_autoptr(GError) error = NULL;
    if (!save(app, &error)) {
        GtkAlertDialog *dialog = gtk_alert_dialog_new(_("Could not save settings: %s"), error->message); gtk_alert_dialog_show(dialog, app->window); g_object_unref(dialog); return;
    }
    app->closing = TRUE;
    g_clear_handle_id(&app->system_theme_timer, g_source_remove);
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
        GtkAlertDialog *dialog = gtk_alert_dialog_new(_("Disconnect all serial sessions and quit?"));
        const char *buttons[] = {_("Cancel"), _("Disconnect and quit"), NULL}; gtk_alert_dialog_set_buttons(dialog, buttons); gtk_alert_dialog_set_cancel_button(dialog, 0);
        gtk_alert_dialog_choose(dialog, app->window, NULL, quit_done, app); g_object_unref(dialog); return TRUE;
    }
    quit(app); return TRUE;
}
static void activate(GtkApplication *application, gpointer data)
{
    App *app = data; if (app->window) { gtk_window_present(app->window); return; }
    app->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(app->window, "tio-gui " TIO_GUI_VERSION " — Serial"); gtk_window_set_default_size(app->window, 1000, 660);
    g_signal_connect(app->window, "close-request", G_CALLBACK(close_requested), app);
#ifdef G_OS_WIN32
    if (native_font_path) {
        PangoFontMap *map = pango_context_get_font_map(gtk_widget_get_pango_context(GTK_WIDGET(app->window)));
        g_autoptr(GError) error = NULL;
        if (!pango_font_map_add_font_file(map, native_font_path, &error)) {
            g_warning("Could not load bundled font: %s", error->message);
        } else {
            /* Preserve the system face/size; explicitly append the private
             * family because DirectWrite's system fallback omits private fonts. */
            g_autofree gchar *name = NULL;
            g_object_get(gtk_settings_get_default(), "gtk-font-name", &name, NULL);
            PangoFontDescription *description = pango_font_description_from_string(name ? name : "Segoe UI 10");
            g_autofree gchar *families = g_strdup_printf("%s,Noto Sans SC", pango_font_description_get_family(description));
            pango_font_description_set_family(description, families);
            g_autofree gchar *fallback = pango_font_description_to_string(description);
            g_object_set(gtk_settings_get_default(), "gtk-font-name", fallback, NULL);
            pango_font_description_free(description);
        }
    }
#endif
    native_language(app->settings.language);
    native_theme(app);
    gtk_widget_add_css_class(GTK_WIDGET(app->window), "tio-workspace");
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_window_set_child(app->window, root);
    gtk_widget_add_css_class(root, "workspace-content"); gtk_widget_set_overflow(root, GTK_OVERFLOW_HIDDEN);
    GtkWidget *header = gtk_header_bar_new(); gtk_window_set_titlebar(app->window, header);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), gtk_label_new("tio-gui"));
    app->notebook = GTK_NOTEBOOK(gtk_notebook_new()); gtk_notebook_set_scrollable(app->notebook, TRUE);
    gtk_notebook_set_show_border(app->notebook, FALSE); gtk_widget_set_vexpand(GTK_WIDGET(app->notebook), TRUE); append(root, GTK_WIDGET(app->notebook));
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *settings = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(settings), "emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings, _("Application settings"));
    GtkWidget *popover = gtk_popover_new(); gtk_widget_set_halign(popover, GTK_ALIGN_START); gtk_popover_set_offset(GTK_POPOVER(popover), 12, 0); gtk_popover_set_child(GTK_POPOVER(popover), preferences(app));
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(settings), popover); append(actions, settings);
    GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic"); gtk_button_set_has_frame(GTK_BUTTON(add), FALSE);
    gtk_widget_set_tooltip_text(add, _("New session")); append(actions, add); g_signal_connect(add, "clicked", G_CALLBACK(add_clicked), app);
    gtk_notebook_set_action_widget(app->notebook, actions, GTK_PACK_START);
    tio_workspace_install_css();
    for (guint i = 0; i < app->settings.tab_configs->len; i++) new_tab(app, g_ptr_array_index(app->settings.tab_configs, i));
    if (!app->tabs->len) new_tab(app, &app->settings.defaults);
#ifdef G_OS_WIN32
    app->system_theme_timer = g_timeout_add_seconds(1, poll_system_theme, app);
#endif
    gtk_window_present(app->window);
}
int main(int argc, char **argv)
{
    if (argc == 2 && g_str_equal(argv[1], "--version")) { g_print("tio-gui %s (native serial)\n", TIO_GUI_VERSION); return 0; }
    bundled_runtime();
    App app = {0}; app.tabs = g_ptr_array_new(); tio_settings_init(&app.settings);
    replace(&app.settings.language, "zh_CN");
    app.settings_path = g_build_filename(g_get_user_config_dir(), "tio-gui", "serial.ini", NULL);
    if (g_file_test(app.settings_path, G_FILE_TEST_EXISTS)) {
        g_autoptr(GError) error = NULL;
        if (!tio_settings_load_from_file(&app.settings, app.settings_path, &error)) g_printerr("Settings: %s\n", error->message);
    }
    /* Earlier native builds saved an unused system language preference. */
    if (g_strcmp0(app.settings.language, "en")) replace(&app.settings.language, "zh_CN");
    native_language(app.settings.language);
    app.application = gtk_application_new("io.github.keithxc.tio_gui.serial", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
    int result = g_application_run(G_APPLICATION(app.application), argc, argv);
    g_object_unref(app.application); tio_settings_clear(&app.settings); g_ptr_array_unref(app.tabs); g_free(app.settings_path); g_free(native_font_path); return result;
}
