/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L
#define PCRE2_CODE_UNIT_WIDTH 8

#include <errno.h>
#include <glob.h>
#include <libintl.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <pcre2.h>

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <vte/vte.h>

#include "settings.h"

#define _(message) gettext(message)

#define TIO_GUI_HISTORY_MENU_LIMIT 25

typedef struct {
    GtkWidget *window;

    /* Application menu. */
    GtkMenuButton *settings_button;
    GtkPopover *settings_popover;
    GtkDropDown *theme_dropdown;
    GtkLabel *settings_title_label;
    GtkLabel *appearance_section_label;
    GtkLabel *session_section_label;
    GtkLabel *connection_section_label;
    GtkBox *connection_settings_box;
    GtkLabel *backup_section_label;
    GtkLabel *about_section_label;
    GtkLabel *theme_label;
    GtkLabel *timestamp_format_label;
    GtkLabel *timestamp_format_hint;
    GtkLabel *timestamp_preview_label;
    GtkDropDown *log_filename_dropdown;
    GtkLabel *log_filename_format_label;
    GtkLabel *log_filename_preview_label;
    GtkLabel *about_description_label;
    GtkLabel *update_available_label;
    GtkButton *download_update_button;
    GtkLabel *version_label;
    GtkButton *github_button;
    GtkButton *export_settings_button;
    GtkButton *import_settings_button;

    /* Connection toolbar. */
    GtkMenuButton *profile_button;
    GtkPopover *profile_popover;
    GtkListBox *profile_list;
    GtkLabel *profile_empty_label;
    GtkButton *profile_save_button;
    GtkButton *profile_update_button;
    GtkButton *profile_duplicate_button;
    GtkButton *profile_delete_button;
    GtkDropDown *device_dropdown;
    GtkDropDown *baud_dropdown;
    GtkEntry *custom_baud_entry;
    GtkWidget *refresh_button;
    GtkButton *connect_button;
    GtkLabel *device_label;
    GtkLabel *baud_label;

    /* Advanced settings. */
    GtkDropDown *data_bits_dropdown;
    GtkDropDown *stop_bits_dropdown;
    GtkDropDown *parity_dropdown;
    GtkDropDown *flow_dropdown;
    GtkDropDown *language_dropdown;
    GtkLabel *data_bits_label;
    GtkLabel *stop_bits_label;
    GtkLabel *parity_label;
    GtkLabel *flow_label;
    GtkLabel *language_label;
    GtkLabel *output_delay_label;
    GtkLabel *output_line_delay_label;
    GtkLabel *log_warning_label;
    GtkSpinButton *output_delay_spin;
    GtkSpinButton *output_line_delay_spin;
    GtkSpinButton *log_warning_spin;
    GtkCheckButton *local_echo_check;
    GtkCheckButton *show_all_ttys_check;
    GtkCheckButton *timestamp_check;
    GtkDropDown *timestamp_format_dropdown;
    GtkCheckButton *log_check;
    GtkCheckButton *log_append_check;
    GtkCheckButton *log_strip_check;
    GtkEntry *log_directory_entry;
    GtkButton *choose_log_directory_button;
    GtkEntry *log_file_entry;
    GtkButton *open_log_directory_button;

    /* Status. */
    GtkLabel *status_label;
    GtkLabel *session_label;
    GtkButton *clear_terminal_button;

    /* Search. */
    GtkSearchBar *search_bar;
    GtkSearchEntry *search_entry;
    GtkButton *search_previous_button;
    GtkButton *search_next_button;
    GtkToggleButton *search_case_toggle;
    GtkToggleButton *search_regex_toggle;
    gchar *search_pattern;
    guint32 search_flags;

    /* Terminal. */
    VteTerminal *terminal;
    GtkButton *scroll_bottom_button;
    gboolean follow_output;
    gboolean pending_output;

    /* Send bar. */
    GtkMenuButton *history_button;
    GtkPopover *history_popover;
    GtkListBox *history_list;
    GtkButton *history_clear_button;
    GtkLabel *history_empty_label;
    GtkEntry *send_entry;
    GtkDropDown *line_ending_dropdown;
    GtkButton *send_button;
    guint history_cursor;
    gchar *history_draft;
    gboolean history_updating;
    gboolean retranslating;
    gboolean update_check_started;
    gchar *update_url;
    gchar *latest_version;

    /* Quick buttons. */
    GtkButton *customize_quick_buttons;
    GtkButton *quick_buttons[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkToggleButton *hex_toggle;

    /* Session state. */
    GPid child_pid;
    gchar **spawn_argv;
    gchar *log_path;
    gboolean log_warning_shown;
    gint64 connected_at;
    guint session_timer;
    guint settings_preview_timer;
    gint64 preview_started_at;
    gchar *pending_profile_delete;
    TioSettings settings;
} TioGui;

typedef struct {
    GtkWidget *window;
    SoupMessage *message;
} UpdateCheck;

typedef struct {
    TioGui *gui;
    GtkWidget *window;
    GtkEntry *label_entries[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkEntry *payload_entries[TIO_GUI_QUICK_BUTTON_COUNT];
} QuickButtonEditor;

typedef enum {
    TIO_GUI_PROFILE_SAVE_NEW,
    TIO_GUI_PROFILE_SAVE_DUPLICATE,
} TioProfileSaveMode;

typedef struct {
    TioGui *gui;
    GtkWidget *window;
    GtkEntry *name_entry;
    GtkLabel *hint_label;
} ProfileNameDialog;

static void update_quick_buttons(TioGui *gui);
static GtkWidget *make_label(const char *text);
static void set_status(TioGui *gui, const char *message);
static void update_session_label(TioGui *gui);
static void refresh_profile_ui(TioGui *gui);
static void refresh_history_ui(TioGui *gui);
static void capture_session_config(TioGui *gui, TioSessionConfig *config);
static void apply_session_config(TioGui *gui, const TioSessionConfig *config);
static void refresh_devices(TioGui *gui);
static void update_search_regex(TioGui *gui);
static void on_language_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data);
static void capture_all_settings(TioGui *gui);
static gboolean update_settings_previews(gpointer user_data);

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
static const char *const theme_values[] = {"system", "light", "dark", NULL};
static const char *const timestamp_format_values[] = {
    "24hour", "24hour-start", "24hour-delta", "iso8601", "epoch", NULL,
};
static const char *const log_filename_templates[] = {
    "tio-gui-{device}-{date}-{time}.log",
    "{date}-{time}-{device}.log",
    "{device}-{date}-{time}.log",
    NULL,
};
#define TIO_GUI_CUSTOM_LOG_FILENAME_INDEX 3
static const char *const line_ending_values[] = {"none", "lf", "cr", "crlf", NULL};

static const char *line_ending_bytes(const char *line_ending)
{
    if (g_strcmp0(line_ending, "lf") == 0) {
        return "\n";
    }
    if (g_strcmp0(line_ending, "crlf") == 0) {
        return "\r\n";
    }
    if (g_strcmp0(line_ending, "cr") == 0) {
        return "\r";
    }
    return "";
}

static guint value_index(const char *const *values, const char *wanted, guint fallback)
{
    for (guint index = 0; values[index] != NULL; ++index) {
        if (g_strcmp0(values[index], wanted) == 0) {
            return index;
        }
    }
    return fallback;
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

/* Replacing one row of a GtkStringList makes GtkDropDown drop its selection,
   so a translated placeholder row has to be spliced with the selection put
   back afterwards. */
static void replace_dropdown_row(GtkDropDown *dropdown, guint position, const char *text)
{
    GListModel *model = gtk_drop_down_get_model(dropdown);
    if (!GTK_IS_STRING_LIST(model)) {
        return;
    }

    guint selected = gtk_drop_down_get_selected(dropdown);
    const char *rows[] = {text, NULL};
    gtk_string_list_splice(GTK_STRING_LIST(model), position, 1, rows);
    gtk_drop_down_set_selected(dropdown, selected);
}

static void retranslate_ui(TioGui *gui)
{
    /* Re-splicing translated dropdown rows re-emits notify::selected. */
    gui->retranslating = TRUE;
    gtk_label_set_text(gui->device_label, _("Device"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->settings_button), _("Application settings"));
    gtk_label_set_text(gui->settings_title_label, _("Settings"));
    gtk_label_set_text(gui->appearance_section_label, _("Appearance"));
    gtk_label_set_text(gui->session_section_label, _("Session"));
    gtk_label_set_text(gui->connection_section_label, _("Connection and logging"));
    gtk_label_set_text(gui->backup_section_label, _("Backup"));
    gtk_label_set_text(gui->about_section_label, _("About"));
    gtk_label_set_text(gui->theme_label, _("Theme"));
    replace_dropdown_row(gui->theme_dropdown, 0, _("Follow system"));
    replace_dropdown_row(gui->theme_dropdown, 1, _("Light"));
    replace_dropdown_row(gui->theme_dropdown, 2, _("Dark"));
    gtk_label_set_text(gui->timestamp_format_label, _("Timestamp format"));
    gtk_label_set_text(gui->timestamp_format_hint,
                       _("Applied when line timestamps are enabled"));
    gtk_label_set_text(gui->log_filename_format_label, _("Log filename"));
    replace_dropdown_row(gui->log_filename_dropdown, 0, _("tio-gui default"));
    replace_dropdown_row(gui->log_filename_dropdown, 1, _("Date first"));
    replace_dropdown_row(gui->log_filename_dropdown, 2, _("Device first"));
    replace_dropdown_row(gui->log_filename_dropdown, 3, _("Custom…"));
    replace_dropdown_row(gui->timestamp_format_dropdown, 0, _("24-hour"));
    replace_dropdown_row(gui->timestamp_format_dropdown, 1, _("Since start"));
    replace_dropdown_row(gui->timestamp_format_dropdown, 2, _("Since previous"));
    replace_dropdown_row(gui->timestamp_format_dropdown, 3, _("ISO 8601"));
    replace_dropdown_row(gui->timestamp_format_dropdown, 4, _("Unix epoch"));
    gtk_button_set_label(gui->github_button, _("GitHub project"));
    gtk_button_set_label(gui->export_settings_button, _("Export settings…"));
    gtk_button_set_label(gui->import_settings_button, _("Import settings…"));
    gtk_label_set_text(gui->about_description_label,
                       _("A lightweight, reliable GUI for tio"));
    if (gui->latest_version != NULL) {
        g_autofree gchar *update =
            g_strdup_printf(_("Version %s is available"), gui->latest_version);
        gtk_label_set_text(gui->update_available_label, update);
    }
    gtk_button_set_label(gui->download_update_button, _("Download update"));
    g_autofree gchar *version = g_strdup_printf(_("Version %s"), TIO_GUI_VERSION);
    gtk_label_set_text(gui->version_label, version);
    gtk_label_set_text(gui->baud_label, _("Baud"));
    replace_dropdown_row(gui->baud_dropdown, TIO_GUI_CUSTOM_BAUD_INDEX, _("Custom…"));
    replace_dropdown_row(gui->language_dropdown, 0, _("System default"));
    gtk_entry_set_placeholder_text(gui->custom_baud_entry, _("Custom baud"));
    gtk_widget_set_tooltip_text(gui->refresh_button, _("Refresh serial devices"));
    gtk_button_set_label(gui->connect_button,
                         gui->child_pid > 0 ? _("Disconnect") : _("Connect"));
    gtk_check_button_set_label(gui->timestamp_check, _("Timestamps"));
    gtk_check_button_set_label(gui->log_check, _("Log session"));
    gtk_entry_set_placeholder_text(gui->log_directory_entry, _("Log directory"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->choose_log_directory_button),
                                _("Choose log directory"));
    gtk_label_set_text(gui->data_bits_label, _("Data bits"));
    gtk_label_set_text(gui->stop_bits_label, _("Stop bits"));
    gtk_label_set_text(gui->parity_label, _("Parity"));
    gtk_label_set_text(gui->flow_label, _("Flow control"));
    gtk_check_button_set_label(gui->local_echo_check, _("Local echo"));
    gtk_check_button_set_label(gui->show_all_ttys_check, _("Show all TTY devices"));
    gtk_label_set_text(gui->language_label, _("Language"));

    gtk_label_set_text(gui->output_delay_label, _("Character delay (ms)"));
    gtk_label_set_text(gui->output_line_delay_label, _("Line delay (ms)"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->output_delay_spin),
                                _("tio --output-delay: pause between sent characters"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->output_line_delay_spin),
                                _("tio --output-line-delay: pause between sent lines"));
    gtk_label_set_text(gui->log_warning_label, _("Warn above (MB)"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->log_warning_spin),
                                _("Warn once the current log file passes this size; 0 disables it"));
    gtk_entry_set_placeholder_text(gui->log_file_entry,
                                   _("Example: {device}-{date}-{time}.log"));
    gtk_check_button_set_label(gui->log_append_check, _("Append"));
    gtk_check_button_set_label(gui->log_strip_check, _("Strip control characters"));
    gtk_button_set_label(gui->open_log_directory_button, _("Open folder"));

    gtk_button_set_label(gui->clear_terminal_button, _("Clear"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->clear_terminal_button),
                                _("Clear the terminal and its scrollback history"));
    gtk_entry_set_placeholder_text(gui->send_entry, _("Send text…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->send_entry),
                                _("Press Up and Down to browse the send history"));
    gtk_button_set_label(gui->send_button, _("Send"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->history_button), _("Send history"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->line_ending_dropdown), _("Line ending"));
    gtk_button_set_label(gui->history_clear_button, _("Clear history"));
    gtk_label_set_text(gui->history_empty_label, _("No history yet"));
    gtk_button_set_label(gui->customize_quick_buttons, _("Customize…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->hex_toggle),
                                _("Display incoming bytes as 16-byte hex rows"));

    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->profile_button), _("Connection profiles"));
    gtk_menu_button_set_label(gui->profile_button, _("Profiles"));
    gtk_button_set_label(gui->profile_save_button, _("Save as new profile…"));
    gtk_button_set_label(gui->profile_update_button, _("Update this profile"));
    gtk_button_set_label(gui->profile_duplicate_button, _("Duplicate…"));
    gtk_button_set_label(gui->profile_delete_button, _("Delete profile"));
    gtk_label_set_text(gui->profile_empty_label, _("No saved profiles"));

    gtk_search_entry_set_placeholder_text(gui->search_entry, _("Search terminal…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->search_previous_button), _("Find previous"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->search_next_button), _("Find next"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->search_case_toggle), _("Match case"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->search_regex_toggle), _("Regular expression"));
    gtk_button_set_label(gui->scroll_bottom_button,
                         gui->pending_output ? _("New output ↓") : _("Back to bottom ↓"));

    set_status(gui, gui->child_pid > 0 ? _("Connected") : _("Ready"));
    refresh_profile_ui(gui);
    refresh_history_ui(gui);
    update_session_label(gui);
    update_settings_previews(gui);
    gui->retranslating = FALSE;
}

static void on_language_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    TioGui *gui = user_data;
    guint selected = gtk_drop_down_get_selected(dropdown);
    if (gui->retranslating || selected >= G_N_ELEMENTS(language_values) - 1 ||
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

static void apply_theme(const char *theme)
{
    GtkSettings *settings = gtk_settings_get_default();
    if (settings == NULL) {
        return;
    }
    if (g_strcmp0(theme, "system") == 0) {
        gtk_settings_reset_property(settings, "gtk-application-prefer-dark-theme");
    } else {
        g_object_set(settings,
                     "gtk-application-prefer-dark-theme",
                     g_strcmp0(theme, "dark") == 0,
                     NULL);
    }
}

static void on_theme_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    TioGui *gui = user_data;
    guint selected = gtk_drop_down_get_selected(dropdown);
    if (gui->retranslating || selected >= G_N_ELEMENTS(theme_values) - 1) {
        return;
    }
    g_free(gui->settings.theme);
    gui->settings.theme = g_strdup(theme_values[selected]);
    apply_theme(gui->settings.theme);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&gui->settings, &error)) {
        g_warning("Could not save theme setting: %s", error->message);
    }
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

/* /dev/ttyUSB numbering moves around between reboots and re-plugs, so profiles
   remember the by-id name whenever udev created one. */
static gchar *device_stable_id(const char *device)
{
    if (device == NULL || device[0] == '\0') {
        return NULL;
    }

    g_autofree gchar *target = realpath(device, NULL);
    if (target == NULL) {
        return NULL;
    }

    g_autoptr(GDir) directory = g_dir_open("/dev/serial/by-id", 0, NULL);
    if (directory == NULL) {
        return NULL;
    }

    const char *name = NULL;
    while ((name = g_dir_read_name(directory)) != NULL) {
        g_autofree gchar *link = g_build_filename("/dev/serial/by-id", name, NULL);
        g_autofree gchar *resolved = realpath(link, NULL);
        if (g_strcmp0(resolved, target) == 0) {
            return g_steal_pointer(&link);
        }
    }
    return NULL;
}

static gchar *device_from_stable_id(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return NULL;
    }
    return realpath(device_id, NULL);
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
            if (g_strcmp0(g_ptr_array_index(devices, index), gui->settings.session.device) == 0) {
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
    update_session_label(gui);
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
    update_session_label(gui);
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

static void select_baud(TioGui *gui, const char *baud)
{
    for (guint index = 0; baud_rates[index] != NULL; ++index) {
        if (g_strcmp0(baud_rates[index], baud) == 0) {
            gtk_drop_down_set_selected(gui->baud_dropdown, index);
            gtk_widget_set_visible(GTK_WIDGET(gui->custom_baud_entry), FALSE);
            return;
        }
    }
    gtk_editable_set_text(GTK_EDITABLE(gui->custom_baud_entry), baud != NULL ? baud : "");
    gtk_drop_down_set_selected(gui->baud_dropdown, TIO_GUI_CUSTOM_BAUD_INDEX);
    gtk_widget_set_visible(GTK_WIDGET(gui->custom_baud_entry), TRUE);
}

static void capture_session_config(TioGui *gui, TioSessionConfig *config)
{
    const char *device = selected_string(gui->device_dropdown);
    if (device != NULL) {
        g_free(config->device);
        config->device = g_strdup(device);
        g_free(config->device_id);
        config->device_id = device_stable_id(device);
    }

    const char *baud = selected_baud(gui);
    if (baud_is_valid(baud)) {
        g_free(config->baud);
        config->baud = g_strdup(baud);
    }

    g_free(config->data_bits);
    config->data_bits = g_strdup(selected_string(gui->data_bits_dropdown));
    g_free(config->stop_bits);
    config->stop_bits = g_strdup(selected_string(gui->stop_bits_dropdown));
    g_free(config->parity);
    config->parity = g_strdup(selected_string(gui->parity_dropdown));
    g_free(config->flow);
    config->flow = g_strdup(selected_string(gui->flow_dropdown));
    g_free(config->line_ending);
    config->line_ending =
        g_strdup(line_ending_values[gtk_drop_down_get_selected(gui->line_ending_dropdown)]);
    g_free(config->log_directory);
    config->log_directory =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(gui->log_directory_entry)));
    g_free(config->log_file);
    config->log_file = g_strdup(gtk_editable_get_text(GTK_EDITABLE(gui->log_file_entry)));

    config->local_echo = gtk_check_button_get_active(gui->local_echo_check);
    config->hex_output = gtk_toggle_button_get_active(gui->hex_toggle);
    config->timestamps = gtk_check_button_get_active(gui->timestamp_check);
    g_free(config->timestamp_format);
    config->timestamp_format = g_strdup(
        timestamp_format_values[gtk_drop_down_get_selected(gui->timestamp_format_dropdown)]);
    config->logging = gtk_check_button_get_active(gui->log_check);
    config->log_append = gtk_check_button_get_active(gui->log_append_check);
    config->log_strip = gtk_check_button_get_active(gui->log_strip_check);
    config->output_delay = (guint)gtk_spin_button_get_value_as_int(gui->output_delay_spin);
    config->output_line_delay =
        (guint)gtk_spin_button_get_value_as_int(gui->output_line_delay_spin);
}

static void apply_session_config(TioGui *gui, const TioSessionConfig *config)
{
    g_autofree gchar *resolved = device_from_stable_id(config->device_id);
    const char *device = resolved != NULL ? resolved : config->device;
    if (device != NULL) {
        select_string(gui->device_dropdown, device);
    }

    select_baud(gui, config->baud);
    select_string(gui->data_bits_dropdown, config->data_bits);
    select_string(gui->stop_bits_dropdown, config->stop_bits);
    select_string(gui->parity_dropdown, config->parity);
    select_string(gui->flow_dropdown, config->flow);
    gtk_drop_down_set_selected(gui->line_ending_dropdown,
                               value_index(line_ending_values, config->line_ending, 2));
    gtk_check_button_set_active(gui->local_echo_check, config->local_echo);
    gtk_toggle_button_set_active(gui->hex_toggle, config->hex_output);
    gtk_check_button_set_active(gui->timestamp_check, config->timestamps);
    gtk_drop_down_set_selected(
        gui->timestamp_format_dropdown,
        value_index(timestamp_format_values, config->timestamp_format, 3));
    gtk_check_button_set_active(gui->log_check, config->logging);
    gtk_check_button_set_active(gui->log_append_check, config->log_append);
    gtk_check_button_set_active(gui->log_strip_check, config->log_strip);
    gtk_editable_set_text(GTK_EDITABLE(gui->log_directory_entry), config->log_directory);
    guint filename_index = config->log_file[0] == '\0'
                               ? 0
                               : value_index(log_filename_templates,
                                             config->log_file,
                                             TIO_GUI_CUSTOM_LOG_FILENAME_INDEX);
    gtk_drop_down_set_selected(gui->log_filename_dropdown, filename_index);
    gtk_editable_set_text(GTK_EDITABLE(gui->log_file_entry),
                          filename_index == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX
                              ? config->log_file
                              : log_filename_templates[filename_index]);
    gtk_widget_set_visible(GTK_WIDGET(gui->log_file_entry),
                           filename_index == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX);
    gtk_spin_button_set_value(gui->output_delay_spin, config->output_delay);
    gtk_spin_button_set_value(gui->output_line_delay_spin, config->output_line_delay);
    update_session_label(gui);
}

/* ---------------------------------------------------------------- profiles */

static void on_profile_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    (void)list;
    TioGui *gui = user_data;
    const char *name = g_object_get_data(G_OBJECT(row), "profile-name");
    TioProfile *profile = tio_settings_find_profile(&gui->settings, name);
    if (profile == NULL) {
        return;
    }

    apply_session_config(gui, &profile->session);
    g_free(gui->settings.active_profile);
    gui->settings.active_profile = g_strdup(name);
    refresh_profile_ui(gui);
    gtk_popover_popdown(gui->profile_popover);

    g_autofree gchar *message = g_strdup_printf(_("Loaded profile “%s”"), name);
    set_status(gui, message);
}

static void refresh_profile_ui(TioGui *gui)
{
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(gui->profile_list))) != NULL) {
        gtk_list_box_remove(gui->profile_list, child);
    }

    for (guint index = 0; index < gui->settings.profiles->len; ++index) {
        const TioProfile *profile = g_ptr_array_index(gui->settings.profiles, index);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(profile->name);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_widget_set_margin_start(label, 6);
        gtk_widget_set_margin_end(label, 6);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row),
                               "profile-name",
                               g_strdup(profile->name),
                               g_free);
        gtk_list_box_append(gui->profile_list, row);
    }

    gboolean has_profiles = gui->settings.profiles->len > 0;
    gtk_widget_set_visible(GTK_WIDGET(gui->profile_list), has_profiles);
    gtk_widget_set_visible(GTK_WIDGET(gui->profile_empty_label), !has_profiles);

    TioProfile *active = tio_settings_find_profile(&gui->settings, gui->settings.active_profile);
    if (active == NULL) {
        g_clear_pointer(&gui->settings.active_profile, g_free);
    }
    gtk_menu_button_set_label(gui->profile_button,
                              active != NULL ? active->name : _("Profile"));
    gtk_widget_set_sensitive(GTK_WIDGET(gui->profile_update_button), active != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->profile_duplicate_button), active != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->profile_delete_button), active != NULL);
}

static void on_profile_name_cancel(GtkButton *button, gpointer user_data)
{
    (void)button;
    ProfileNameDialog *dialog = user_data;
    gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void on_profile_name_save(GtkButton *button, gpointer user_data)
{
    (void)button;
    ProfileNameDialog *dialog = user_data;
    TioGui *gui = dialog->gui;

    g_autofree gchar *name =
        g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry))));
    if (name[0] == '\0') {
        gtk_label_set_text(dialog->hint_label, _("Enter a profile name"));
        return;
    }
    if (strchr(name, '[') != NULL || strchr(name, ']') != NULL) {
        gtk_label_set_text(dialog->hint_label, _("A profile name cannot contain [ or ]"));
        return;
    }
    if (tio_settings_find_profile(&gui->settings, name) != NULL) {
        gtk_label_set_text(dialog->hint_label, _("That profile name is already in use"));
        return;
    }

    TioSessionConfig captured;
    tio_session_config_init(&captured);
    capture_session_config(gui, &captured);
    tio_settings_store_profile(&gui->settings, name, &captured);
    tio_session_config_clear(&captured);

    g_free(gui->settings.active_profile);
    gui->settings.active_profile = g_strdup(name);
    refresh_profile_ui(gui);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&gui->settings, &error)) {
        g_warning("Could not save profile: %s", error->message);
    }

    g_autofree gchar *message = g_strdup_printf(_("Saved profile “%s”"), name);
    set_status(gui, message);
    gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void present_profile_name_dialog(TioGui *gui, TioProfileSaveMode mode)
{
    ProfileNameDialog *dialog = g_new0(ProfileNameDialog, 1);
    dialog->gui = gui;
    dialog->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog->window),
                         mode == TIO_GUI_PROFILE_SAVE_DUPLICATE ? _("Duplicate profile")
                                                                : _("Save profile"));
    gtk_window_set_transient_for(GTK_WINDOW(dialog->window), GTK_WINDOW(gui->window));
    gtk_window_set_modal(GTK_WINDOW(dialog->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog->window), 380, 120);
    g_object_set_data_full(G_OBJECT(dialog->window), "profile-dialog", dialog, g_free);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(dialog->window), root);

    gtk_box_append(GTK_BOX(root),
                   make_label(_("The profile stores the device, serial settings, logging "
                                "options and quick buttons shown right now.")));

    dialog->name_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(dialog->name_entry, _("Profile name"));
    gtk_entry_set_activates_default(dialog->name_entry, TRUE);
    if (mode == TIO_GUI_PROFILE_SAVE_DUPLICATE && gui->settings.active_profile != NULL) {
        g_autofree gchar *suggestion =
            g_strdup_printf(_("%s copy"), gui->settings.active_profile);
        gtk_editable_set_text(GTK_EDITABLE(dialog->name_entry), suggestion);
    }
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(dialog->name_entry));

    dialog->hint_label = GTK_LABEL(make_label(""));
    gtk_widget_add_css_class(GTK_WIDGET(dialog->hint_label), "dim-label");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(dialog->hint_label));

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label(_("Cancel"));
    GtkWidget *save = gtk_button_new_with_label(_("Save"));
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), save);
    gtk_box_append(GTK_BOX(root), actions);

    g_signal_connect(cancel, "clicked", G_CALLBACK(on_profile_name_cancel), dialog);
    g_signal_connect(save, "clicked", G_CALLBACK(on_profile_name_save), dialog);
    g_signal_connect_swapped(dialog->name_entry, "activate", G_CALLBACK(gtk_widget_activate), save);

    gtk_window_present(GTK_WINDOW(dialog->window));
    gtk_widget_grab_focus(GTK_WIDGET(dialog->name_entry));
}

static void on_profile_save_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    gtk_popover_popdown(gui->profile_popover);
    present_profile_name_dialog(gui, TIO_GUI_PROFILE_SAVE_NEW);
}

static void on_profile_duplicate_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    gtk_popover_popdown(gui->profile_popover);
    present_profile_name_dialog(gui, TIO_GUI_PROFILE_SAVE_DUPLICATE);
}

static void on_profile_update_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    if (gui->settings.active_profile == NULL) {
        return;
    }

    TioSessionConfig captured;
    tio_session_config_init(&captured);
    capture_session_config(gui, &captured);
    tio_settings_store_profile(&gui->settings, gui->settings.active_profile, &captured);
    tio_session_config_clear(&captured);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&gui->settings, &error)) {
        g_warning("Could not save profile: %s", error->message);
    }

    g_autofree gchar *message =
        g_strdup_printf(_("Updated profile “%s”"), gui->settings.active_profile);
    set_status(gui, message);
    gtk_popover_popdown(gui->profile_popover);
}

static void on_profile_delete_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    TioGui *gui = user_data;
    g_autofree gchar *name = g_steal_pointer(&gui->pending_profile_delete);

    g_autoptr(GError) error = NULL;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    if (error != NULL || choice != 1 || name == NULL) {
        return;
    }

    tio_settings_remove_profile(&gui->settings, name);
    if (g_strcmp0(gui->settings.active_profile, name) == 0) {
        g_clear_pointer(&gui->settings.active_profile, g_free);
    }
    refresh_profile_ui(gui);

    g_autoptr(GError) save_error = NULL;
    if (!tio_settings_save(&gui->settings, &save_error)) {
        g_warning("Could not save profiles: %s", save_error->message);
    }

    g_autofree gchar *message = g_strdup_printf(_("Deleted profile “%s”"), name);
    set_status(gui, message);
}

static void on_profile_delete_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    if (gui->settings.active_profile == NULL) {
        return;
    }

    gtk_popover_popdown(gui->profile_popover);
    g_free(gui->pending_profile_delete);
    gui->pending_profile_delete = g_strdup(gui->settings.active_profile);

    g_autofree gchar *question =
        g_strdup_printf(_("Delete profile “%s”?"), gui->settings.active_profile);
    g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", question);
    const char *buttons[] = {_("Cancel"), _("Delete"), NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog,
                            GTK_WINDOW(gui->window),
                            NULL,
                            on_profile_delete_response,
                            gui);
}

/* ----------------------------------------------------------------- session */

static char parity_letter(const char *parity)
{
    if (g_strcmp0(parity, "even") == 0) {
        return 'E';
    }
    if (g_strcmp0(parity, "odd") == 0) {
        return 'O';
    }
    if (g_strcmp0(parity, "mark") == 0) {
        return 'M';
    }
    if (g_strcmp0(parity, "space") == 0) {
        return 'S';
    }
    return 'N';
}

static void update_session_label(TioGui *gui)
{
    const char *device = selected_string(gui->device_dropdown);
    const char *baud = selected_baud(gui);
    const char *data_bits = selected_string(gui->data_bits_dropdown);
    const char *stop_bits = selected_string(gui->stop_bits_dropdown);
    const char *parity = selected_string(gui->parity_dropdown);
    const char *flow = selected_string(gui->flow_dropdown);

    GString *text = g_string_new(NULL);
    g_string_append(text, device != NULL ? device : _("no device"));
    g_string_append_printf(text,
                           " · %s %s%c%s · %s",
                           baud != NULL && baud[0] != '\0' ? baud : "?",
                           data_bits != NULL ? data_bits : "?",
                           parity_letter(parity),
                           stop_bits != NULL ? stop_bits : "?",
                           flow != NULL ? flow : "?");

    if (gui->child_pid > 0) {
        gint64 seconds = (g_get_monotonic_time() - gui->connected_at) / G_USEC_PER_SEC;
        g_string_append_printf(text,
                               " · %02d:%02d:%02d",
                               (int)(seconds / 3600),
                               (int)((seconds / 60) % 60),
                               (int)(seconds % 60));
    }

    if (gui->log_path != NULL) {
        GStatBuf info;
        if (g_stat(gui->log_path, &info) == 0) {
            g_autofree gchar *size = g_format_size((guint64)info.st_size);
            g_string_append_printf(text, " · %s %s", _("log"), size);
        } else {
            g_string_append_printf(text, " · %s", _("log pending"));
        }
    } else if (gtk_check_button_get_active(gui->log_check)) {
        g_string_append_printf(text, " · %s", _("log ready"));
    }

    gtk_label_set_text(gui->session_label, text->str);
    if (gui->log_path != NULL) {
        gtk_widget_set_tooltip_text(GTK_WIDGET(gui->session_label), gui->log_path);
    } else {
        gtk_widget_set_tooltip_text(GTK_WIDGET(gui->session_label), NULL);
    }
    g_string_free(text, TRUE);
}

static gboolean on_session_tick(gpointer user_data)
{
    TioGui *gui = user_data;
    update_session_label(gui);

    if (gui->log_path != NULL && !gui->log_warning_shown && gui->settings.log_warning_mb > 0) {
        GStatBuf info;
        guint64 limit = (guint64)gui->settings.log_warning_mb * 1024U * 1024U;
        if (g_stat(gui->log_path, &info) == 0 && (guint64)info.st_size > limit) {
            gui->log_warning_shown = TRUE;
            g_autofree gchar *size = g_format_size((guint64)info.st_size);
            g_autofree gchar *message =
                g_strdup_printf(_("The session log has reached %s: %s"), size, gui->log_path);
            set_status(gui, message);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void stop_session_timer(TioGui *gui)
{
    if (gui->session_timer != 0) {
        g_source_remove(gui->session_timer);
        gui->session_timer = 0;
    }
}

static void on_child_exited(VteTerminal *terminal, gint status, gpointer user_data)
{
    (void)terminal;
    TioGui *gui = user_data;

    gui->child_pid = -1;
    stop_session_timer(gui);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->connect_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_entry), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_button), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->hex_toggle), TRUE);
    update_quick_buttons(gui);
    gtk_button_set_label(gui->connect_button, _("Connect"));

    g_autofree gchar *message =
        g_strdup_printf(_("Disconnected (tio exit status: %d)"), status);
    set_status(gui, message);
    update_session_label(gui);
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
        g_clear_pointer(&gui->log_path, g_free);
        gtk_button_set_label(gui->connect_button, _("Connect"));
        return;
    }

    gui->child_pid = pid;
    gui->connected_at = g_get_monotonic_time();
    gtk_button_set_label(gui->connect_button, _("Disconnect"));
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->hex_toggle), FALSE);
    update_quick_buttons(gui);
    set_status(gui, _("Connected"));
    stop_session_timer(gui);
    gui->session_timer = g_timeout_add_seconds(1, on_session_tick, gui);
    update_session_label(gui);
    gtk_widget_grab_focus(GTK_WIDGET(gui->terminal));
}

/* tio can name log files by itself, but then tio-gui would not know which file
   to watch or open, so the full path is always decided here. */
static gchar *build_log_path(const TioSessionConfig *config)
{
    if (config->log_file != NULL && config->log_file[0] != '\0') {
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autofree gchar *date = g_date_time_format(now, "%Y%m%d");
        g_autofree gchar *time = g_date_time_format(now, "%H%M%S");
        g_autofree gchar *device = config->device != NULL
                                       ? g_path_get_basename(config->device)
                                       : g_strdup("serial");
        g_autofree gchar *expanded = g_strdup(config->log_file);
        const struct {
            const char *token;
            const char *value;
        } replacements[] = {
            {"{device}", device}, {"{date}", date}, {"{time}", time},
        };
        for (gsize index = 0; index < G_N_ELEMENTS(replacements); ++index) {
            gchar **parts = g_strsplit(expanded, replacements[index].token, -1);
            g_free(expanded);
            expanded = g_strjoinv(replacements[index].value, parts);
            g_strfreev(parts);
        }
        if (g_path_is_absolute(expanded)) {
            return g_steal_pointer(&expanded);
        }
        return g_build_filename(config->log_directory, expanded, NULL);
    }

    g_autofree gchar *device = config->device != NULL
                                   ? g_path_get_basename(config->device)
                                   : g_strdup("serial");
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_autofree gchar *name = g_strdup_printf("tio-gui-%s-%s.log", device, stamp);
    return g_build_filename(config->log_directory, name, NULL);
}

static gchar **build_tio_argv(const TioSessionConfig *config, const char *log_path)
{
    GPtrArray *arguments = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(arguments, g_strdup("tio"));
    g_ptr_array_add(arguments, g_strdup("--baudrate"));
    g_ptr_array_add(arguments, g_strdup(config->baud));
    g_ptr_array_add(arguments, g_strdup("--databits"));
    g_ptr_array_add(arguments, g_strdup(config->data_bits));
    g_ptr_array_add(arguments, g_strdup("--flow"));
    g_ptr_array_add(arguments, g_strdup(config->flow));
    g_ptr_array_add(arguments, g_strdup("--stopbits"));
    g_ptr_array_add(arguments, g_strdup(config->stop_bits));
    g_ptr_array_add(arguments, g_strdup("--parity"));
    g_ptr_array_add(arguments, g_strdup(config->parity));

    if (config->local_echo) {
        g_ptr_array_add(arguments, g_strdup("--local-echo"));
    }
    if (config->hex_output) {
        g_ptr_array_add(arguments, g_strdup("--output-mode"));
        g_ptr_array_add(arguments, g_strdup("hex16"));
    }
    if (config->output_delay > 0) {
        g_ptr_array_add(arguments, g_strdup("--output-delay"));
        g_ptr_array_add(arguments, g_strdup_printf("%u", config->output_delay));
    }
    if (config->output_line_delay > 0) {
        g_ptr_array_add(arguments, g_strdup("--output-line-delay"));
        g_ptr_array_add(arguments, g_strdup_printf("%u", config->output_line_delay));
    }

    if (config->timestamps) {
        g_ptr_array_add(arguments, g_strdup("--timestamp"));
        g_ptr_array_add(arguments, g_strdup("--timestamp-format"));
        g_ptr_array_add(arguments, g_strdup(config->timestamp_format));
    }
    if (config->logging && log_path != NULL) {
        g_ptr_array_add(arguments, g_strdup("--log"));
        g_ptr_array_add(arguments, g_strdup("--log-file"));
        g_ptr_array_add(arguments, g_strdup(log_path));
        if (config->log_append) {
            g_ptr_array_add(arguments, g_strdup("--log-append"));
        }
        if (config->log_strip) {
            g_ptr_array_add(arguments, g_strdup("--log-strip"));
        }
    }

    g_ptr_array_add(arguments, g_strdup(config->device));
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

    capture_session_config(gui, &gui->settings.session);
    const TioSessionConfig *config = &gui->settings.session;

    g_clear_pointer(&gui->log_path, g_free);
    gui->log_warning_shown = FALSE;
    if (config->logging) {
        if (config->log_directory[0] == '\0') {
            set_status(gui, _("Choose a log directory first"));
            return;
        }
        gui->log_path = build_log_path(config);
        g_autofree gchar *log_parent = g_path_get_dirname(gui->log_path);
        if (g_mkdir_with_parents(log_parent, 0750) == -1) {
            g_autofree gchar *message =
                g_strdup_printf(_("Could not create log directory: %s"), g_strerror(errno));
            set_status(gui, message);
            g_clear_pointer(&gui->log_path, g_free);
            return;
        }
    }

    vte_terminal_reset(gui->terminal, TRUE, TRUE);
    gui->spawn_argv = build_tio_argv(config, gui->log_path);
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

static void on_serial_setting_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)dropdown;
    (void)pspec;
    update_session_label(user_data);
}

/* ------------------------------------------------------------------- send */

static void send_bytes(TioGui *gui, const char *data, gsize length)
{
    if (gui->child_pid <= 0 || length == 0) {
        return;
    }
    vte_terminal_feed_child(gui->terminal, data, (gssize)length);
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

    g_autofree gchar *payload = g_strdup(text);
    send_bytes(gui, payload, strlen(payload));
    const char *ending =
        line_ending_bytes(line_ending_values[gtk_drop_down_get_selected(gui->line_ending_dropdown)]);
    if (ending[0] != '\0') {
        send_bytes(gui, ending, strlen(ending));
    }

    tio_settings_push_history(&gui->settings, payload);
    refresh_history_ui(gui);
    gui->history_cursor = 0;
    g_clear_pointer(&gui->history_draft, g_free);

    gui->history_updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(gui->send_entry), "");
    gui->history_updating = FALSE;
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

static void on_send_entry_changed(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    TioGui *gui = user_data;
    if (!gui->history_updating) {
        gui->history_cursor = 0;
    }
}

/* Cursor 0 is the line being typed; 1 is the newest stored entry. */
static void history_navigate(TioGui *gui, gint direction)
{
    GPtrArray *history = gui->settings.history;
    if (history->len == 0) {
        return;
    }

    gint cursor = (gint)gui->history_cursor + direction;
    cursor = CLAMP(cursor, 0, (gint)history->len);
    if (cursor == (gint)gui->history_cursor) {
        return;
    }

    if (gui->history_cursor == 0) {
        g_free(gui->history_draft);
        gui->history_draft = g_strdup(gtk_editable_get_text(GTK_EDITABLE(gui->send_entry)));
    }

    const char *text = cursor == 0
                           ? (gui->history_draft != NULL ? gui->history_draft : "")
                           : g_ptr_array_index(history, history->len - (guint)cursor);

    gui->history_updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(gui->send_entry), text);
    gtk_editable_set_position(GTK_EDITABLE(gui->send_entry), -1);
    gui->history_updating = FALSE;
    gui->history_cursor = (guint)cursor;
}

static gboolean on_send_entry_key(GtkEventControllerKey *controller,
                                  guint keyval,
                                  guint keycode,
                                  GdkModifierType state,
                                  gpointer user_data)
{
    (void)controller;
    (void)keycode;
    TioGui *gui = user_data;

    if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) != 0) {
        return GDK_EVENT_PROPAGATE;
    }
    if (keyval == GDK_KEY_Up) {
        history_navigate(gui, 1);
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_Down) {
        history_navigate(gui, -1);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void on_history_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    (void)list;
    TioGui *gui = user_data;
    const char *text = g_object_get_data(G_OBJECT(row), "history-text");
    if (text == NULL) {
        return;
    }

    gui->history_updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(gui->send_entry), text);
    gtk_editable_set_position(GTK_EDITABLE(gui->send_entry), -1);
    gui->history_updating = FALSE;
    gui->history_cursor = 0;
    gtk_popover_popdown(gui->history_popover);
    gtk_widget_grab_focus(GTK_WIDGET(gui->send_entry));
}

static void refresh_history_ui(TioGui *gui)
{
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(gui->history_list))) != NULL) {
        gtk_list_box_remove(gui->history_list, child);
    }

    GPtrArray *history = gui->settings.history;
    guint shown = MIN(history->len, TIO_GUI_HISTORY_MENU_LIMIT);
    for (guint index = 0; index < shown; ++index) {
        const char *text = g_ptr_array_index(history, history->len - 1 - index);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 32);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 32);
        gtk_widget_set_margin_top(label, 3);
        gtk_widget_set_margin_bottom(label, 3);
        gtk_widget_set_margin_start(label, 6);
        gtk_widget_set_margin_end(label, 6);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "history-text", g_strdup(text), g_free);
        gtk_list_box_append(gui->history_list, row);
    }

    gboolean has_history = history->len > 0;
    gtk_widget_set_visible(GTK_WIDGET(gui->history_list), has_history);
    gtk_widget_set_visible(GTK_WIDGET(gui->history_empty_label), !has_history);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->history_clear_button), has_history);
}

static void on_history_clear_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;

    tio_settings_clear_history(&gui->settings);
    gui->history_cursor = 0;
    g_clear_pointer(&gui->history_draft, g_free);
    refresh_history_ui(gui);
    gtk_popover_popdown(gui->history_popover);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&gui->settings, &error)) {
        g_warning("Could not save settings: %s", error->message);
    }
    set_status(gui, _("Send history cleared"));
}

/* ----------------------------------------------------------------- search */

static void update_search_regex(TioGui *gui)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(gui->search_entry));
    if (text == NULL || text[0] == '\0') {
        vte_terminal_search_set_regex(gui->terminal, NULL, 0);
        gtk_widget_remove_css_class(GTK_WIDGET(gui->search_entry), "error");
        g_clear_pointer(&gui->search_pattern, g_free);
        return;
    }

    g_autofree gchar *pattern = gtk_toggle_button_get_active(gui->search_regex_toggle)
                                    ? g_strdup(text)
                                    : g_regex_escape_string(text, -1);
    guint32 flags = PCRE2_MULTILINE | PCRE2_UTF | PCRE2_NO_UTF_CHECK;
    if (!gtk_toggle_button_get_active(gui->search_case_toggle)) {
        flags |= PCRE2_CASELESS;
    }

    /* Installing a regex restarts VTE's search from the current view, so an
       unchanged pattern has to be left alone for Find next to keep advancing. */
    if (flags == gui->search_flags && g_strcmp0(pattern, gui->search_pattern) == 0 &&
        vte_terminal_search_get_regex(gui->terminal) != NULL) {
        return;
    }

    g_autoptr(GError) error = NULL;
    VteRegex *regex = vte_regex_new_for_search(pattern, -1, flags, &error);
    if (regex == NULL) {
        vte_terminal_search_set_regex(gui->terminal, NULL, 0);
        g_clear_pointer(&gui->search_pattern, g_free);
        gtk_widget_add_css_class(GTK_WIDGET(gui->search_entry), "error");
        g_autofree gchar *message =
            g_strdup_printf(_("Invalid search pattern: %s"), error->message);
        set_status(gui, message);
        return;
    }

    gtk_widget_remove_css_class(GTK_WIDGET(gui->search_entry), "error");
    vte_terminal_search_set_regex(gui->terminal, regex, 0);
    vte_terminal_search_set_wrap_around(gui->terminal, TRUE);
    vte_regex_unref(regex);
    g_free(gui->search_pattern);
    gui->search_pattern = g_steal_pointer(&pattern);
    gui->search_flags = flags;
}

static void search_step(TioGui *gui, gboolean forward)
{
    update_search_regex(gui);
    if (vte_terminal_search_get_regex(gui->terminal) == NULL) {
        return;
    }

    gboolean found = forward ? vte_terminal_search_find_next(gui->terminal)
                             : vte_terminal_search_find_previous(gui->terminal);
    if (!found) {
        set_status(gui, _("No matches"));
    }
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    update_search_regex(user_data);
}

static void on_search_next(GtkButton *button, gpointer user_data)
{
    (void)button;
    search_step(user_data, TRUE);
}

static void on_search_previous(GtkButton *button, gpointer user_data)
{
    (void)button;
    search_step(user_data, FALSE);
}

static void on_search_activate(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    search_step(user_data, TRUE);
}

static void on_search_option_toggled(GtkToggleButton *button, gpointer user_data)
{
    (void)button;
    update_search_regex(user_data);
}

static void on_search_stopped(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    TioGui *gui = user_data;
    gtk_search_bar_set_search_mode(gui->search_bar, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(gui->terminal));
}

/* ------------------------------------------------------------- autoscroll */

static GtkAdjustment *terminal_adjustment(TioGui *gui)
{
    return gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(gui->terminal));
}

static gboolean terminal_at_bottom(TioGui *gui)
{
    GtkAdjustment *adjustment = terminal_adjustment(gui);
    if (adjustment == NULL) {
        return TRUE;
    }
    double value = gtk_adjustment_get_value(adjustment);
    double upper = gtk_adjustment_get_upper(adjustment);
    double page = gtk_adjustment_get_page_size(adjustment);
    return value >= upper - page - 0.5;
}

static void scroll_terminal_to_bottom(TioGui *gui)
{
    GtkAdjustment *adjustment = terminal_adjustment(gui);
    if (adjustment == NULL) {
        return;
    }
    gtk_adjustment_set_value(adjustment,
                             gtk_adjustment_get_upper(adjustment) -
                                 gtk_adjustment_get_page_size(adjustment));
}

static void update_scroll_button(TioGui *gui)
{
    gtk_button_set_label(gui->scroll_bottom_button,
                         gui->pending_output ? _("New output ↓") : _("Back to bottom ↓"));
    gtk_widget_set_visible(GTK_WIDGET(gui->scroll_bottom_button), !gui->follow_output);
}

static void on_terminal_scrolled(GtkAdjustment *adjustment, gpointer user_data)
{
    (void)adjustment;
    TioGui *gui = user_data;

    gboolean at_bottom = terminal_at_bottom(gui);
    if (at_bottom == gui->follow_output) {
        return;
    }
    gui->follow_output = at_bottom;
    if (at_bottom) {
        gui->pending_output = FALSE;
    }
    update_scroll_button(gui);
}

static void on_terminal_content_changed(GtkAdjustment *adjustment, gpointer user_data)
{
    (void)adjustment;
    TioGui *gui = user_data;

    if (gui->follow_output) {
        scroll_terminal_to_bottom(gui);
        return;
    }
    if (!gui->pending_output) {
        gui->pending_output = TRUE;
        update_scroll_button(gui);
    }
}

static void on_scroll_bottom_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;

    scroll_terminal_to_bottom(gui);
    gui->follow_output = TRUE;
    gui->pending_output = FALSE;
    update_scroll_button(gui);
}

/* ---------------------------------------------------------- quick buttons */

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
    g_autoptr(GByteArray) bytes =
        decode_quick_payload(gui->settings.session.quick_payloads[index]);
    send_bytes(gui, (const char *)bytes->data, bytes->len);
}

static void update_quick_buttons(TioGui *gui)
{
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        GtkButton *button = gui->quick_buttons[index];
        if (button == NULL) {
            continue;
        }
        gtk_button_set_label(button, gui->settings.session.quick_labels[index]);
        gtk_widget_set_tooltip_text(GTK_WIDGET(button),
                                    gui->settings.session.quick_payloads[index]);
        gtk_widget_set_sensitive(GTK_WIDGET(button),
                                 gui->child_pid > 0 &&
                                     gui->settings.session.quick_payloads[index][0] != '\0');
    }
}

static void on_quick_editor_save(GtkButton *button, gpointer user_data)
{
    (void)button;
    QuickButtonEditor *editor = user_data;
    TioSessionConfig *session = &editor->gui->settings.session;

    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_free(session->quick_labels[index]);
        session->quick_labels[index] =
            g_strdup(gtk_editable_get_text(GTK_EDITABLE(editor->label_entries[index])));
        g_free(session->quick_payloads[index]);
        session->quick_payloads[index] =
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
                              gui->settings.session.quick_labels[index]);
        gtk_grid_attach(GTK_GRID(grid),
                        GTK_WIDGET(editor->label_entries[index]),
                        1, (gint)index + 1, 1, 1);

        editor->payload_entries[index] = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_text(GTK_EDITABLE(editor->payload_entries[index]),
                              gui->settings.session.quick_payloads[index]);
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

/* ---------------------------------------------------------------- logging */

static void on_log_toggled(GtkCheckButton *button, gpointer user_data)
{
    TioGui *gui = user_data;
    gboolean enabled = gtk_check_button_get_active(button);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->log_directory_entry), enabled);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->choose_log_directory_button), enabled);
    if (enabled) {
        gtk_check_button_set_active(gui->timestamp_check, TRUE);
    }
    update_session_label(gui);
}

static void on_open_log_directory(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;

    const char *directory = gtk_editable_get_text(GTK_EDITABLE(gui->log_directory_entry));
    if (directory[0] == '\0') {
        set_status(gui, _("Choose a log directory first"));
        return;
    }
    if (g_mkdir_with_parents(directory, 0750) == -1) {
        g_autofree gchar *message =
            g_strdup_printf(_("Could not create log directory: %s"), g_strerror(errno));
        set_status(gui, message);
        return;
    }

    g_autoptr(GFile) file = g_file_new_for_path(directory);
    g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, GTK_WINDOW(gui->window), NULL, NULL, NULL);
}

/* -------------------------------------------------------------- shortcuts */

static void action_clear_terminal(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioGui *gui = user_data;
    vte_terminal_reset(gui->terminal, TRUE, TRUE);
}

static void action_copy(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioGui *gui = user_data;
    vte_terminal_copy_clipboard_format(gui->terminal, VTE_FORMAT_TEXT);
}

static void action_paste(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioGui *gui = user_data;
    vte_terminal_paste_clipboard(gui->terminal);
}

static void action_connect(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioGui *gui = user_data;
    if (gui->child_pid <= 0) {
        connect_tio(gui);
    }
}

static void action_disconnect(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    disconnect_tio(user_data);
}

static void action_search(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioGui *gui = user_data;

    gboolean active = !gtk_search_bar_get_search_mode(gui->search_bar);
    gtk_search_bar_set_search_mode(gui->search_bar, active);
    if (active) {
        gtk_widget_grab_focus(GTK_WIDGET(gui->search_entry));
    } else {
        gtk_widget_grab_focus(GTK_WIDGET(gui->terminal));
    }
}

static void install_shortcuts(GtkApplication *application, TioGui *gui)
{
    static const GActionEntry entries[] = {
        {.name = "clear-terminal", .activate = action_clear_terminal},
        {.name = "copy", .activate = action_copy},
        {.name = "paste", .activate = action_paste},
        {.name = "connect", .activate = action_connect},
        {.name = "disconnect", .activate = action_disconnect},
        {.name = "search", .activate = action_search},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(gui->window),
                                    entries,
                                    G_N_ELEMENTS(entries),
                                    gui);

    /* Only Shift-modified control combinations and the function keys are
       claimed, so plain Ctrl-C and friends still reach the serial device. */
    static const struct {
        const char *action;
        const char *accelerator;
    } accelerators[] = {
        {"win.clear-terminal", "<Control><Shift>l"},
        {"win.copy", "<Control><Shift>c"},
        {"win.paste", "<Control><Shift>v"},
        {"win.search", "<Control><Shift>f"},
        {"win.connect", "F5"},
        {"win.disconnect", "F6"},
    };
    for (gsize index = 0; index < G_N_ELEMENTS(accelerators); ++index) {
        const char *keys[] = {accelerators[index].accelerator, NULL};
        gtk_application_set_accels_for_action(application, accelerators[index].action, keys);
    }
}

/* ------------------------------------------------------------------- exit */

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data)
{
    (void)window;
    TioGui *gui = user_data;

    if (gui->child_pid > 0) {
        (void)kill(gui->child_pid, SIGHUP);
    }
    stop_session_timer(gui);

    capture_all_settings(gui);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&gui->settings, &error)) {
        g_warning("Could not save settings: %s", error->message);
    }
    g_clear_pointer(&gui->spawn_argv, g_strfreev);
    return FALSE;
}

static void on_window_destroy(GtkWidget *window, gpointer user_data)
{
    (void)window;
    g_object_set_data(G_OBJECT(user_data), "tio-gui-window", NULL);
}

static void tio_gui_free(gpointer data)
{
    TioGui *gui = data;

    stop_session_timer(gui);
    if (gui->settings_preview_timer != 0) {
        g_source_remove(gui->settings_preview_timer);
        gui->settings_preview_timer = 0;
    }
    g_clear_pointer(&gui->spawn_argv, g_strfreev);
    g_clear_pointer(&gui->log_path, g_free);
    g_clear_pointer(&gui->history_draft, g_free);
    g_clear_pointer(&gui->pending_profile_delete, g_free);
    g_clear_pointer(&gui->search_pattern, g_free);
    g_clear_pointer(&gui->update_url, g_free);
    g_clear_pointer(&gui->latest_version, g_free);
    tio_settings_clear(&gui->settings);
    g_free(gui);
}

/* --------------------------------------------------------------------- UI */

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

static GtkSpinButton *make_spin_button(guint value, double maximum, double step)
{
    GtkSpinButton *spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0.0, maximum, step));
    gtk_spin_button_set_value(spin, value);
    gtk_editable_set_width_chars(GTK_EDITABLE(spin), 6);
    return spin;
}

/* One row of the advanced panel: a horizontal group that only ever asks for
   the width of its own contents. */
static GtkWidget *make_settings_row(GtkWidget *parent)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(parent), row);
    return row;
}

static void append_labelled(GtkWidget *row, GtkLabel *label, GtkWidget *control)
{
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(label));
    gtk_box_append(GTK_BOX(row), control);
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
        ".session-status { font-size: 0.9em; }"
        ".settings-title {"
        "  font-size: 1.15em;"
        "  font-weight: 700;"
        "}"
        ".settings-section-title {"
        "  font-size: 0.78em;"
        "  font-weight: 700;"
        "  opacity: 0.65;"
        "  margin-top: 4px;"
        "}"
        ".settings-card {"
        "  background-color: alpha(@theme_fg_color, 0.055);"
        "  border: 1px solid alpha(@theme_fg_color, 0.12);"
        "  border-radius: 8px;"
        "  padding: 9px;"
        "}"
        ".settings-hint { font-size: 0.82em; opacity: 0.62; }"
        ".settings-version { font-weight: 700; }"
        ".scroll-bottom {"
        "  margin: 10px;"
        "  opacity: 0.92;"
        "}");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void capture_all_settings(TioGui *gui)
{
    capture_session_config(gui, &gui->settings.session);
    gui->settings.show_all_ttys = gtk_check_button_get_active(gui->show_all_ttys_check);
    gui->settings.advanced_expanded = FALSE;
    gui->settings.log_warning_mb =
        (guint)gtk_spin_button_get_value_as_int(gui->log_warning_spin);
}

static void on_github_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    GtkUriLauncher *launcher =
        gtk_uri_launcher_new("https://github.com/keithxc/tio-gui");
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(gui->window), NULL, NULL, NULL);
    g_object_unref(launcher);
}

static gint compare_versions(const char *left, const char *right)
{
    while (*left == 'v' || *left == 'V') {
        ++left;
    }
    while (*right == 'v' || *right == 'V') {
        ++right;
    }
    g_auto(GStrv) left_parts = g_strsplit(left, ".", 4);
    g_auto(GStrv) right_parts = g_strsplit(right, ".", 4);
    for (guint index = 0; index < 3; ++index) {
        guint64 left_value = left_parts[index] != NULL
                                 ? g_ascii_strtoull(left_parts[index], NULL, 10)
                                 : 0;
        guint64 right_value = right_parts[index] != NULL
                                  ? g_ascii_strtoull(right_parts[index], NULL, 10)
                                  : 0;
        if (left_value != right_value) {
            return left_value > right_value ? 1 : -1;
        }
    }
    return 0;
}

static void update_check_free(UpdateCheck *check)
{
    g_object_unref(check->message);
    g_object_unref(check->window);
    g_free(check);
}

static void on_update_check_finished(GObject *source,
                                     GAsyncResult *result,
                                     gpointer user_data)
{
    UpdateCheck *check = user_data;
    TioGui *gui = g_object_get_data(G_OBJECT(check->window), "tio-gui");
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) body =
        soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
    if (gui == NULL || body == NULL ||
        soup_message_get_status(check->message) != SOUP_STATUS_OK) {
        update_check_free(check);
        return;
    }

    gsize length = 0;
    const gchar *data = g_bytes_get_data(body, &length);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, (gssize)length, NULL)) {
        update_check_free(check);
        return;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        update_check_free(check);
        return;
    }

    JsonObject *object = json_node_get_object(root);
    const char *tag = json_object_get_string_member_with_default(object, "tag_name", NULL);
    const char *url = json_object_get_string_member_with_default(object, "html_url", NULL);
    if (tag != NULL && url != NULL && compare_versions(tag, TIO_GUI_VERSION) > 0) {
        g_free(gui->latest_version);
        gui->latest_version = g_strdup(tag);
        g_free(gui->update_url);
        gui->update_url = g_strdup(url);
        g_autofree gchar *message = g_strdup_printf(_("Version %s is available"), tag);
        gtk_label_set_text(gui->update_available_label, message);
        gtk_widget_set_visible(GTK_WIDGET(gui->update_available_label), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(gui->download_update_button), TRUE);
    }
    update_check_free(check);
}

static void on_settings_popover_visible(GObject *object,
                                        GParamSpec *pspec,
                                        gpointer user_data)
{
    (void)pspec;
    TioGui *gui = user_data;
    if (!gtk_widget_get_visible(GTK_WIDGET(object)) || gui->update_check_started) {
        return;
    }
    gui->update_check_started = TRUE;
    SoupSession *session = soup_session_new();
    SoupMessage *message = soup_message_new(
        "GET", "https://api.github.com/repos/keithxc/tio-gui/releases/latest");
    SoupMessageHeaders *headers = soup_message_get_request_headers(message);
    soup_message_headers_append(headers, "Accept", "application/vnd.github+json");
    soup_message_headers_append(headers, "User-Agent", "tio-gui");
    soup_message_headers_append(headers, "X-GitHub-Api-Version", "2022-11-28");
    UpdateCheck *check = g_new0(UpdateCheck, 1);
    check->window = g_object_ref(gui->window);
    check->message = g_object_ref(message);
    soup_session_send_and_read_async(session,
                                     message,
                                     G_PRIORITY_DEFAULT,
                                     NULL,
                                     on_update_check_finished,
                                     check);
    g_object_unref(message);
    g_object_unref(session);
}

static void on_download_update_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    if (gui->update_url == NULL) {
        return;
    }
    GtkUriLauncher *launcher = gtk_uri_launcher_new(gui->update_url);
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(gui->window), NULL, NULL, NULL);
    g_object_unref(launcher);
}
static void on_export_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    TioGui *gui = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    if (file == NULL) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            set_status(gui, error->message);
        }
        return;
    }

    g_autofree gchar *path = g_file_get_path(file);
    if (path == NULL) {
        set_status(gui, _("Choose a local file for settings export"));
        return;
    }
    capture_all_settings(gui);
    if (!tio_settings_save_to_file(&gui->settings, path, &error)) {
        set_status(gui, error->message);
        return;
    }
    set_status(gui, _("Settings exported"));
}

static void on_export_settings_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Export settings"));
    gtk_file_dialog_set_initial_name(dialog, "tio-gui-settings.ini");
    gtk_file_dialog_save(dialog,
                         GTK_WINDOW(gui->window),
                         NULL,
                         on_export_finished,
                         gui);
    g_object_unref(dialog);
}

static void apply_imported_settings(TioGui *gui)
{
    apply_theme(gui->settings.theme);
    gtk_drop_down_set_selected(gui->theme_dropdown,
                               value_index(theme_values, gui->settings.theme, 0));
    gtk_drop_down_set_selected(gui->language_dropdown,
                               value_index(language_values, gui->settings.language, 0));
    gtk_check_button_set_active(gui->show_all_ttys_check, gui->settings.show_all_ttys);
    gtk_spin_button_set_value(gui->log_warning_spin, gui->settings.log_warning_mb);
    apply_session_config(gui, &gui->settings.session);
    update_quick_buttons(gui);
    refresh_profile_ui(gui);
    refresh_history_ui(gui);
    refresh_devices(gui);
    g_unsetenv("TIO_GUI_LANGUAGE");
    if (apply_language(gui->settings.language)) {
        retranslate_ui(gui);
    }
}

static void on_import_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    TioGui *gui = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (file == NULL) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            set_status(gui, error->message);
        }
        return;
    }

    g_autofree gchar *path = g_file_get_path(file);
    if (path == NULL) {
        set_status(gui, _("Choose a local settings file"));
        return;
    }

    TioSettings imported;
    tio_settings_init(&imported);
    if (!tio_settings_load_from_file(&imported, path, &error)) {
        tio_settings_clear(&imported);
        set_status(gui, error->message);
        return;
    }
    tio_settings_clear(&gui->settings);
    gui->settings = imported;
    apply_imported_settings(gui);
    if (!tio_settings_save(&gui->settings, &error)) {
        set_status(gui, error->message);
        return;
    }
    set_status(gui, _("Settings imported"));
}

static void on_import_settings_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    if (gui->child_pid > 0) {
        set_status(gui, _("Disconnect before importing settings"));
        return;
    }
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Import settings"));
    gtk_file_dialog_open(dialog,
                         GTK_WINDOW(gui->window),
                         NULL,
                         on_import_finished,
                         gui);
    g_object_unref(dialog);
}

static void on_log_directory_finished(GObject *source,
                                      GAsyncResult *result,
                                      gpointer user_data)
{
    TioGui *gui = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) folder =
        gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
    if (folder == NULL) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            set_status(gui, error->message);
        }
        return;
    }
    g_autofree gchar *path = g_file_get_path(folder);
    if (path == NULL) {
        set_status(gui, _("Choose a local log directory"));
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(gui->log_directory_entry), path);
    set_status(gui, _("Log directory selected"));
}

static void on_choose_log_directory(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioGui *gui = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Choose log directory"));
    const char *current = gtk_editable_get_text(GTK_EDITABLE(gui->log_directory_entry));
    g_autoptr(GFile) initial = NULL;
    if (current[0] != '\0') {
        initial = g_file_new_for_path(current);
        gtk_file_dialog_set_initial_folder(dialog, initial);
    }
    gtk_file_dialog_select_folder(dialog,
                                  GTK_WINDOW(gui->window),
                                  NULL,
                                  on_log_directory_finished,
                                  gui);
    g_object_unref(dialog);
}

static gchar *timestamp_preview(TioGui *gui)
{
    guint selected = gtk_drop_down_get_selected(gui->timestamp_format_dropdown);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    if (selected == 0) {
        g_autofree gchar *base = g_date_time_format(now, "%H:%M:%S");
        return g_strdup_printf("[%s.%03d]", base, g_date_time_get_microsecond(now) / 1000);
    }
    if (selected == 1) {
        gint64 elapsed = g_get_monotonic_time() - gui->preview_started_at;
        return g_strdup_printf("[%02lld:%02lld:%02lld.%03lld]",
                               (long long)(elapsed / G_USEC_PER_SEC / 3600),
                               (long long)(elapsed / G_USEC_PER_SEC / 60 % 60),
                               (long long)(elapsed / G_USEC_PER_SEC % 60),
                               (long long)(elapsed / 1000 % 1000));
    }
    if (selected == 2) {
        return g_strdup("[00:00:00.250]");
    }
    if (selected == 3) {
        g_autofree gchar *base = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S");
        return g_strdup_printf("[%s.%03d]", base, g_date_time_get_microsecond(now) / 1000);
    }
    return g_strdup_printf("[%lld.%03d]",
                           (long long)g_date_time_to_unix(now),
                           g_date_time_get_microsecond(now) / 1000);
}

static gboolean update_settings_previews(gpointer user_data)
{
    TioGui *gui = user_data;
    g_autofree gchar *timestamp = timestamp_preview(gui);
    g_autofree gchar *timestamp_text = g_strdup_printf(_("Preview: %s"), timestamp);
    gtk_label_set_text(gui->timestamp_preview_label, timestamp_text);

    TioSessionConfig preview;
    tio_session_config_init(&preview);
    g_free(preview.device);
    preview.device = g_strdup(selected_string(gui->device_dropdown));
    g_free(preview.log_directory);
    preview.log_directory = g_strdup("/");
    g_free(preview.log_file);
    preview.log_file = g_strdup(gtk_editable_get_text(GTK_EDITABLE(gui->log_file_entry)));
    g_autofree gchar *path = build_log_path(&preview);
    g_autofree gchar *basename = g_path_get_basename(path);
    g_autofree gchar *filename_text = g_strdup_printf(_("Preview: %s"), basename);
    gtk_label_set_text(gui->log_filename_preview_label, filename_text);
    tio_session_config_clear(&preview);
    return G_SOURCE_CONTINUE;
}

static void on_log_filename_format_changed(GtkDropDown *dropdown,
                                           GParamSpec *pspec,
                                           gpointer user_data)
{
    (void)pspec;
    TioGui *gui = user_data;
    guint selected = gtk_drop_down_get_selected(dropdown);
    gboolean custom = selected == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX;
    gtk_widget_set_visible(GTK_WIDGET(gui->log_file_entry), custom);
    if (!custom && selected < G_N_ELEMENTS(log_filename_templates) - 1) {
        gtk_editable_set_text(GTK_EDITABLE(gui->log_file_entry),
                              log_filename_templates[selected]);
    }
    update_settings_previews(gui);
}

static void on_timestamp_format_changed(GtkDropDown *dropdown,
                                        GParamSpec *pspec,
                                        gpointer user_data)
{
    (void)dropdown;
    (void)pspec;
    update_settings_previews(user_data);
}

static GtkWidget *build_settings_popover(TioGui *gui)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_size_request(root, 360, -1);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    gui->settings_title_label = GTK_LABEL(make_label(_("Settings")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->settings_title_label), "settings-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->settings_title_label));

    gui->appearance_section_label = GTK_LABEL(make_label(_("Appearance")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->appearance_section_label),
                             "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->appearance_section_label));

    GtkWidget *appearance_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(appearance_card, "settings-card");

    GtkWidget *theme_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gui->theme_label = GTK_LABEL(make_label(_("Theme")));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->theme_label), TRUE);
    const char *theme_names[] = {_("Follow system"), _("Light"), _("Dark"), NULL};
    gui->theme_dropdown = make_string_dropdown(theme_names,
                                                value_index(theme_values,
                                                            gui->settings.theme,
                                                            0));
    gtk_box_append(GTK_BOX(theme_row), GTK_WIDGET(gui->theme_label));
    gtk_box_append(GTK_BOX(theme_row), GTK_WIDGET(gui->theme_dropdown));
    gtk_box_append(GTK_BOX(appearance_card), theme_row);

    GtkWidget *language_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gui->language_label = GTK_LABEL(make_label(_("Language")));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->language_label), TRUE);
    const char *language_names[] = {
        _("System default"), "简体中文", "繁體中文", "English", "日本語", "Deutsch", NULL,
    };
    gui->language_dropdown = make_string_dropdown(
        language_names, value_index(language_values, gui->settings.language, 0));
    gtk_box_append(GTK_BOX(language_row), GTK_WIDGET(gui->language_label));
    gtk_box_append(GTK_BOX(language_row), GTK_WIDGET(gui->language_dropdown));
    gtk_box_append(GTK_BOX(appearance_card), language_row);
    gtk_box_append(GTK_BOX(root), appearance_card);

    gui->session_section_label = GTK_LABEL(make_label(_("Session")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->session_section_label), "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->session_section_label));

    GtkWidget *session_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(session_card, "settings-card");

    GtkWidget *timestamp_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gui->timestamp_format_label = GTK_LABEL(make_label(_("Timestamp format")));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->timestamp_format_label), TRUE);
    const char *timestamp_names[] = {
        _("24-hour"), _("Since start"), _("Since previous"), _("ISO 8601"),
        _("Unix epoch"), NULL,
    };
    gui->timestamp_format_dropdown = make_string_dropdown(
        timestamp_names,
        value_index(timestamp_format_values, gui->settings.session.timestamp_format, 3));
    gtk_box_append(GTK_BOX(timestamp_row), GTK_WIDGET(gui->timestamp_format_label));
    gtk_box_append(GTK_BOX(timestamp_row), GTK_WIDGET(gui->timestamp_format_dropdown));
    gtk_box_append(GTK_BOX(session_card), timestamp_row);
    gui->timestamp_format_hint =
        GTK_LABEL(make_label(_("Applied when line timestamps are enabled")));
    gtk_label_set_wrap(gui->timestamp_format_hint, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(gui->timestamp_format_hint), "settings-hint");
    gtk_box_append(GTK_BOX(session_card), GTK_WIDGET(gui->timestamp_format_hint));

    gui->timestamp_preview_label = GTK_LABEL(make_label(""));
    gtk_widget_add_css_class(GTK_WIDGET(gui->timestamp_preview_label), "settings-hint");
    gtk_box_append(GTK_BOX(session_card), GTK_WIDGET(gui->timestamp_preview_label));

    gtk_box_append(GTK_BOX(session_card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *filename_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gui->log_filename_format_label = GTK_LABEL(make_label(_("Log filename")));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->log_filename_format_label), TRUE);
    const char *filename_names[] = {
        _("tio-gui default"), _("Date first"), _("Device first"), _("Custom…"), NULL,
    };
    guint filename_index = gui->settings.session.log_file[0] == '\0'
                               ? 0
                               : value_index(log_filename_templates,
                                             gui->settings.session.log_file,
                                             TIO_GUI_CUSTOM_LOG_FILENAME_INDEX);
    gui->log_filename_dropdown = make_string_dropdown(filename_names, filename_index);
    gtk_box_append(GTK_BOX(filename_row), GTK_WIDGET(gui->log_filename_format_label));
    gtk_box_append(GTK_BOX(filename_row), GTK_WIDGET(gui->log_filename_dropdown));
    gtk_box_append(GTK_BOX(session_card), filename_row);
    gui->log_file_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(gui->log_file_entry,
                                   _("Example: {device}-{date}-{time}.log"));
    gtk_editable_set_text(GTK_EDITABLE(gui->log_file_entry),
                          filename_index == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX
                              ? gui->settings.session.log_file
                              : log_filename_templates[filename_index]);
    gtk_widget_set_visible(GTK_WIDGET(gui->log_file_entry),
                           filename_index == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX);
    gtk_box_append(GTK_BOX(session_card), GTK_WIDGET(gui->log_file_entry));
    gui->log_filename_preview_label = GTK_LABEL(make_label(""));
    gtk_widget_add_css_class(GTK_WIDGET(gui->log_filename_preview_label), "settings-hint");
    gtk_box_append(GTK_BOX(session_card), GTK_WIDGET(gui->log_filename_preview_label));
    gtk_box_append(GTK_BOX(root), session_card);

    gui->connection_section_label = GTK_LABEL(make_label(_("Connection and logging")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->connection_section_label),
                             "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->connection_section_label));
    gui->connection_settings_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_widget_add_css_class(GTK_WIDGET(gui->connection_settings_box), "settings-card");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->connection_settings_box));

    gui->backup_section_label = GTK_LABEL(make_label(_("Backup")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->backup_section_label), "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->backup_section_label));

    gui->export_settings_button = GTK_BUTTON(gtk_button_new_with_label(_("Export settings…")));
    gui->import_settings_button = GTK_BUTTON(gtk_button_new_with_label(_("Import settings…")));
    GtkWidget *backup_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(backup_card, "settings-card");
    gtk_widget_set_hexpand(GTK_WIDGET(gui->export_settings_button), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(gui->import_settings_button), TRUE);
    gtk_box_append(GTK_BOX(backup_card), GTK_WIDGET(gui->export_settings_button));
    gtk_box_append(GTK_BOX(backup_card), GTK_WIDGET(gui->import_settings_button));
    gtk_box_append(GTK_BOX(root), backup_card);

    gui->about_section_label = GTK_LABEL(make_label(_("About")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->about_section_label), "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->about_section_label));

    GtkWidget *about_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(about_card, "settings-card");
    GtkWidget *about_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    g_autofree gchar *version = g_strdup_printf(_("Version %s"), TIO_GUI_VERSION);
    gui->version_label = GTK_LABEL(gtk_label_new(version));
    gtk_widget_add_css_class(GTK_WIDGET(gui->version_label), "settings-version");
    gtk_label_set_xalign(gui->version_label, 0.0F);
    gtk_widget_set_hexpand(GTK_WIDGET(gui->version_label), TRUE);
    gui->github_button = GTK_BUTTON(gtk_button_new_with_label(_("GitHub project")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->github_button), "flat");
    gtk_box_append(GTK_BOX(about_header), GTK_WIDGET(gui->version_label));
    gtk_box_append(GTK_BOX(about_header), GTK_WIDGET(gui->github_button));
    gtk_box_append(GTK_BOX(about_card), about_header);
    gui->about_description_label =
        GTK_LABEL(make_label(_("A lightweight, reliable GUI for tio")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->about_description_label), "settings-hint");
    gtk_box_append(GTK_BOX(about_card), GTK_WIDGET(gui->about_description_label));
    GtkWidget *update_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gui->update_available_label = GTK_LABEL(make_label(""));
    gtk_widget_add_css_class(GTK_WIDGET(gui->update_available_label), "settings-hint");
    gtk_widget_set_hexpand(GTK_WIDGET(gui->update_available_label), TRUE);
    gui->download_update_button =
        GTK_BUTTON(gtk_button_new_with_label(_("Download update")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->download_update_button), "suggested-action");
    gtk_widget_set_visible(GTK_WIDGET(gui->update_available_label), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(gui->download_update_button), FALSE);
    gtk_box_append(GTK_BOX(update_row), GTK_WIDGET(gui->update_available_label));
    gtk_box_append(GTK_BOX(update_row), GTK_WIDGET(gui->download_update_button));
    gtk_box_append(GTK_BOX(about_card), update_row);
    gtk_box_append(GTK_BOX(root), about_card);

    g_signal_connect(gui->theme_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_theme_changed),
                     gui);
    g_signal_connect(gui->timestamp_format_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_timestamp_format_changed),
                     gui);
    g_signal_connect(gui->log_filename_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_log_filename_format_changed),
                     gui);
    g_signal_connect_swapped(gui->log_file_entry,
                             "changed",
                             G_CALLBACK(update_settings_previews),
                             gui);
    g_signal_connect(gui->language_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_language_changed),
                     gui);
    g_signal_connect(gui->github_button, "clicked", G_CALLBACK(on_github_clicked), gui);
    g_signal_connect(gui->download_update_button,
                     "clicked",
                     G_CALLBACK(on_download_update_clicked),
                     gui);
    g_signal_connect(gui->export_settings_button,
                     "clicked",
                     G_CALLBACK(on_export_settings_clicked),
                     gui);
    g_signal_connect(gui->import_settings_button,
                     "clicked",
                     G_CALLBACK(on_import_settings_clicked),
                     gui);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 560);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), root);
    return scroll;
}

static GtkWidget *build_profile_popover(TioGui *gui)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(root, 240, -1);

    gui->profile_empty_label = GTK_LABEL(make_label(_("No saved profiles")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->profile_empty_label), "dim-label");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->profile_empty_label));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gui->profile_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(gui->profile_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(gui->profile_list));
    gtk_box_append(GTK_BOX(root), scroll);

    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gui->profile_save_button = GTK_BUTTON(gtk_button_new_with_label(_("Save as new profile…")));
    gui->profile_update_button = GTK_BUTTON(gtk_button_new_with_label(_("Update this profile")));
    gui->profile_duplicate_button = GTK_BUTTON(gtk_button_new_with_label(_("Duplicate…")));
    gui->profile_delete_button = GTK_BUTTON(gtk_button_new_with_label(_("Delete profile")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->profile_delete_button), "destructive-action");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->profile_save_button));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->profile_update_button));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->profile_duplicate_button));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->profile_delete_button));

    g_signal_connect(gui->profile_list, "row-activated", G_CALLBACK(on_profile_row_activated), gui);
    g_signal_connect(gui->profile_save_button, "clicked", G_CALLBACK(on_profile_save_clicked), gui);
    g_signal_connect(gui->profile_update_button,
                     "clicked",
                     G_CALLBACK(on_profile_update_clicked),
                     gui);
    g_signal_connect(gui->profile_duplicate_button,
                     "clicked",
                     G_CALLBACK(on_profile_duplicate_clicked),
                     gui);
    g_signal_connect(gui->profile_delete_button,
                     "clicked",
                     G_CALLBACK(on_profile_delete_clicked),
                     gui);
    return root;
}

static GtkWidget *build_history_popover(TioGui *gui)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(root, 280, -1);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 8);
    gtk_widget_set_margin_start(root, 8);
    gtk_widget_set_margin_end(root, 8);

    gui->history_empty_label = GTK_LABEL(make_label(_("No history yet")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->history_empty_label), "dim-label");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->history_empty_label));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 260);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gui->history_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(gui->history_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(gui->history_list));
    gtk_box_append(GTK_BOX(root), scroll);

    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gui->history_clear_button = GTK_BUTTON(gtk_button_new_with_label(_("Clear history")));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(gui->history_clear_button));

    g_signal_connect(gui->history_list, "row-activated", G_CALLBACK(on_history_row_activated), gui);
    g_signal_connect(gui->history_clear_button,
                     "clicked",
                     G_CALLBACK(on_history_clear_clicked),
                     gui);
    return root;
}

static GtkWidget *build_search_bar(TioGui *gui)
{
    gui->search_bar = GTK_SEARCH_BAR(gtk_search_bar_new());
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(box, "compact-controls");

    gui->search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(gui->search_entry, _("Search terminal…"));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->search_entry), TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->search_entry));

    gui->search_previous_button =
        GTK_BUTTON(gtk_button_new_from_icon_name("go-up-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->search_previous_button), _("Find previous"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->search_previous_button));

    gui->search_next_button = GTK_BUTTON(gtk_button_new_from_icon_name("go-down-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->search_next_button), _("Find next"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->search_next_button));

    gui->search_case_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Aa"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->search_case_toggle), _("Match case"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->search_case_toggle));

    gui->search_regex_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(".*"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->search_regex_toggle), _("Regular expression"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(gui->search_regex_toggle));

    gtk_search_bar_set_child(gui->search_bar, box);
    gtk_search_bar_connect_entry(gui->search_bar, GTK_EDITABLE(gui->search_entry));
    gtk_search_bar_set_show_close_button(gui->search_bar, TRUE);

    g_signal_connect(gui->search_entry, "search-changed", G_CALLBACK(on_search_changed), gui);
    g_signal_connect(gui->search_entry, "activate", G_CALLBACK(on_search_activate), gui);
    g_signal_connect(gui->search_entry, "stop-search", G_CALLBACK(on_search_stopped), gui);
    g_signal_connect(gui->search_next_button, "clicked", G_CALLBACK(on_search_next), gui);
    g_signal_connect(gui->search_previous_button,
                     "clicked",
                     G_CALLBACK(on_search_previous),
                     gui);
    g_signal_connect(gui->search_case_toggle,
                     "toggled",
                     G_CALLBACK(on_search_option_toggled),
                     gui);
    g_signal_connect(gui->search_regex_toggle,
                     "toggled",
                     G_CALLBACK(on_search_option_toggled),
                     gui);
    return GTK_WIDGET(gui->search_bar);
}

static void activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;

    /* A second launch must reuse the running session instead of opening a
       window that cannot own the same serial device. */
    GtkWidget *existing = g_object_get_data(G_OBJECT(application), "tio-gui-window");
    if (existing != NULL) {
        gtk_window_present(GTK_WINDOW(existing));
        return;
    }

    TioGui *gui = g_new0(TioGui, 1);
    gui->child_pid = -1;
    gui->follow_output = TRUE;
    tio_settings_init(&gui->settings);
    tio_settings_load(&gui->settings);
    const char *development_language = g_getenv("TIO_GUI_LANGUAGE");
    if (development_language != NULL && development_language[0] != '\0') {
        g_free(gui->settings.language);
        gui->settings.language = g_strdup(development_language);
    }
    apply_theme(gui->settings.theme);

    install_css();

    gui->window = gtk_application_window_new(application);
    g_object_set_data_full(G_OBJECT(gui->window), "tio-gui", gui, tio_gui_free);
    g_object_set_data(G_OBJECT(application), "tio-gui-window", gui->window);
    gtk_window_set_title(GTK_WINDOW(gui->window),
                         g_getenv("TIO_GUI_NON_UNIQUE") != NULL
                             ? "tio-gui — Preview"
                             : "tio-gui");
    gtk_window_set_default_size(GTK_WINDOW(gui->window), 1000, 660);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 8);
    gtk_widget_set_margin_start(root, 8);
    gtk_widget_set_margin_end(root, 8);
    gtk_window_set_child(GTK_WINDOW(gui->window), root);

    /* Toolbar. */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(toolbar, "compact-controls");
    gtk_box_append(GTK_BOX(root), toolbar);

    gui->settings_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(gui->settings_button, "emblem-system-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->settings_button), _("Application settings"));
    gui->settings_popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_position(gui->settings_popover, GTK_POS_BOTTOM);
    gtk_widget_set_halign(GTK_WIDGET(gui->settings_popover), GTK_ALIGN_START);
    gtk_popover_set_offset(gui->settings_popover, 12, 0);
    gtk_popover_set_child(gui->settings_popover, build_settings_popover(gui));
    g_signal_connect(gui->settings_popover,
                     "notify::visible",
                     G_CALLBACK(on_settings_popover_visible),
                     gui);
    gtk_menu_button_set_popover(gui->settings_button, GTK_WIDGET(gui->settings_popover));
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->settings_button));

    gui->profile_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_label(gui->profile_button, _("Profiles"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->profile_button), _("Connection profiles"));
    gui->profile_popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_position(gui->profile_popover, GTK_POS_BOTTOM);
    gtk_widget_set_halign(GTK_WIDGET(gui->profile_popover), GTK_ALIGN_START);
    gtk_popover_set_offset(gui->profile_popover, 8, 0);
    gtk_popover_set_child(gui->profile_popover, build_profile_popover(gui));
    gtk_menu_button_set_popover(gui->profile_button, GTK_WIDGET(gui->profile_popover));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->profile_button), TRUE);
    gtk_box_append(gui->connection_settings_box, GTK_WIDGET(gui->profile_button));

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
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->baud_dropdown));
    gui->custom_baud_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(gui->custom_baud_entry, _("Custom baud"));
    gtk_entry_set_input_purpose(gui->custom_baud_entry, GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_max_length(gui->custom_baud_entry, 10);
    gtk_editable_set_width_chars(GTK_EDITABLE(gui->custom_baud_entry), 10);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->custom_baud_entry));

    gui->connect_button = GTK_BUTTON(gtk_button_new_with_label(_("Connect")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->connect_button), "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(gui->connect_button));

    /* Keep only frequently used switches on the main workspace. */
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(options, "compact-controls");

    gui->timestamp_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Timestamps")));
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(gui->timestamp_check));

    gui->log_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Log session")));
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(gui->log_check));
    gtk_box_append(GTK_BOX(root), options);

    gui->log_directory_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(gui->log_directory_entry, _("Log directory"));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->log_directory_entry), TRUE);
    gui->choose_log_directory_button =
        GTK_BUTTON(gtk_button_new_from_icon_name("folder-open-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->choose_log_directory_button),
                                _("Choose log directory"));
    GtkWidget *advanced_box = GTK_WIDGET(gui->connection_settings_box);
    gtk_widget_add_css_class(advanced_box, "compact-controls");

    gui->data_bits_dropdown = make_string_dropdown(data_bits_values, 3);
    gui->stop_bits_dropdown = make_string_dropdown(stop_bits_values, 0);
    gui->parity_dropdown = make_string_dropdown(parity_values, 0);
    gui->flow_dropdown = make_string_dropdown(flow_values, 0);
    gui->local_echo_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Local echo")));
    gui->show_all_ttys_check =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Show all TTY devices")));
    gui->output_delay_spin = make_spin_button(0, 10000.0, 1.0);
    gui->output_line_delay_spin = make_spin_button(0, 10000.0, 1.0);
    gui->log_warning_spin = make_spin_button(gui->settings.log_warning_mb, 65536.0, 16.0);
    gui->log_append_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Append")));
    gui->log_strip_check =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Strip control characters")));
    gui->open_log_directory_button = GTK_BUTTON(gtk_button_new_with_label(_("Open folder")));

    gtk_check_button_set_active(gui->show_all_ttys_check, gui->settings.show_all_ttys);

    gui->data_bits_label = GTK_LABEL(make_label(_("Data bits")));
    gui->stop_bits_label = GTK_LABEL(make_label(_("Stop bits")));
    gui->parity_label = GTK_LABEL(make_label(_("Parity")));
    gui->flow_label = GTK_LABEL(make_label(_("Flow control")));
    gui->output_delay_label = GTK_LABEL(make_label(_("Character delay (ms)")));
    gui->output_line_delay_label = GTK_LABEL(make_label(_("Line delay (ms)")));
    gui->log_warning_label = GTK_LABEL(make_label(_("Warn above (MB)")));

    GtkWidget *framing_row = make_settings_row(advanced_box);
    append_labelled(framing_row, gui->data_bits_label, GTK_WIDGET(gui->data_bits_dropdown));
    append_labelled(framing_row, gui->stop_bits_label, GTK_WIDGET(gui->stop_bits_dropdown));

    GtkWidget *protocol_row = make_settings_row(advanced_box);
    append_labelled(protocol_row, gui->parity_label, GTK_WIDGET(gui->parity_dropdown));
    append_labelled(protocol_row, gui->flow_label, GTK_WIDGET(gui->flow_dropdown));

    GtkWidget *behaviour_row = make_settings_row(advanced_box);
    gtk_box_append(GTK_BOX(behaviour_row), GTK_WIDGET(gui->local_echo_check));
    gtk_box_append(GTK_BOX(behaviour_row), GTK_WIDGET(gui->show_all_ttys_check));

    GtkWidget *delay_row = make_settings_row(advanced_box);
    append_labelled(delay_row, gui->output_delay_label, GTK_WIDGET(gui->output_delay_spin));
    append_labelled(delay_row,
                    gui->output_line_delay_label,
                    GTK_WIDGET(gui->output_line_delay_spin));

    GtkWidget *log_directory_row = make_settings_row(advanced_box);
    gtk_box_append(GTK_BOX(log_directory_row), GTK_WIDGET(gui->log_directory_entry));
    gtk_box_append(GTK_BOX(log_directory_row), GTK_WIDGET(gui->choose_log_directory_button));

    GtkWidget *log_option_row = make_settings_row(advanced_box);
    gtk_box_append(GTK_BOX(log_option_row), GTK_WIDGET(gui->log_append_check));
    gtk_box_append(GTK_BOX(log_option_row), GTK_WIDGET(gui->log_strip_check));

    GtkWidget *log_limit_row = make_settings_row(advanced_box);
    append_labelled(log_limit_row, gui->log_warning_label, GTK_WIDGET(gui->log_warning_spin));
    gtk_box_append(GTK_BOX(log_limit_row), GTK_WIDGET(gui->open_log_directory_button));

    /* Status bar. */
    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(status_bar, "compact-controls");
    gui->status_label = GTK_LABEL(gtk_label_new(_("Ready")));
    gtk_label_set_xalign(gui->status_label, 0.0F);
    gtk_label_set_ellipsize(gui->status_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(gui->status_label), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(gui->status_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(gui->status_label), "session-status");
    gtk_box_append(GTK_BOX(status_bar), GTK_WIDGET(gui->status_label));

    gui->session_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(gui->session_label, 1.0F);
    gtk_widget_add_css_class(GTK_WIDGET(gui->session_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(gui->session_label), "session-status");
    gtk_box_append(GTK_BOX(status_bar), GTK_WIDGET(gui->session_label));

    gui->clear_terminal_button = GTK_BUTTON(gtk_button_new_with_label(_("Clear")));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->clear_terminal_button),
                                _("Clear the terminal and its scrollback history"));
    gtk_box_append(GTK_BOX(status_bar), GTK_WIDGET(gui->clear_terminal_button));
    gtk_box_append(GTK_BOX(root), status_bar);

    gtk_box_append(GTK_BOX(root), build_search_bar(gui));

    /* Terminal. */
    gui->terminal = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scrollback_lines(gui->terminal, 10000);
    vte_terminal_set_mouse_autohide(gui->terminal, TRUE);
    vte_terminal_set_scroll_on_output(gui->terminal, FALSE);
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

    GtkWidget *terminal_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(terminal_overlay), terminal_scroll);
    gui->scroll_bottom_button =
        GTK_BUTTON(gtk_button_new_with_label(_("Back to bottom ↓")));
    gtk_widget_add_css_class(GTK_WIDGET(gui->scroll_bottom_button), "scroll-bottom");
    gtk_widget_add_css_class(GTK_WIDGET(gui->scroll_bottom_button), "suggested-action");
    gtk_widget_set_halign(GTK_WIDGET(gui->scroll_bottom_button), GTK_ALIGN_END);
    gtk_widget_set_valign(GTK_WIDGET(gui->scroll_bottom_button), GTK_ALIGN_END);
    gtk_widget_set_visible(GTK_WIDGET(gui->scroll_bottom_button), FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(terminal_overlay),
                            GTK_WIDGET(gui->scroll_bottom_button));

    gtk_frame_set_child(GTK_FRAME(terminal_frame), terminal_overlay);
    gtk_widget_set_hexpand(terminal_frame, TRUE);
    gtk_widget_set_vexpand(terminal_frame, TRUE);
    gtk_box_append(GTK_BOX(root), terminal_frame);

    /* Send bar. */
    GtkWidget *send_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(send_bar, "compact-controls");

    gui->history_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(gui->history_button, "document-open-recent-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->history_button), _("Send history"));
    gui->history_popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_position(gui->history_popover, GTK_POS_TOP);
    gtk_widget_set_halign(GTK_WIDGET(gui->history_popover), GTK_ALIGN_START);
    gtk_popover_set_offset(gui->history_popover, 8, 0);
    gtk_popover_set_child(gui->history_popover, build_history_popover(gui));
    gtk_menu_button_set_popover(gui->history_button, GTK_WIDGET(gui->history_popover));
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(gui->history_button));

    gui->send_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(gui->send_entry, _("Send text…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->send_entry),
                                _("Press Up and Down to browse the send history"));
    gtk_widget_set_hexpand(GTK_WIDGET(gui->send_entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_entry), FALSE);
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(gui->send_entry));

    const char *line_ending_names[] = {
        _("No line ending"), "LF (\\n)", "CR (\\r)", "CR+LF (\\r\\n)", NULL,
    };
    gui->line_ending_dropdown = make_string_dropdown(line_ending_names, 2);
    gtk_widget_set_tooltip_text(GTK_WIDGET(gui->line_ending_dropdown), _("Line ending"));
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(gui->line_ending_dropdown));

    gui->send_button = GTK_BUTTON(gtk_button_new_with_label(_("Send")));
    gtk_widget_set_sensitive(GTK_WIDGET(gui->send_button), FALSE);
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(gui->send_button));
    gtk_box_append(GTK_BOX(root), send_bar);

    /* Quick buttons. */
    GtkWidget *quick_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(quick_bar, "compact-controls");
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        gui->quick_buttons[index] = GTK_BUTTON(gtk_button_new_with_label(""));
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
    gtk_box_append(GTK_BOX(quick_bar), GTK_WIDGET(gui->hex_toggle));
    gtk_box_append(GTK_BOX(root), quick_bar);

    /* Signals. */
    g_signal_connect(gui->refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), gui);
    g_signal_connect(gui->baud_dropdown, "notify::selected", G_CALLBACK(on_baud_changed), gui);
    g_signal_connect_swapped(gui->custom_baud_entry,
                             "changed",
                             G_CALLBACK(update_session_label),
                             gui);
    g_signal_connect(gui->device_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     gui);
    g_signal_connect(gui->data_bits_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     gui);
    g_signal_connect(gui->stop_bits_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     gui);
    g_signal_connect(gui->parity_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     gui);
    g_signal_connect(gui->flow_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     gui);
    g_signal_connect(gui->clear_terminal_button,
                     "clicked",
                     G_CALLBACK(on_clear_terminal_clicked),
                     gui);
    g_signal_connect(gui->connect_button, "clicked", G_CALLBACK(on_connect_clicked), gui);
    g_signal_connect(gui->log_check, "toggled", G_CALLBACK(on_log_toggled), gui);
    g_signal_connect(gui->open_log_directory_button,
                     "clicked",
                     G_CALLBACK(on_open_log_directory),
                     gui);
    g_signal_connect(gui->choose_log_directory_button,
                     "clicked",
                     G_CALLBACK(on_choose_log_directory),
                     gui);
    g_signal_connect(gui->show_all_ttys_check,
                     "toggled",
                     G_CALLBACK(on_show_all_ttys_toggled),
                     gui);
    g_signal_connect(gui->terminal, "child-exited", G_CALLBACK(on_child_exited), gui);
    g_signal_connect(gui->send_button, "clicked", G_CALLBACK(on_send_clicked), gui);
    g_signal_connect(gui->send_entry, "activate", G_CALLBACK(on_send_activate), gui);
    g_signal_connect(gui->send_entry, "changed", G_CALLBACK(on_send_entry_changed), gui);
    g_signal_connect(gui->customize_quick_buttons,
                     "clicked",
                     G_CALLBACK(on_customize_quick_buttons),
                     gui);
    g_signal_connect(gui->scroll_bottom_button,
                     "clicked",
                     G_CALLBACK(on_scroll_bottom_clicked),
                     gui);
    g_signal_connect(gui->window,
                     "close-request",
                     G_CALLBACK(on_window_close_request),
                     gui);
    g_signal_connect(gui->window, "destroy", G_CALLBACK(on_window_destroy), application);

    GtkEventControllerKey *send_keys = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
    g_signal_connect(send_keys, "key-pressed", G_CALLBACK(on_send_entry_key), gui);
    gtk_widget_add_controller(GTK_WIDGET(gui->send_entry),
                              GTK_EVENT_CONTROLLER(send_keys));

    GtkAdjustment *adjustment = terminal_adjustment(gui);
    if (adjustment != NULL) {
        g_signal_connect(adjustment, "value-changed", G_CALLBACK(on_terminal_scrolled), gui);
        g_signal_connect(adjustment, "changed", G_CALLBACK(on_terminal_content_changed), gui);
    }

    install_shortcuts(application, gui);

    apply_session_config(gui, &gui->settings.session);
    update_quick_buttons(gui);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->log_directory_entry),
                             gui->settings.session.logging);
    gtk_widget_set_sensitive(GTK_WIDGET(gui->choose_log_directory_button),
                             gui->settings.session.logging);
    refresh_profile_ui(gui);
    refresh_history_ui(gui);
    refresh_devices(gui);
    gui->preview_started_at = g_get_monotonic_time();
    update_settings_previews(gui);
    gui->settings_preview_timer = g_timeout_add(250, update_settings_previews, gui);
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
