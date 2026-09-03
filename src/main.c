/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <glob.h>
#include <libintl.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

#include "settings.h"

#define _(message) gettext(message)

typedef struct {
    GtkWidget *window;
    GtkDropDown *device_dropdown;
    GtkDropDown *baud_dropdown;
    GtkEntry *custom_baud_entry;
    GtkDropDown *data_bits_dropdown;
    GtkDropDown *stop_bits_dropdown;
    GtkDropDown *parity_dropdown;
    GtkDropDown *flow_dropdown;
    GtkDropDown *language_dropdown;
    GtkLabel *device_label;
    GtkLabel *baud_label;
    GtkLabel *data_bits_label;
    GtkLabel *stop_bits_label;
    GtkLabel *parity_label;
    GtkLabel *flow_label;
    GtkLabel *language_label;
    GtkLabel *more_settings_label;
    GtkWidget *refresh_button;
    GtkExpander *advanced_expander;
    GtkCheckButton *local_echo_check;
    GtkCheckButton *show_all_ttys_check;
    GtkCheckButton *timestamp_check;
    GtkCheckButton *log_check;
    GtkEntry *log_directory_entry;
    GtkButton *connect_button;
    GtkLabel *status_label;
    VteTerminal *terminal;
    GtkEntry *send_entry;
    GtkButton *send_button;
    GtkButton *clear_terminal_button;
    GtkButton *customize_quick_buttons;
    GtkButton *quick_buttons[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkToggleButton *hex_toggle;
    GPid child_pid;
    gchar **spawn_argv;
    TioSettings settings;
} TioGui;

typedef struct {
    TioGui *gui;
    GtkWidget *window;
    GtkEntry *label_entries[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkEntry *payload_entries[TIO_GUI_QUICK_BUTTON_COUNT];
} QuickButtonEditor;

static void update_quick_buttons(TioGui *gui);
static GtkWidget *make_label(const char *text);
static void set_status(TioGui *gui, const char *message);
static void on_language_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data);

static const char *const common_device_patterns[] = {
    "/dev/ttyUSB*",
    "/dev/ttyACM*",
    NULL,
};

static const char *const all_device_patterns[] = {
    "/dev/tty*",
    NULL,
};

static const char *const baud_rates[] = {
    "9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600",
    "1500000", NULL,
};

#define TIO_GUI_CUSTOM_BAUD_INDEX (G_N_ELEMENTS(baud_rates) - 1)

static const char *const data_bits_values[] = {"5", "6", "7", "8", NULL};
static const char *const stop_bits_values[] = {"1", "2", NULL};
static const char *const parity_values[] = {"none", "even", "odd", "mark", "space", NULL};
static const char *const flow_values[] = {"none", "hard", "soft", NULL};
static const char *const language_values[] = {"system", "zh_CN", "zh_TW", "en", "ja", "de", NULL};

static guint language_index(const char *language)
{
    for (guint index = 0; language_values[index] != NULL; ++index) {
        if (g_strcmp0(language_values[index], language) == 0) {
            return index;
        }
    }
    return 0;
}

static gboolean apply_language(const char *language)
{
    g_unsetenv("LANGUAGE");
    if (g_strcmp0(language, "system") == 0) {
        return setlocale(LC_MESSAGES, "") != NULL;
    }

    /* GNU gettext uses LANGUAGE for catalogue selection, but ignores it in
       the C locale. Toggling through C also invalidates its public locale
       state so an already-open window can be translated again safely. */
    if (setlocale(LC_MESSAGES, "C") == NULL) {
        return FALSE;
    }
    g_setenv("LANGUAGE", language, TRUE);
    return setlocale(LC_MESSAGES, "en_US.UTF-8") != NULL;
}

static void retranslate_ui(TioGui *gui)
{
    gtk_label_set_text(gui->device_label, _("Device"));
    gtk_label_set_text(gui->baud_label, _("Baud"));
    GListModel *baud_model = gtk_drop_down_get_model(gui->baud_dropdown);
    if (GTK_IS_STRING_LIST(baud_model)) {
        const char *custom_label[] = {_("Custom…"), NULL};
        gtk_string_list_splice(GTK_STRING_LIST(baud_model),
                               TIO_GUI_CUSTOM_BAUD_INDEX,
                               1,
                               custom_label);
    }
    gtk_entry_set_placeholder_text(gui->custom_baud_entry, _("Custom baud"));
    gtk_widget_set_tooltip_text(gui->refresh_button, _("Refresh serial devices"));
    gtk_button_set_label(gui->connect_button,
                         gui->child_pid > 0 ? _("Disconnect") : _("Connect"));
    gtk_check_button_set_label(gui->timestamp_check, _("Timestamps"));
    gtk_check_button_set_label(gui->log_check, _("Log session"));
    gtk_entry_set_placeholder_text(gui->log_directory_entry, _("Log directory"));
    gtk_label_set_text(gui->more_settings_label, _("More settings"));
    gtk_label_set_text(gui->data_bits_label, _("Data bits"));
    gtk_label_set_text(gui->stop_bits_label, _("Stop bits"));
    gtk_label_set_text(gui->parity_label, _("Parity"));
    gtk_label_set_text(gui->flow_label, _("Flow control"));
    gtk_check_button_set_label(gui->local_echo_check, _("Local echo"));
    gtk_check_button_set_label(gui->show_all_ttys_check, _("Show all TTY devices"));
    gtk_label_set_text(gui->language_label, _("Language"));

    set_status(gui, gui->child_pid > 0 ? _("Connected") : _("Ready"));
    gtk_button_set_label(gui->clear_terminal_button, _("Clear"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->clear_terminal_button),
                                _("Clear the terminal and its scrollback history"));
    gtk_entry_set_placeholder_text(gui->send_entry, _("Send text…"));
    gtk_button_set_label(gui->send_button, _("Send"));
    gtk_button_set_label(gui->customize_quick_buttons, _("Customize…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->hex_toggle),
                                _("Display incoming bytes as 16-byte hex rows"));
}

static void on_language_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    TioGui *gui = user_data;
    guint selected = gtk_drop_down_get_selected(dropdown);
    if (selected >= G_N_ELEMENTS(language_values) - 1 ||
        g_strcmp0(gui->settings.language, language_values[selected]) == 0) {
        return;
    }

    g_free(gui->settings.language);
    gui->settings.language = g_strdup(language_values[selected]);
    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&gui->settings, &error)) {
        g_warning("Could not save language setting: %s", error->message);
        return;
    }

    g_unsetenv("TIO_GUI_LANGUAGE");
    if (!apply_language(language_values[selected])) {
        g_warning("Locale is unavailable for language: %s", language_values[selected]);
        return;
    }
    retranslate_ui(gui);
}

static void set_status(TioGui *gui, const char *message)
{
    gtk_label_set_text(gui->status_label, message);
}

static guint device_priority(const char *path)
{
    if (g_str_has_prefix(path, "/dev/ttyACM")) {
        return 0;
    }
    if (g_str_has_prefix(path, "/dev/ttyUSB")) {
        return 1;
    }
    return 2;
}

static gint compare_devices(gconstpointer left, gconstpointer right)
{
    const char *const *left_string = left;
    const char *const *right_string = right;
    guint left_priority = device_priority(*left_string);
    guint right_priority = device_priority(*right_string);
    if (left_priority != right_priority) {
        return left_priority < right_priority ? -1 : 1;
    }

    g_autofree gchar *left_key = g_utf8_collate_key_for_filename(*left_string, -1);
    g_autofree gchar *right_key = g_utf8_collate_key_for_filename(*right_string, -1);
    return strcmp(left_key, right_key);
}

static void refresh_devices(TioGui *gui)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *devices = g_ptr_array_new_with_free_func(g_free);

    const char *const *patterns =
        gtk_check_button_get_active(gui->show_all_ttys_check)
            ? all_device_patterns
            : common_device_patterns;

    for (size_t pattern_index = 0; patterns[pattern_index] != NULL; ++pattern_index) {
        glob_t matches = {0};
        if (glob(patterns[pattern_index], GLOB_NOSORT, NULL, &matches) == 0) {
            for (size_t match_index = 0; match_index < matches.gl_pathc; ++match_index) {
                const char *path = matches.gl_pathv[match_index];
                if (!g_hash_table_contains(seen, path)) {
                    g_hash_table_add(seen, g_strdup(path));
                    g_ptr_array_add(devices, g_strdup(path));
                }
            }
        }
        globfree(&matches);
    }

    g_ptr_array_sort(devices, compare_devices);

    GtkStringList *model = gtk_string_list_new(NULL);
    for (guint index = 0; index < devices->len; ++index) {
        gtk_string_list_append(model, g_ptr_array_index(devices, index));
    }
    gtk_drop_down_set_model(gui->device_dropdown, G_LIST_MODEL(model));
    g_object_unref(model);

    if (devices->len > 0) {
        guint selected = 0;
        for (guint index = 0; index < devices->len; ++index) {
            if (g_strcmp0(g_ptr_array_index(devices, index), gui->settings.device) == 0) {
                selected = index;
                break;
            }
        }
        gtk_drop_down_set_selected(gui->device_dropdown, selected);
        const char *format = ngettext("Found %u serial device",
                                     "Found %u serial devices",
                                     devices->len);
        g_autofree gchar *message = g_strdup_printf(format, devices->len);
        set_status(gui, message);
    } else {
        set_status(gui, _("No serial devices found"));
    }

    g_ptr_array_unref(devices);
    g_hash_table_unref(seen);
}

static const char *selected_string(GtkDropDown *dropdown)
{
    GtkStringObject *item = GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(dropdown));
    return item == NULL ? NULL : gtk_string_object_get_string(item);
}

static gboolean baud_is_valid(const char *baud)
{
    if (baud == NULL || baud[0] == '\0') {
        return FALSE;
    }
    for (const char *character = baud; *character != '\0'; ++character) {
        if (!g_ascii_isdigit(*character)) {
            return FALSE;
        }
    }

    errno = 0;
    guint64 value = g_ascii_strtoull(baud, NULL, 10);
    return errno == 0 && value > 0 && value <= G_MAXUINT32;
}

static const char *selected_baud(TioGui *gui)
{
    if (gtk_drop_down_get_selected(gui->baud_dropdown) == TIO_GUI_CUSTOM_BAUD_INDEX) {
        return gtk_editable_get_text(GTK_EDITABLE(gui->custom_baud_entry));
    }
    return selected_string(gui->baud_dropdown);
}

static void on_baud_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    TioGui *gui = user_data;
    gboolean custom = gtk_drop_down_get_selected(dropdown) == TIO_GUI_CUSTOM_BAUD_INDEX;
    gtk_widget_set_visible(GTK_WIDGET(gui->custom_baud_entry), custom);
    if (custom) {
        gtk_widget_grab_focus(GTK_WIDGET(gui->custom_baud_entry));
    }
}

static void select_string(GtkDropDown *dropdown, const char *wanted)
{
    GListModel *model = gtk_drop_down_get_model(dropdown);
    if (model == NULL || wanted == NULL) {
        return;
    }

    guint count = g_list_model_get_n_items(model);
    for (guint index = 0; index < count; ++index) {
        g_autoptr(GtkStringObject) item = g_list_model_get_item(model, index);
        if (g_strcmp0(gtk_string_object_get_string(item), wanted) == 0) {
            gtk_drop_down_set_selected(dropdown, index);
            return;
        }
    }
}

static void on_child_exited(VteTerminal *terminal, gint status, gpointer user_data)
{
    (void)terminal;
    TioGui *gui = user_data;

    gui->child_pid = -1;
    gtk_widget_set_sensitive(GTK_WIDGET(gui->connect_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_entry), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_button), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->hex_toggle), TRUE);
    update_quick_buttons(gui);
    gtk_button_set_label(gui->connect_button, _("Connect"));

    g_autofree gchar *message =
        g_strdup_printf(_("Disconnected (tio exit status: %d)"), status);
    set_status(gui, message);
}

static void on_spawn_finished(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
    (void)terminal;
    TioGui *gui = user_data;

    g_clear_pointer(&gui->spawn_argv, g_strfreev);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->connect_button), TRUE);

    if (error != NULL) {
        g_autofree gchar *message =
            g_strdup_printf(_("Could not start tio: %s"), error->message);
        set_status(gui, message);
        gui->child_pid = -1;
        gtk_button_set_label(gui->connect_button, _("Connect"));
        return;
    }

    gui->child_pid = pid;
    gtk_button_set_label(gui->connect_button, _("Disconnect"));
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->hex_toggle), FALSE);
    update_quick_buttons(gui);
    set_status(gui, _("Connected"));
    gtk_widget_grab_focus(GTK_WIDGET(gui->terminal));
}

static gchar **build_tio_argv(TioGui *gui, const char *device, const char *baud)
{
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);
    const char *data_bits = selected_string(gui->data_bits_dropdown);
    const char *stop_bits = selected_string(gui->stop_bits_dropdown);
    const char *parity = selected_string(gui->parity_dropdown);
    const char *flow = selected_string(gui->flow_dropdown);

    g_ptr_array_add(arguments, g_strdup("tio"));
    g_ptr_array_add(arguments, g_strdup("--baudrate"));
    g_ptr_array_add(arguments, g_strdup(baud));
    g_ptr_array_add(arguments, g_strdup("--databits"));
    g_ptr_array_add(arguments, g_strdup(data_bits));
    g_ptr_array_add(arguments, g_strdup("--flow"));
    g_ptr_array_add(arguments, g_strdup(flow));
    g_ptr_array_add(arguments, g_strdup("--stopbits"));
    g_ptr_array_add(arguments, g_strdup(stop_bits));
    g_ptr_array_add(arguments, g_strdup("--parity"));
    g_ptr_array_add(arguments, g_strdup(parity));

    if (gtk_check_button_get_active(gui->local_echo_check)) {
        g_ptr_array_add(arguments, g_strdup("--local-echo"));
    }
    if (gtk_toggle_button_get_active(gui->hex_toggle)) {
        g_ptr_array_add(arguments, g_strdup("--output-mode"));
        g_ptr_array_add(arguments, g_strdup("hex16"));
    }

    if (gtk_check_button_get_active(gui->timestamp_check)) {
        g_ptr_array_add(arguments, g_strdup("--timestamp"));
        g_ptr_array_add(arguments, g_strdup("--timestamp-format"));
        g_ptr_array_add(arguments, g_strdup("iso8601"));
    }
    if (gtk_check_button_get_active(gui->log_check)) {
        g_ptr_array_add(arguments, g_strdup("--log"));
        g_ptr_array_add(arguments, g_strdup("--log-directory"));
        g_ptr_array_add(arguments,
                        g_strdup(gtk_editable_get_text(GTK_EDITABLE(gui->log_directory_entry))));
    }

    g_ptr_array_add(arguments, g_strdup(device));
    g_ptr_array_add(arguments, NULL);
    return (gchar **)g_ptr_array_free(arguments, FALSE);
}

static void disconnect_tio(TioGui *gui)
{
    if (gui->child_pid <= 0) {
        return;
    }

    if (kill(gui->child_pid, SIGHUP) == -1 && errno != ESRCH) {
        g_autofree gchar *message =
            g_strdup_printf(_("Could not stop tio: %s"), g_strerror(errno));
        set_status(gui, message);
        return;
    }

    set_status(gui, _("Disconnecting…"));
}

static void connect_tio(TioGui *gui)
{
    const char *device = selected_string(gui->device_dropdown);
    const char *baud = selected_baud(gui);

    if (device == NULL || baud == NULL) {
        set_status(gui, _("Select a serial device and baud rate first"));
        return;
    }
    if (!baud_is_valid(baud)) {
        set_status(gui, _("Enter a valid baud rate from 1 to 4294967295"));
        gtk_widget_grab_focus(GTK_WIDGET(gui->custom_baud_entry));
        return;
    }

    g_autofree gchar *tio_path = g_find_program_in_path("tio");
    if (tio_path == NULL) {
        set_status(gui, _("tio was not found in PATH"));
        return;
    }

    if (gtk_check_button_get_active(gui->log_check)) {
        const char *log_directory =
            gtk_editable_get_text(GTK_EDITABLE(gui->log_directory_entry));
        if (log_directory[0] == '\0') {
            set_status(gui, _("Choose a log directory first"));
            return;
        }
        if (g_mkdir_with_parents(log_directory, 0750) == -1) {
            g_autofree gchar *message =
                g_strdup_printf(_("Could not create log directory: %s"), g_strerror(errno));
            set_status(gui, message);
            return;
        }
    }

    vte_terminal_reset(gui->terminal, TRUE, TRUE);
    gui->spawn_argv = build_tio_argv(gui, device, baud);
    set_status(gui, _("Connecting…"));
    gtk_widget_set_sensitive(GTK_WIDGET(gui->connect_button), FALSE);

    vte_terminal_spawn_async(gui->terminal,
                             VTE_PTY_DEFAULT,
                             NULL,
                             gui->spawn_argv,
                             NULL,
                             G_SPAWN_SEARCH_PATH,
                             NULL,
                             NULL,
                             NULL,
                             -1,
                             NULL,
                             on_spawn_finished,
                             gui);

}

static void on_connect_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;

    if (gui->child_pid > 0) {
        disconnect_tio(gui);
    } else {
        connect_tio(gui);
    }
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    refresh_devices(user_data);
}

static void on_clear_terminal_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;

    vte_terminal_reset(gui->terminal, TRUE, TRUE);
}

static void on_show_all_ttys_toggled(GtkCheckButton *button, gpointer user_data)
{
    (void)button;
    refresh_devices(user_data);
}

static void send_entry_contents(TioGui *gui)
{
    if (gui->child_pid <= 0) {
        return;
    }

    const char *text = gtk_editable_get_text(GTK_EDITABLE(gui->send_entry));
    if (text[0] == '\0') {
        return;
    }

    vte_terminal_feed_child(gui->terminal, text, -1);
    vte_terminal_feed_child(gui->terminal, "\r", 1);
    gtk_editable_set_text(GTK_EDITABLE(gui->send_entry), "");
}

static void on_send_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    send_entry_contents(user_data);
}

static void on_send_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    send_entry_contents(user_data);
}

static GByteArray *decode_quick_payload(const char *payload)
{
    GByteArray *bytes = g_byte_array_new();

    for (gsize index = 0; payload[index] != '\0'; ++index) {
        guint8 value = (guint8)payload[index];
        if (payload[index] == '\\' && payload[index + 1] != '\0') {
            char escaped = payload[++index];
            switch (escaped) {
            case 'r': value = '\r'; break;
            case 'n': value = '\n'; break;
            case 't': value = '\t'; break;
            case 'e': value = 0x1b; break;
            case '\\': value = '\\'; break;
            case 'x':
                if (g_ascii_isxdigit(payload[index + 1]) &&
                    g_ascii_isxdigit(payload[index + 2])) {
                    value = (guint8)((g_ascii_xdigit_value(payload[index + 1]) << 4) |
                                     g_ascii_xdigit_value(payload[index + 2]));
                    index += 2;
                } else {
                    value = (guint8)'x';
                }
                break;
            default: value = (guint8)escaped; break;
            }
        }
        g_byte_array_append(bytes, &value, 1);
    }
    return bytes;
}

static void on_quick_button_clicked(GtkButton *button, gpointer user_data)
{
    TioGui *gui = user_data;
    guint stored_index = GPOINTER_TO_UINT(
        g_object_get_data(G_OBJECT(button), "quick-button-index"));
    if (gui->child_pid <= 0 || stored_index == 0) {
        return;
    }

    guint index = stored_index - 1;
    g_autoptr(GByteArray) bytes = decode_quick_payload(gui->settings.quick_payloads[index]);
    if (bytes->len > 0) {
        vte_terminal_feed_child_binary(gui->terminal, bytes->data, bytes->len);
    }
}

static void update_quick_buttons(TioGui *gui)
{
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        GtkButton *button = gui->quick_buttons[index];
        if (button == NULL) {
            continue;
        }
        gtk_button_set_label(button, gui->settings.quick_labels[index]);
        gtk_widget_set_tooltip_text(GTK_WIDGET(button), gui->settings.quick_payloads[index]);
        gtk_widget_set_sensitive(GTK_WIDGET(button),
                                 gui->child_pid > 0 &&
                                     gui->settings.quick_payloads[index][0] != '\0');
    }
}

static void on_quick_editor_save(GtkButton *button, gpointer user_data)
{
    (void)button;
    QuickButtonEditor *editor = user_data;

    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_free(editor->gui->settings.quick_labels[index]);
        editor->gui->settings.quick_labels[index] =
            g_strdup(gtk_editable_get_text(GTK_EDITABLE(editor->label_entries[index])));
        g_free(editor->gui->settings.quick_payloads[index]);
        editor->gui->settings.quick_payloads[index] =
            g_strdup(gtk_editable_get_text(GTK_EDITABLE(editor->payload_entries[index])));
    }
    update_quick_buttons(editor->gui);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&editor->gui->settings, &error)) {
        g_warning("Could not save quick buttons: %s", error->message);
    }
    gtk_window_destroy(GTK_WINDOW(editor->window));
}

static void on_quick_editor_cancel(GtkButton *button, gpointer user_data)
{
    (void)button;
    QuickButtonEditor *editor = user_data;
    gtk_window_destroy(GTK_WINDOW(editor->window));
}

static void on_customize_quick_buttons(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    QuickButtonEditor *editor = g_new0(QuickButtonEditor, 1);
    editor->gui = gui;
    editor->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(editor->window), _("Customize quick buttons"));
    gtk_window_set_transient_for(GTK_WINDOW(editor->window), GTK_WINDOW(gui->window));
    gtk_window_set_modal(GTK_WINDOW(editor->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(editor->window), 560, 280);
    g_object_set_data_full(G_OBJECT(editor->window), "quick-editor", editor, g_free);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(editor->window), root);

    GtkWidget *hint = gtk_label_new(_("Payload supports \\r, \\n, \\t, \\e, and \\xNN escapes."));
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0F);
    gtk_box_append(GTK_BOX(root), hint);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_vexpand(grid, TRUE);
    gtk_box_append(GTK_BOX(root), grid);
    gtk_grid_attach(GTK_GRID(grid), make_label(_("Button")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label(_("Label")), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label(_("Payload")), 2, 0, 1, 1);

    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_autofree gchar *number = g_strdup_printf("%u", index + 1);
        gtk_grid_attach(GTK_GRID(grid), make_label(number), 0, (gint)index + 1, 1, 1);

        editor->label_entries[index] = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_text(GTK_EDITABLE(editor->label_entries[index]),
                              gui->settings.quick_labels[index]);
        gtk_grid_attach(GTK_GRID(grid),
                        GTK_WIDGET(editor->label_entries[index]),
                        1, (gint)index + 1, 1, 1);

        editor->payload_entries[index] = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_text(GTK_EDITABLE(editor->payload_entries[index]),
                              gui->settings.quick_payloads[index]);
        gtk_widget_set_hexpand(GTK_WIDGET(editor->payload_entries[index]), TRUE);
        gtk_grid_attach(GTK_GRID(grid),
                        GTK_WIDGET(editor->payload_entries[index]),
                        2, (gint)index + 1, 1, 1);
    }

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label(_("Cancel"));
    GtkWidget *save = gtk_button_new_with_label(_("Save"));
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), save);
    gtk_box_append(GTK_BOX(root), actions);

    g_signal_connect(cancel, "clicked", G_CALLBACK(on_quick_editor_cancel), editor);
    g_signal_connect(save, "clicked", G_CALLBACK(on_quick_editor_save), editor);
    gtk_window_present(GTK_WINDOW(editor->window));
}

static void on_log_toggled(GtkCheckButton *button, gpointer user_data)
{
    TioGui *gui = user_data;
    gboolean enabled = gtk_check_button_get_active(button);
    gtk_widget_set_visible(GTK_WIDGET(gui->log_directory_entry), enabled);
    if (enabled) {
        gtk_check_button_set_active(gui->timestamp_check, TRUE);
    }
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data)
{
    (void)window;
    TioGui *gui = user_data;

    if (gui->child_pid > 0) {
        (void)kill(gui->child_pid, SIGHUP);
    }

    const char *device = selected_string(gui->device_dropdown);
    const char *baud = selected_baud(gui);
    const char *data_bits = selected_string(gui->data_bits_dropdown);
    const char *stop_bits = selected_string(gui->stop_bits_dropdown);
    const char *parity = selected_string(gui->parity_dropdown);
    const char *flow = selected_string(gui->flow_dropdown);

    g_free(gui->settings.device);
    gui->settings.device = g_strdup(device);
    if (baud_is_valid(baud)) {
        g_free(gui->settings.baud);
        gui->settings.baud = g_strdup(baud);
    }
    g_free(gui->settings.data_bits);
    gui->settings.data_bits = g_strdup(data_bits);
    g_free(gui->settings.stop_bits);
    gui->settings.stop_bits = g_strdup(stop_bits);
    g_free(gui->settings.parity);
    gui->settings.parity = g_strdup(parity);
    g_free(gui->settings.flow);
    gui->settings.flow = g_strdup(flow);
    g_free(gui->settings.log_directory);
    gui->settings.log_directory =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(gui->log_directory_entry)));
    gui->settings.local_echo = gtk_check_button_get_active(gui->local_echo_check);
    gui->settings.show_all_ttys =
        gtk_check_button_get_active(gui->show_all_ttys_check);
    gui->settings.timestamps = gtk_check_button_get_active(gui->timestamp_check);
    gui->settings.logging = gtk_check_button_get_active(gui->log_check);
    gui->settings.hex_output = gtk_toggle_button_get_active(gui->hex_toggle);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&gui->settings, &error)) {
        g_warning("Could not save settings: %s", error->message);
    }
    g_clear_pointer(&gui->spawn_argv, g_strfreev);
    return FALSE;
}

static GtkWidget *make_label(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

static GtkDropDown *make_string_dropdown(const char *const *values, guint selected)
{
    GtkStringList *model = gtk_string_list_new(values);
    GtkDropDown *dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(model), NULL));
    gtk_drop_down_set_selected(dropdown, selected);
    return dropdown;
}

static void install_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        provider,
        ".compact-controls button, .compact-controls entry {"
        "  min-height: 26px;"
        "  padding-top: 2px;"
        "  padding-bottom: 2px;"
        "}"
        ".compact-controls dropdown button {"
        "  min-height: 26px;"
        "  padding-top: 2px;"
        "  padding-bottom: 2px;"
        "}"
        ".terminal-frame {"
        "  border: 1px solid alpha(@theme_fg_color, 0.22);"
        "  border-radius: 6px;"
        "  padding: 1px;"
        "  background-color: black;"
        "}"
        ".session-status { font-size: 0.9em; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;

    TioGui *gui = g_new0(TioGui, 1);
    gui->child_pid = -1;
    tio_settings_init(&gui->settings);
    tio_settings_load(&gui->settings);
    const char *development_language = g_getenv("TIO_GUI_LANGUAGE");
    if (development_language != NULL && development_language[0] != '\0') {
        g_free(gui->settings.language);
        gui->settings.language = g_strdup(development_language);
    }

    install_css();

    gui->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(gui->window),
                         g_getenv("TIO_GUI_NON_UNIQUE") != NULL
                             ? "tio-gui — Preview"
                             : "tio-gui");
    gtk_window_set_default_size(GTK_WINDOW(gui->window), 960, 620);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 8);
    gtk_widget_set_margin_start(root, 8);
    gtk_widget_set_margin_end(root, 8);
    gtk_window_set_child(GTK_WINDOW(gui->window), root);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(toolbar, "compact-controls");
    gtk_box_append(GTK_BOX(root), toolbar);

    gui->device_label = GTK_LABEL(make_label(_("Device")));
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->device_label));
    gui->device_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->device_dropdown), TRUE);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->device_dropdown));

    gui->refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(gui->refresh_button, _("Refresh serial devices"));
    gtk_box_append(GTK_BOX(toolbar), gui->refresh_button);

    gui->baud_label = GTK_LABEL(make_label(_("Baud")));
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->baud_label));
    GtkStringList *baud_model = gtk_string_list_new(baud_rates);
    gtk_string_list_append(baud_model, _("Custom…"));
    gui->baud_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(baud_model), NULL));
    gboolean preset_baud = FALSE;
    for (guint index = 0; baud_rates[index] != NULL; ++index) {
        if (g_strcmp0(baud_rates[index], gui->settings.baud) == 0) {
            gtk_drop_down_set_selected(gui->baud_dropdown, index);
            preset_baud = TRUE;
            break;
        }
    }
    if (!preset_baud) {
        gtk_drop_down_set_selected(gui->baud_dropdown, TIO_GUI_CUSTOM_BAUD_INDEX);
    }
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->baud_dropdown));
    gui->custom_baud_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(gui->custom_baud_entry, _("Custom baud"));
    gtk_entry_set_input_purpose(gui->custom_baud_entry, GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_max_length(gui->custom_baud_entry, 10);
    gtk_editable_set_width_chars(GTK_EDITABLE(gui->custom_baud_entry), 10);
    gtk_editable_set_text(GTK_EDITABLE(gui->custom_baud_entry), gui->settings.baud);
    gtk_widget_set_visible(GTK_WIDGET(gui->custom_baud_entry), !preset_baud);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->custom_baud_entry));

    gui->connect_button = GTK_BUTTON(gtk_button_new_with_label(_("Connect")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->connect_button), "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->connect_button));

    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(options, "compact-controls");

    gui->more_settings_label = GTK_LABEL(make_label(_("More settings")));
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(gui->more_settings_label));

    gui->timestamp_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Timestamps")));
    gtk_check_button_set_active(gui->timestamp_check, gui->settings.timestamps);
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(gui->timestamp_check));

    gui->log_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Log session")));
    gtk_check_button_set_active(gui->log_check, gui->settings.logging);
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(gui->log_check));

    gui->log_directory_entry = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(gui->log_directory_entry),
                          gui->settings.log_directory);
    gtk_entry_set_placeholder_text(gui->log_directory_entry, _("Log directory"));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->log_directory_entry), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(gui->log_directory_entry), gui->settings.logging);
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(gui->log_directory_entry));

    gui->advanced_expander = GTK_EXPANDER(gtk_expander_new(NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->advanced_expander), TRUE);
    gtk_expander_set_label_widget(gui->advanced_expander, options);
    GtkWidget *advanced_grid = gtk_grid_new();
    gtk_widget_add_css_class(advanced_grid, "compact-controls");
    gtk_grid_set_column_spacing(GTK_GRID(advanced_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(advanced_grid), 6);
    gtk_widget_set_margin_top(advanced_grid, 4);
    gtk_widget_set_margin_start(advanced_grid, 18);

    gui->data_bits_dropdown = make_string_dropdown(data_bits_values, 3);
    gui->stop_bits_dropdown = make_string_dropdown(stop_bits_values, 0);
    gui->parity_dropdown = make_string_dropdown(parity_values, 0);
    gui->flow_dropdown = make_string_dropdown(flow_values, 0);
    const char *language_names[] = {
        _("System default"), "简体中文", "繁體中文", "English", "日本語", "Deutsch", NULL,
    };
    gui->language_dropdown = make_string_dropdown(language_names, 0);
    gui->local_echo_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Local echo")));
    gui->show_all_ttys_check =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Show all TTY devices")));
    select_string(gui->data_bits_dropdown, gui->settings.data_bits);
    select_string(gui->stop_bits_dropdown, gui->settings.stop_bits);
    select_string(gui->parity_dropdown, gui->settings.parity);
    select_string(gui->flow_dropdown, gui->settings.flow);
    gtk_drop_down_set_selected(gui->language_dropdown, language_index(gui->settings.language));
    gtk_check_button_set_active(gui->local_echo_check, gui->settings.local_echo);
    gtk_check_button_set_active(gui->show_all_ttys_check, gui->settings.show_all_ttys);

    gui->data_bits_label = GTK_LABEL(make_label(_("Data bits")));
    gui->stop_bits_label = GTK_LABEL(make_label(_("Stop bits")));
    gui->parity_label = GTK_LABEL(make_label(_("Parity")));
    gui->flow_label = GTK_LABEL(make_label(_("Flow control")));
    gui->language_label = GTK_LABEL(make_label(_("Language")));
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->data_bits_label), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->data_bits_dropdown), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->stop_bits_label), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->stop_bits_dropdown), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->parity_label), 4, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->parity_dropdown), 5, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->flow_label), 6, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->flow_dropdown), 7, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->local_echo_check), 8, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->show_all_ttys_check), 0, 1, 4, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->language_label), 4, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced_grid), GTK_WIDGET(gui->language_dropdown), 5, 1, 4, 1);

    gtk_expander_set_child(gui->advanced_expander, advanced_grid);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->advanced_expander));
    gtk_box_reorder_child_after(GTK_BOX(root), GTK_WIDGET(gui->advanced_expander), toolbar);

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(status_bar, "compact-controls");
    gui->status_label = GTK_LABEL(gtk_label_new(_("Ready")));
    gtk_label_set_xalign(gui->status_label, 0.0F);
    gtk_widget_set_hexpand(GTK_WIDGET(gui->status_label), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(gui->status_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(gui->status_label), "session-status");
    gtk_box_append(GTK_BOX(status_bar), GTK_WIDGET(gui->status_label));
    gui->clear_terminal_button = GTK_BUTTON(gtk_button_new_with_label(_("Clear")));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->clear_terminal_button),
                                _("Clear the terminal and its scrollback history"));
    gtk_box_append(GTK_BOX(status_bar), GTK_WIDGET(gui->clear_terminal_button));
    gtk_box_append(GTK_BOX(root), status_bar);

    gui->terminal = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scrollback_lines(gui->terminal, 10000);
    vte_terminal_set_mouse_autohide(gui->terminal, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(gui->terminal), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(gui->terminal), TRUE);

    GtkWidget *terminal_frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(terminal_frame, "terminal-frame");
    GtkWidget *terminal_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(terminal_scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(terminal_scroll),
                                  GTK_WIDGET(gui->terminal));
    gtk_frame_set_child(GTK_FRAME(terminal_frame), terminal_scroll);
    gtk_widget_set_hexpand(terminal_frame, TRUE);
    gtk_widget_set_vexpand(terminal_frame, TRUE);
    gtk_box_append(GTK_BOX(root), terminal_frame);

    GtkWidget *send_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(send_bar, "compact-controls");
    gui->send_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(gui->send_entry, _("Send text…"));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->send_entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_entry), FALSE);
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(gui->send_entry));
    gui->send_button = GTK_BUTTON(gtk_button_new_with_label(_("Send")));
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_button), FALSE);
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(gui->send_button));
    gtk_box_append(GTK_BOX(root), send_bar);

    GtkWidget *quick_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(quick_bar, "compact-controls");
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        gui->quick_buttons[index] = GTK_BUTTON(
            gtk_button_new_with_label(gui->settings.quick_labels[index]));
        g_object_set_data(G_OBJECT(gui->quick_buttons[index]),
                          "quick-button-index",
                          GUINT_TO_POINTER(index + 1));
        gtk_widget_set_sensitive(GTK_WIDGET(gui->quick_buttons[index]), FALSE);
        gtk_box_append(GTK_BOX(quick_bar), GTK_WIDGET(gui->quick_buttons[index]));
        g_signal_connect(gui->quick_buttons[index],
                         "clicked",
                         G_CALLBACK(on_quick_button_clicked),
                         gui);
    }
    gui->customize_quick_buttons =
        GTK_BUTTON(gtk_button_new_with_label(_("Customize…")));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->customize_quick_buttons), TRUE);
    gtk_widget_set_halign(GTK_WIDGET(gui->customize_quick_buttons), GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(quick_bar), GTK_WIDGET(gui->customize_quick_buttons));
    gui->hex_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("HEX"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->hex_toggle),
                                _("Display incoming bytes as 16-byte hex rows"));
    gtk_toggle_button_set_active(gui->hex_toggle, gui->settings.hex_output);
    gtk_box_append(GTK_BOX(quick_bar), GTK_WIDGET(gui->hex_toggle));
    gtk_box_append(GTK_BOX(root), quick_bar);

    g_signal_connect(gui->refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), gui);
    g_signal_connect(gui->baud_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_baud_changed),
                     gui);
    g_signal_connect(gui->clear_terminal_button,
                     "clicked",
                     G_CALLBACK(on_clear_terminal_clicked),
                     gui);
    g_signal_connect(gui->connect_button, "clicked", G_CALLBACK(on_connect_clicked), gui);
    g_signal_connect(gui->log_check, "toggled", G_CALLBACK(on_log_toggled), gui);
    g_signal_connect(gui->show_all_ttys_check,
                     "toggled",
                     G_CALLBACK(on_show_all_ttys_toggled),
                     gui);
    g_signal_connect(gui->language_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_language_changed),
                     gui);
    g_signal_connect(gui->terminal, "child-exited", G_CALLBACK(on_child_exited), gui);
    g_signal_connect(gui->send_button, "clicked", G_CALLBACK(on_send_clicked), gui);
    g_signal_connect(gui->send_entry, "activate", G_CALLBACK(on_send_activate), gui);
    g_signal_connect(gui->customize_quick_buttons,
                     "clicked",
                     G_CALLBACK(on_customize_quick_buttons),
                     gui);
    g_signal_connect(gui->window,
                     "close-request",
                     G_CALLBACK(on_window_close_request),
                     gui);

    refresh_devices(gui);
    gtk_window_present(GTK_WINDOW(gui->window));
}

int main(int argc, char **argv)
{
    if (argc == 2 && g_str_equal(argv[1], "--version")) {
        printf("tio-gui %s\n", TIO_GUI_VERSION);
        return 0;
    }

    const char *development_language = g_getenv("TIO_GUI_LANGUAGE");
    TioSettings startup_settings;
    tio_settings_init(&startup_settings);
    tio_settings_load(&startup_settings);
    g_autofree gchar *startup_language = g_strdup(
        development_language != NULL && development_language[0] != '\0'
            ? development_language : startup_settings.language);
    setlocale(LC_ALL, "");
    tio_settings_clear(&startup_settings);
    const char *locale_directory = g_getenv("TIO_GUI_LOCALE_DIR");
    if (locale_directory == NULL || locale_directory[0] == '\0') {
        locale_directory = g_file_test(TIO_GUI_BUILD_LOCALE_DIR, G_FILE_TEST_IS_DIR)
                               ? TIO_GUI_BUILD_LOCALE_DIR
                               : TIO_GUI_LOCALE_DIR;
    }
    bindtextdomain("tio-gui", locale_directory);
    bind_textdomain_codeset("tio-gui", "UTF-8");
    textdomain("tio-gui");
    if (!apply_language(startup_language)) {
        g_warning("Locale is unavailable for language: %s", startup_language);
    }

    GApplicationFlags application_flags = G_APPLICATION_DEFAULT_FLAGS;
    if (g_getenv("TIO_GUI_NON_UNIQUE") != NULL) {
        application_flags |= G_APPLICATION_NON_UNIQUE;
    }

    g_autoptr(GtkApplication) application =
        gtk_application_new("io.github.keithxc.tio_gui", application_flags);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    return g_application_run(G_APPLICATION(application), argc, argv);
}
