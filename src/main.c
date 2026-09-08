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
#include <sys/un.h>
#include <sys/utsname.h>

#include <pcre2.h>

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <vte/vte.h>

#include "settings.h"
#include "payload.h"
#include "quick_presets.h"
#include "sequence.h"
#include "serial_options.h"
#include "connection_state.h"
#include "analyzer.h"
#include "capture.h"
#include "transfer.h"
#include "highlighter.h"

#define _(message) gettext(message)

#define TIO_GUI_HISTORY_MENU_LIMIT 25

typedef struct _TioApp TioApp;
typedef struct _TioTab TioTab;
typedef struct _TioRawTap TioRawTap;

/* One serial session: its own tio child process, terminal, controls and
   configuration. Only the window chrome around it is shared. */
struct _TioTab {
    TioApp *app;

    /* The page this session occupies, and the text on its tab. */
    GtkWidget *content;
    GtkLabel *tab_label;

    /* The settings this session is actually running with. A new session
       starts as a copy of TioApp.settings.defaults and may then diverge. */
    TioSessionConfig config;

    GtkDropDown *device_dropdown;
    GtkDropDown *baud_dropdown;
    GtkEntry *custom_baud_entry;
    GtkWidget *refresh_button;
    GtkButton *connect_button;
    GtkLabel *device_label;
    GtkLabel *baud_label;

    /* Advanced settings. */
    GtkCheckButton *timestamp_check;
    GtkCheckButton *log_check;
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
    GtkStack *terminal_stack;
    GtkTextView *highlight_view;
    TioHighlighter *highlighter;
    GtkCheckButton *highlight_toggle;
    GtkButton *scroll_bottom_button;
    gboolean follow_output;
    gboolean pending_output;
    gboolean highlight_follow;
    gboolean highlight_pending;
    gboolean highlight_adjusting;
    gboolean highlight_pointer_down;
    gboolean highlight_selected;

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

    /* Raw data tap: the bytes tio received, before any display formatting. */
    gchar *socket_path;
    guint64 id;
    TioTransfer *transfer;
    guint transfer_timer;
    GtkDropDown *transfer_protocol;
    GtkSpinButton *transfer_timeout;
    GtkLabel *transfer_status;
    TioCapture *capture;
    GtkSpinButton *capture_part_spin, *capture_time_spin, *capture_keep_spin, *capture_disk_spin;
    GtkButton *capture_button;
    GtkLabel *capture_label;
    guint capture_timer, deferred_close_timer;
    gboolean spawn_pending, close_requested;
    gchar *running_metadata;
    TioLogModel *log_model;
    GtkWidget *analyzer_window;
    GtkWidget *sequence_window;
    TioSequenceRunner *sequence_runner;
    gboolean sequence_paused;
    guint quick_send_timer;
    GByteArray *quick_pending;
    TioRawTap *raw;
    guint raw_connect_timer;
    guint raw_connect_attempts;
    guint64 rx_bytes;
    guint64 rx_lines;
    guint64 rx_bytes_at_tick;
    guint64 rx_rate;

    /* Per-connection controls. These belong to the session, not the
       window: each tab configures its own link and its own logging. */
    GtkWidget *session_settings_card;
    GtkWidget *session_settings_expander;
    GtkLabel *session_section_label;
    GtkLabel *connection_section_label;
    GtkBox *connection_settings_box;
    GtkLabel *timestamp_format_label;
    GtkLabel *timestamp_format_hint;
    GtkLabel *timestamp_preview_label;
    GtkDropDown *log_filename_dropdown;
    GtkLabel *log_filename_format_label;
    GtkLabel *log_filename_preview_label;
    GtkMenuButton *profile_button;
    GtkPopover *profile_popover;
    GtkListBox *profile_list;
    GtkLabel *profile_empty_label;
    GtkButton *profile_save_button;
    GtkButton *profile_update_button;
    GtkButton *profile_duplicate_button;
    GtkButton *profile_delete_button;
    GtkDropDown *data_bits_dropdown;
    GtkDropDown *stop_bits_dropdown;
    GtkDropDown *parity_dropdown;
    GtkDropDown *flow_dropdown;
    GtkLabel *data_bits_label;
    GtkLabel *stop_bits_label;
    GtkLabel *parity_label;
    GtkLabel *flow_label;
    GtkLabel *output_delay_label;
    GtkLabel *output_line_delay_label;
    GtkCheckButton *reconnect_check, *connection_notify_check, *connection_sound_check;
    GtkDropDown *auto_connect_dropdown;
    GtkEntry *exclude_devices_entry, *exclude_drivers_entry, *exclude_tids_entry;
    gboolean observed_connected, ever_connected, connection_checked;
    guint reconnect_count;
    gchar *observed_device, *disconnect_reason;
    GtkDropDown *dtr_default, *rts_default;
    GtkSpinButton *line_pulse_spin;
    GtkCheckButton *rs485_check;
    GtkEntry *rs485_entry;
    guint line_command_timer;
    guint line_command_attempts;
    gchar *line_command_path;
    GPtrArray *line_script_paths;
    GtkSpinButton *output_delay_spin;
    GtkSpinButton *output_line_delay_spin;
    GtkCheckButton *local_echo_check;
    GtkDropDown *timestamp_format_dropdown;
    GtkCheckButton *log_append_check;
    GtkCheckButton *log_strip_check;
    GtkEntry *log_directory_entry;
    GtkButton *choose_log_directory_button;
    GtkEntry *log_file_entry;
    GtkButton *open_log_directory_button;
    gchar *pending_profile_delete;
};

/* The window and everything there is exactly one of, no matter how many
   sessions are open. */
struct _TioApp {
    GtkWidget *window;

    /* Application menu. */
    GtkMenuButton *settings_button;
    GtkPopover *settings_popover;
    GtkDropDown *theme_dropdown;
    GtkLabel *settings_title_label;
    GtkLabel *appearance_section_label;
    GtkLabel *backup_section_label;
    GtkLabel *about_section_label;
    GtkLabel *theme_label;
    GtkLabel *about_description_label;
    GtkLabel *update_available_label;
    GtkButton *download_update_button;
    GtkLabel *version_label;
    GtkButton *github_button;
    GtkButton *export_settings_button;
    GtkButton *import_settings_button;

    /* Global settings, in the popover. */
    GtkLabel *general_section_label;
    GtkBox *general_settings_box;
    GtkDropDown *language_dropdown;
    GtkLabel *language_label;
    GtkLabel *log_warning_label;
    GtkSpinButton *log_warning_spin;
    GtkCheckButton *show_all_ttys_check;
    GtkCheckButton *restore_tabs_check;

    /* Status. */
    gboolean retranslating;
    gboolean update_check_started;
    gchar *update_url;
    gchar *latest_version;

    /* Quick buttons. */
    guint settings_preview_timer;
    gint64 preview_started_at;
    TioSettings settings;

    guint close_capture_timer;
    GtkButton *new_tab_button;
    GtkNotebook *notebook;
    GPtrArray *tabs; /* TioTab *, in page order */

    /* The session the window is currently showing. */
    TioTab *active;
};

typedef struct {
    GtkWidget *window;
    SoupMessage *message;
} UpdateCheck;

typedef struct {
    TioTab *tab;
    GtkWidget *window;
    GtkEntry *label_entries[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkEntry *payload_entries[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkSpinButton *delays[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkDropDown *modes[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkDropDown *endings[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkDropDown *crcs[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkLabel *previews[TIO_GUI_QUICK_BUTTON_COUNT];
    GtkWidget *save;
    GtkLabel *file_status;
    GtkWidget *export_button;
} QuickButtonEditor;

typedef enum {
    TIO_GUI_PROFILE_SAVE_NEW,
    TIO_GUI_PROFILE_SAVE_DUPLICATE,
} TioProfileSaveMode;

typedef struct {
    TioTab *tab;
    GtkWidget *window;
    GtkEntry *name_entry;
    GtkLabel *hint_label;
} ProfileNameDialog;

static void update_quick_buttons(TioTab *tab);
static GtkWidget *make_label(const char *text);
static void set_status(TioTab *tab, const char *message);
static void update_session_label(TioTab *tab);
static void update_tab_label(TioTab *tab);
static void action_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void action_close_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void action_next_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void action_previous_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void raw_tap_start(TioTab *tab);
static void raw_tap_stop(TioTab *tab);
static void update_scroll_button(TioTab *tab);
static void scroll_highlight_to_bottom(TioTab *tab);
static void focus_log_view(TioTab *tab)
{
    gtk_widget_grab_focus(gtk_check_button_get_active(tab->highlight_toggle)
                             ? GTK_WIDGET(tab->highlight_view) : GTK_WIDGET(tab->terminal));
}
static void refresh_profile_ui(TioTab *tab);
static void refresh_history_ui(TioTab *tab);
static void capture_session_config(TioTab *tab, TioSessionConfig *config);
static void apply_session_config(TioTab *tab, const TioSessionConfig *config);
static void refresh_devices(TioTab *tab);
static void update_search_regex(TioTab *tab);
static void on_language_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data);
static void capture_all_settings(TioTab *tab);
static gboolean update_settings_previews(gpointer user_data);
static gboolean tick_settings_previews(gpointer user_data);

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

static void retranslate_ui(TioTab *tab)
{
    /* Re-splicing translated dropdown rows re-emits notify::selected. */
    tab->app->retranslating = TRUE;
    gtk_label_set_text(tab->device_label, _("Device"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->app->settings_button), _("Application settings"));
    gtk_label_set_text(tab->app->settings_title_label, _("Settings"));
    gtk_label_set_text(tab->app->appearance_section_label, _("Appearance"));
    gtk_label_set_text(tab->app->general_section_label, _("General"));
    gtk_label_set_text(tab->session_section_label, _("Session"));
    gtk_label_set_text(tab->connection_section_label, _("Connection and logging"));
    gtk_label_set_text(tab->app->backup_section_label, _("Backup"));
    gtk_label_set_text(tab->app->about_section_label, _("About"));
    gtk_label_set_text(tab->app->theme_label, _("Theme"));
    replace_dropdown_row(tab->app->theme_dropdown, 0, _("Follow system"));
    replace_dropdown_row(tab->app->theme_dropdown, 1, _("Light"));
    replace_dropdown_row(tab->app->theme_dropdown, 2, _("Dark"));
    gtk_label_set_text(tab->timestamp_format_label, _("Timestamp format"));
    gtk_label_set_text(tab->timestamp_format_hint,
                       _("Applied when line timestamps are enabled"));
    gtk_label_set_text(tab->log_filename_format_label, _("Log filename"));
    replace_dropdown_row(tab->log_filename_dropdown, 0, _("tio-gui default"));
    replace_dropdown_row(tab->log_filename_dropdown, 1, _("Date first"));
    replace_dropdown_row(tab->log_filename_dropdown, 2, _("Device first"));
    replace_dropdown_row(tab->log_filename_dropdown, 3, _("Custom…"));
    replace_dropdown_row(tab->timestamp_format_dropdown, 0, _("24-hour"));
    replace_dropdown_row(tab->timestamp_format_dropdown, 1, _("Since start"));
    replace_dropdown_row(tab->timestamp_format_dropdown, 2, _("Since previous"));
    replace_dropdown_row(tab->timestamp_format_dropdown, 3, _("ISO 8601"));
    replace_dropdown_row(tab->timestamp_format_dropdown, 4, _("Unix epoch"));
    gtk_button_set_label(tab->app->github_button, _("GitHub project"));
    gtk_button_set_label(tab->app->export_settings_button, _("Export settings…"));
    gtk_button_set_label(tab->app->import_settings_button, _("Import settings…"));
    gtk_label_set_text(tab->app->about_description_label,
                       _("A lightweight, reliable GUI for tio"));
    if (tab->app->latest_version != NULL) {
        g_autofree gchar *update =
            g_strdup_printf(_("Version %s is available"), tab->app->latest_version);
        gtk_label_set_text(tab->app->update_available_label, update);
    }
    gtk_button_set_label(tab->app->download_update_button, _("Download update"));
    g_autofree gchar *version = g_strdup_printf(_("Version %s"), TIO_GUI_VERSION);
    gtk_label_set_text(tab->app->version_label, version);
    gtk_label_set_text(tab->baud_label, _("Baud"));
    replace_dropdown_row(tab->baud_dropdown, TIO_GUI_CUSTOM_BAUD_INDEX, _("Custom…"));
    replace_dropdown_row(tab->app->language_dropdown, 0, _("System default"));
    gtk_entry_set_placeholder_text(tab->custom_baud_entry, _("Custom baud"));
    gtk_widget_set_tooltip_text(tab->refresh_button, _("Refresh serial devices"));
    gtk_button_set_label(tab->connect_button,
                         tab->child_pid > 0 ? _("Disconnect") : _("Connect"));
    gtk_check_button_set_label(tab->timestamp_check, _("Timestamps"));
    gtk_check_button_set_label(tab->log_check, _("Log session"));
    gtk_check_button_set_label(tab->highlight_toggle, _("Highlight view"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->highlight_toggle),
                                _("Show a semantic view of the raw serial log"));
    gtk_entry_set_placeholder_text(tab->log_directory_entry, _("Log directory"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->choose_log_directory_button),
                                _("Choose log directory"));
    gtk_label_set_text(tab->data_bits_label, _("Data bits"));
    gtk_label_set_text(tab->stop_bits_label, _("Stop bits"));
    gtk_label_set_text(tab->parity_label, _("Parity"));
    gtk_label_set_text(tab->flow_label, _("Flow control"));
    gtk_check_button_set_label(tab->local_echo_check, _("Local echo"));
    gtk_check_button_set_label(tab->app->show_all_ttys_check, _("Show all TTY devices"));
    gtk_label_set_text(tab->app->language_label, _("Language"));

    gtk_label_set_text(tab->output_delay_label, _("Character delay (ms)"));
    gtk_label_set_text(tab->output_line_delay_label, _("Line delay (ms)"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->output_delay_spin),
                                _("tio --output-delay: pause between sent characters"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->output_line_delay_spin),
                                _("tio --output-line-delay: pause between sent lines"));
    gtk_label_set_text(tab->app->log_warning_label, _("Warn above (MB)"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->app->log_warning_spin),
                                _("Warn once the current log file passes this size; 0 disables it"));
    gtk_entry_set_placeholder_text(tab->log_file_entry,
                                   _("Example: {device}-{date}-{time}.log"));
    gtk_check_button_set_label(tab->log_append_check, _("Append"));
    gtk_check_button_set_label(tab->log_strip_check, _("Strip control characters"));
    gtk_button_set_label(tab->open_log_directory_button, _("Open folder"));

    gtk_button_set_label(tab->clear_terminal_button, _("Clear"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->clear_terminal_button),
                                _("Clear the terminal and its scrollback history"));
    gtk_entry_set_placeholder_text(tab->send_entry, _("Send text…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->send_entry),
                                _("Press Up and Down to browse the send history"));
    gtk_button_set_label(tab->send_button, _("Send"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->history_button), _("Send history"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->line_ending_dropdown), _("Line ending"));
    gtk_button_set_label(tab->history_clear_button, _("Clear history"));
    gtk_label_set_text(tab->history_empty_label, _("No history yet"));
    gtk_button_set_label(tab->customize_quick_buttons, _("Customize…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->hex_toggle),
                                _("Display incoming bytes as 16-byte hex rows"));

    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->profile_button), _("Connection profiles"));
    gtk_menu_button_set_label(tab->profile_button, _("Profiles"));
    gtk_button_set_label(tab->profile_save_button, _("Save as new profile…"));
    gtk_button_set_label(tab->profile_update_button, _("Update this profile"));
    gtk_button_set_label(tab->profile_duplicate_button, _("Duplicate…"));
    gtk_button_set_label(tab->profile_delete_button, _("Delete profile"));
    gtk_label_set_text(tab->profile_empty_label, _("No saved profiles"));

    gtk_search_entry_set_placeholder_text(tab->search_entry, _("Search terminal…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->search_previous_button), _("Find previous"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->search_next_button), _("Find next"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->search_case_toggle), _("Match case"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->search_regex_toggle), _("Regular expression"));
    gtk_button_set_label(tab->scroll_bottom_button,
                         tab->pending_output ? _("New output ↓") : _("Back to bottom ↓"));

    set_status(tab, tab->child_pid > 0 ? _("Connected") : _("Ready"));
    refresh_profile_ui(tab);
    refresh_history_ui(tab);
    update_session_label(tab);
    update_settings_previews(tab);
    tab->app->retranslating = FALSE;
}

static void on_language_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    guint selected = gtk_drop_down_get_selected(dropdown);
    if (tab->app->retranslating || selected >= G_N_ELEMENTS(language_values) - 1 ||
        g_strcmp0(tab->app->settings.language, language_values[selected]) == 0) {
        return;
    }

    g_free(tab->app->settings.language);
    tab->app->settings.language = g_strdup(language_values[selected]);
    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&tab->app->settings, &error)) {
        g_warning("Could not save language setting: %s", error->message);
        return;
    }

    g_unsetenv("TIO_GUI_LANGUAGE");
    if (!apply_language(language_values[selected])) {
        g_warning("Locale is unavailable for language: %s", language_values[selected]);
        return;
    }
    retranslate_ui(tab);
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
    TioApp *app = user_data;
    TioTab *tab = app->active;
    guint selected = gtk_drop_down_get_selected(dropdown);
    if (tab->app->retranslating || selected >= G_N_ELEMENTS(theme_values) - 1) {
        return;
    }
    g_free(tab->app->settings.theme);
    tab->app->settings.theme = g_strdup(theme_values[selected]);
    apply_theme(tab->app->settings.theme);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&tab->app->settings, &error)) {
        g_warning("Could not save theme setting: %s", error->message);
    }
}

static void set_status(TioTab *tab, const char *message)
{
    gtk_label_set_text(tab->status_label, message);
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

/* A serial device belongs to one session at a time: tio takes a lock, so a
   second session on the same port fails with a message the user has to decode.
   Answer the question here instead. Uses the config captured at connect time,
   which is what the running tio was actually given. */
static TioTab *tab_holding_device(TioApp *app, const char *device, const TioTab *except)
{
    if (device == NULL || device[0] == '\0') {
        return NULL;
    }
    for (guint index = 0; index < app->tabs->len; ++index) {
        TioTab *other = g_ptr_array_index(app->tabs, index);
        if (other == except || other->child_pid <= 0) {
            continue;
        }
        if (g_strcmp0(other->config.device, device) == 0) {
            return other;
        }
    }
    return NULL;
}

static gboolean log_path_in_use(TioApp *app, const char *path, const TioTab *except)
{
    for (guint index = 0; index < app->tabs->len; ++index) {
        TioTab *other = g_ptr_array_index(app->tabs, index);
        if (other != except && g_strcmp0(other->log_path, path) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void on_device_item_setup(GtkSignalListItemFactory *factory,
                                 GObject *item,
                                 gpointer user_data)
{
    (void)factory;
    (void)user_data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_list_item_set_child(GTK_LIST_ITEM(item), label);
}

/* Marks the devices other sessions are holding. The row stays selectable:
   GtkDropDown has no per-item sensitivity, and connect_tio refuses anyway. */
static void on_device_item_bind(GtkSignalListItemFactory *factory,
                                GObject *item,
                                gpointer user_data)
{
    (void)factory;
    TioTab *tab = user_data;
    GtkStringObject *entry = gtk_list_item_get_item(GTK_LIST_ITEM(item));
    GtkWidget *label = gtk_list_item_get_child(GTK_LIST_ITEM(item));
    if (entry == NULL || label == NULL) {
        return;
    }
    const char *device = gtk_string_object_get_string(entry);
    if (gtk_drop_down_get_selected(tab->auto_connect_dropdown) == 0 &&
        tab_holding_device(tab->app, device, tab) != NULL) {
        g_autofree gchar *text = g_strdup_printf(_("%s · in use"), device);
        gtk_label_set_text(GTK_LABEL(label), text);
        gtk_widget_add_css_class(label, "dim-label");
    } else {
        gtk_label_set_text(GTK_LABEL(label), device);
        gtk_widget_remove_css_class(label, "dim-label");
    }
}

static void refresh_devices(TioTab *tab)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *devices = g_ptr_array_new_with_free_func(g_free);

    const char *const *patterns =
        gtk_check_button_get_active(tab->app->show_all_ttys_check)
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
    gtk_drop_down_set_model(tab->device_dropdown, G_LIST_MODEL(model));
    g_object_unref(model);

    if (devices->len > 0) {
        guint selected = 0;
        for (guint index = 0; index < devices->len; ++index) {
            if (g_strcmp0(g_ptr_array_index(devices, index), tab->config.device) == 0) {
                selected = index;
                break;
            }
        }
        gtk_drop_down_set_selected(tab->device_dropdown, selected);
        const char *format = ngettext("Found %u serial device",
                                     "Found %u serial devices",
                                     devices->len);
        g_autofree gchar *message = g_strdup_printf(format, devices->len);
        set_status(tab, message);
    } else {
        set_status(tab, _("No serial devices found"));
    }

    g_ptr_array_unref(devices);
    g_hash_table_unref(seen);
    update_session_label(tab);
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

static const char *selected_baud(TioTab *tab)
{
    if (gtk_drop_down_get_selected(tab->baud_dropdown) == TIO_GUI_CUSTOM_BAUD_INDEX) {
        return gtk_editable_get_text(GTK_EDITABLE(tab->custom_baud_entry));
    }
    return selected_string(tab->baud_dropdown);
}

static void on_baud_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    TioTab *tab = user_data;
    gboolean custom = gtk_drop_down_get_selected(dropdown) == TIO_GUI_CUSTOM_BAUD_INDEX;
    gtk_widget_set_visible(GTK_WIDGET(tab->custom_baud_entry), custom);
    if (custom) {
        gtk_widget_grab_focus(GTK_WIDGET(tab->custom_baud_entry));
    }
    update_session_label(tab);
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

static void select_baud(TioTab *tab, const char *baud)
{
    for (guint index = 0; baud_rates[index] != NULL; ++index) {
        if (g_strcmp0(baud_rates[index], baud) == 0) {
            gtk_drop_down_set_selected(tab->baud_dropdown, index);
            gtk_widget_set_visible(GTK_WIDGET(tab->custom_baud_entry), FALSE);
            return;
        }
    }
    gtk_editable_set_text(GTK_EDITABLE(tab->custom_baud_entry), baud != NULL ? baud : "");
    gtk_drop_down_set_selected(tab->baud_dropdown, TIO_GUI_CUSTOM_BAUD_INDEX);
    gtk_widget_set_visible(GTK_WIDGET(tab->custom_baud_entry), TRUE);
}

static void capture_session_config(TioTab *tab, TioSessionConfig *config)
{
    /* Preserve non-widget settings such as quick buttons in profile/tab snapshots. */
    if (config != &tab->config) tio_session_config_copy(config, &tab->config);
    const char *device = selected_string(tab->device_dropdown);
    if (device != NULL) {
        g_free(config->device);
        config->device = g_strdup(device);
        g_free(config->device_id);
        config->device_id = device_stable_id(device);
    }

    const char *baud = selected_baud(tab);
    if (baud_is_valid(baud)) {
        g_free(config->baud);
        config->baud = g_strdup(baud);
    }

    g_free(config->data_bits);
    config->data_bits = g_strdup(selected_string(tab->data_bits_dropdown));
    g_free(config->stop_bits);
    config->stop_bits = g_strdup(selected_string(tab->stop_bits_dropdown));
    g_free(config->parity);
    config->parity = g_strdup(selected_string(tab->parity_dropdown));
    g_free(config->flow);
    config->flow = g_strdup(selected_string(tab->flow_dropdown));
    g_free(config->line_ending);
    config->line_ending =
        g_strdup(line_ending_values[gtk_drop_down_get_selected(tab->line_ending_dropdown)]);
    g_free(config->log_directory);
    config->log_directory =
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(tab->log_directory_entry)));
    g_free(config->log_file);
    config->log_file = g_strdup(gtk_editable_get_text(GTK_EDITABLE(tab->log_file_entry)));

    config->capture_part_mb = (guint)gtk_spin_button_get_value_as_int(tab->capture_part_spin);
    config->capture_part_seconds = (guint)gtk_spin_button_get_value_as_int(tab->capture_time_spin);
    config->capture_keep_files = (guint)gtk_spin_button_get_value_as_int(tab->capture_keep_spin);
    config->capture_disk_mb = (guint)gtk_spin_button_get_value_as_int(tab->capture_disk_spin);
    config->reconnect = gtk_check_button_get_active(tab->reconnect_check);
    config->connection_notify = gtk_check_button_get_active(tab->connection_notify_check);
    config->connection_sound = gtk_check_button_get_active(tab->connection_sound_check);
    config->auto_connect = gtk_drop_down_get_selected(tab->auto_connect_dropdown);
    g_free(config->exclude_devices);
    config->exclude_devices = g_strdup(gtk_editable_get_text(GTK_EDITABLE(tab->exclude_devices_entry)));
    g_free(config->exclude_drivers);
    config->exclude_drivers = g_strdup(gtk_editable_get_text(GTK_EDITABLE(tab->exclude_drivers_entry)));
    g_free(config->exclude_tids);
    config->exclude_tids = g_strdup(gtk_editable_get_text(GTK_EDITABLE(tab->exclude_tids_entry)));
    config->dtr_default = gtk_drop_down_get_selected(tab->dtr_default);
    config->rts_default = gtk_drop_down_get_selected(tab->rts_default);
    config->line_pulse_ms = (guint)gtk_spin_button_get_value_as_int(tab->line_pulse_spin);
    config->rs485 = gtk_check_button_get_active(tab->rs485_check);
    g_free(config->rs485_config);
    config->rs485_config = g_strdup(gtk_editable_get_text(GTK_EDITABLE(tab->rs485_entry)));
    config->local_echo = gtk_check_button_get_active(tab->local_echo_check);
    config->hex_output = gtk_toggle_button_get_active(tab->hex_toggle);
    config->timestamps = gtk_check_button_get_active(tab->timestamp_check);
    g_free(config->timestamp_format);
    config->timestamp_format = g_strdup(
        timestamp_format_values[gtk_drop_down_get_selected(tab->timestamp_format_dropdown)]);
    config->logging = gtk_check_button_get_active(tab->log_check);
    config->log_append = gtk_check_button_get_active(tab->log_append_check);
    config->log_strip = gtk_check_button_get_active(tab->log_strip_check);
    config->output_delay = (guint)gtk_spin_button_get_value_as_int(tab->output_delay_spin);
    config->output_line_delay =
        (guint)gtk_spin_button_get_value_as_int(tab->output_line_delay_spin);
}

static void apply_session_config(TioTab *tab, const TioSessionConfig *config)
{
    g_autofree gchar *resolved = device_from_stable_id(config->device_id);
    const char *device = resolved != NULL ? resolved : config->device;
    if (device != NULL) {
        select_string(tab->device_dropdown, device);
    }

    select_baud(tab, config->baud);
    select_string(tab->data_bits_dropdown, config->data_bits);
    select_string(tab->stop_bits_dropdown, config->stop_bits);
    select_string(tab->parity_dropdown, config->parity);
    select_string(tab->flow_dropdown, config->flow);
    gtk_drop_down_set_selected(tab->line_ending_dropdown,
                               value_index(line_ending_values, config->line_ending, 2));
    gtk_check_button_set_active(tab->local_echo_check, config->local_echo);
    gtk_toggle_button_set_active(tab->hex_toggle, config->hex_output);
    gtk_check_button_set_active(tab->timestamp_check, config->timestamps);
    gtk_drop_down_set_selected(
        tab->timestamp_format_dropdown,
        value_index(timestamp_format_values, config->timestamp_format, 3));
    gtk_check_button_set_active(tab->log_check, config->logging);
    gtk_check_button_set_active(tab->log_append_check, config->log_append);
    gtk_check_button_set_active(tab->log_strip_check, config->log_strip);
    gtk_editable_set_text(GTK_EDITABLE(tab->log_directory_entry), config->log_directory);
    guint filename_index = config->log_file[0] == '\0'
                               ? 0
                               : value_index(log_filename_templates,
                                             config->log_file,
                                             TIO_GUI_CUSTOM_LOG_FILENAME_INDEX);
    gtk_drop_down_set_selected(tab->log_filename_dropdown, filename_index);
    gtk_editable_set_text(GTK_EDITABLE(tab->log_file_entry),
                          filename_index == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX
                              ? config->log_file
                              : log_filename_templates[filename_index]);
    gtk_widget_set_visible(GTK_WIDGET(tab->log_file_entry),
                           filename_index == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX);
    gtk_spin_button_set_value(tab->capture_part_spin, config->capture_part_mb);
    gtk_spin_button_set_value(tab->capture_time_spin, config->capture_part_seconds);
    gtk_spin_button_set_value(tab->capture_keep_spin, config->capture_keep_files);
    gtk_spin_button_set_value(tab->capture_disk_spin, config->capture_disk_mb);
    gtk_check_button_set_active(tab->reconnect_check, config->reconnect);
    gtk_check_button_set_active(tab->connection_notify_check, config->connection_notify);
    gtk_check_button_set_active(tab->connection_sound_check, config->connection_sound);
    gtk_drop_down_set_selected(tab->auto_connect_dropdown, config->auto_connect);
    gtk_editable_set_text(GTK_EDITABLE(tab->exclude_devices_entry), config->exclude_devices);
    gtk_editable_set_text(GTK_EDITABLE(tab->exclude_drivers_entry), config->exclude_drivers);
    gtk_editable_set_text(GTK_EDITABLE(tab->exclude_tids_entry), config->exclude_tids);
    gtk_drop_down_set_selected(tab->dtr_default, config->dtr_default);
    gtk_drop_down_set_selected(tab->rts_default, config->rts_default);
    gtk_spin_button_set_value(tab->line_pulse_spin, config->line_pulse_ms);
    gtk_check_button_set_active(tab->rs485_check, config->rs485);
    gtk_editable_set_text(GTK_EDITABLE(tab->rs485_entry), config->rs485_config);
    gtk_spin_button_set_value(tab->output_delay_spin, config->output_delay);
    gtk_spin_button_set_value(tab->output_line_delay_spin, config->output_line_delay);
    update_session_label(tab);
}

/* ---------------------------------------------------------------- profiles */

static void on_profile_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    (void)list;
    TioTab *tab = user_data;
    const char *name = g_object_get_data(G_OBJECT(row), "profile-name");
    TioProfile *profile = tio_settings_find_profile(&tab->app->settings, name);
    if (profile == NULL) {
        return;
    }

    apply_session_config(tab, &profile->session);
    g_free(tab->app->settings.active_profile);
    tab->app->settings.active_profile = g_strdup(name);
    refresh_profile_ui(tab);
    gtk_popover_popdown(tab->profile_popover);

    g_autofree gchar *message = g_strdup_printf(_("Loaded profile “%s”"), name);
    set_status(tab, message);
}

static void refresh_profile_ui(TioTab *tab)
{
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(tab->profile_list))) != NULL) {
        gtk_list_box_remove(tab->profile_list, child);
    }

    for (guint index = 0; index < tab->app->settings.profiles->len; ++index) {
        const TioProfile *profile = g_ptr_array_index(tab->app->settings.profiles, index);
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
        gtk_list_box_append(tab->profile_list, row);
    }

    gboolean has_profiles = tab->app->settings.profiles->len > 0;
    gtk_widget_set_visible(GTK_WIDGET(tab->profile_list), has_profiles);
    gtk_widget_set_visible(GTK_WIDGET(tab->profile_empty_label), !has_profiles);

    TioProfile *active = tio_settings_find_profile(&tab->app->settings, tab->app->settings.active_profile);
    if (active == NULL) {
        g_clear_pointer(&tab->app->settings.active_profile, g_free);
    }
    gtk_menu_button_set_label(tab->profile_button,
                              active != NULL ? active->name : _("Profile"));
    gtk_widget_set_sensitive(GTK_WIDGET(tab->profile_update_button), active != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->profile_duplicate_button), active != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->profile_delete_button), active != NULL);
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
    TioTab *tab = dialog->tab;

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
    if (tio_settings_find_profile(&tab->app->settings, name) != NULL) {
        gtk_label_set_text(dialog->hint_label, _("That profile name is already in use"));
        return;
    }

    TioSessionConfig captured;
    tio_session_config_init(&captured);
    capture_session_config(tab, &captured);
    tio_settings_store_profile(&tab->app->settings, name, &captured);
    tio_session_config_clear(&captured);

    g_free(tab->app->settings.active_profile);
    tab->app->settings.active_profile = g_strdup(name);
    refresh_profile_ui(tab);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&tab->app->settings, &error)) {
        g_warning("Could not save profile: %s", error->message);
    }

    g_autofree gchar *message = g_strdup_printf(_("Saved profile “%s”"), name);
    set_status(tab, message);
    gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void present_profile_name_dialog(TioTab *tab, TioProfileSaveMode mode)
{
    ProfileNameDialog *dialog = g_new0(ProfileNameDialog, 1);
    dialog->tab = tab;
    dialog->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog->window),
                         mode == TIO_GUI_PROFILE_SAVE_DUPLICATE ? _("Duplicate profile")
                                                                : _("Save profile"));
    gtk_window_set_transient_for(GTK_WINDOW(dialog->window), GTK_WINDOW(tab->app->window));
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
    if (mode == TIO_GUI_PROFILE_SAVE_DUPLICATE && tab->app->settings.active_profile != NULL) {
        g_autofree gchar *suggestion =
            g_strdup_printf(_("%s copy"), tab->app->settings.active_profile);
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
    TioTab *tab = user_data;
    gtk_popover_popdown(tab->profile_popover);
    present_profile_name_dialog(tab, TIO_GUI_PROFILE_SAVE_NEW);
}

static void on_profile_duplicate_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;
    gtk_popover_popdown(tab->profile_popover);
    present_profile_name_dialog(tab, TIO_GUI_PROFILE_SAVE_DUPLICATE);
}

static void on_profile_update_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;
    if (tab->app->settings.active_profile == NULL) {
        return;
    }

    TioSessionConfig captured;
    tio_session_config_init(&captured);
    capture_session_config(tab, &captured);
    tio_settings_store_profile(&tab->app->settings, tab->app->settings.active_profile, &captured);
    tio_session_config_clear(&captured);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&tab->app->settings, &error)) {
        g_warning("Could not save profile: %s", error->message);
    }

    g_autofree gchar *message =
        g_strdup_printf(_("Updated profile “%s”"), tab->app->settings.active_profile);
    set_status(tab, message);
    gtk_popover_popdown(tab->profile_popover);
}

static void on_profile_delete_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    TioTab *tab = user_data;
    g_autofree gchar *name = g_steal_pointer(&tab->pending_profile_delete);

    g_autoptr(GError) error = NULL;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    if (error != NULL || choice != 1 || name == NULL) {
        return;
    }

    tio_settings_remove_profile(&tab->app->settings, name);
    if (g_strcmp0(tab->app->settings.active_profile, name) == 0) {
        g_clear_pointer(&tab->app->settings.active_profile, g_free);
    }
    refresh_profile_ui(tab);

    g_autoptr(GError) save_error = NULL;
    if (!tio_settings_save(&tab->app->settings, &save_error)) {
        g_warning("Could not save profiles: %s", save_error->message);
    }

    g_autofree gchar *message = g_strdup_printf(_("Deleted profile “%s”"), name);
    set_status(tab, message);
}

static void on_profile_delete_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;
    if (tab->app->settings.active_profile == NULL) {
        return;
    }

    gtk_popover_popdown(tab->profile_popover);
    g_free(tab->pending_profile_delete);
    tab->pending_profile_delete = g_strdup(tab->app->settings.active_profile);

    g_autofree gchar *question =
        g_strdup_printf(_("Delete profile “%s”?"), tab->app->settings.active_profile);
    g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", question);
    const char *buttons[] = {_("Cancel"), _("Delete"), NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog,
                            GTK_WINDOW(tab->app->window),
                            NULL,
                            on_profile_delete_response,
                            tab);
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

static void observe_connection(TioTab *tab)
{
    g_autofree gchar *device = tio_connection_device(tab->child_pid,
        tab->config.auto_connect == 0 ? tab->config.device : NULL);
    gboolean connected = device != NULL;
    gboolean changed = connected != tab->observed_connected;
    tab->connection_checked = TRUE;
    tab->observed_connected = connected;
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_entry), connected);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_button), connected);
    update_quick_buttons(tab);
    if (!changed) return;
    g_free(tab->observed_device);
    tab->observed_device = g_strdup(device);
    g_autofree gchar *message = NULL;
    if (connected) {
        if (tab->ever_connected) ++tab->reconnect_count;
        tab->ever_connected = TRUE;
        tab->connected_at = g_get_monotonic_time();
        message = g_strdup_printf(_("Connected to %s"), device);
    } else {
        g_free(tab->disconnect_reason);
        tio_transfer_cancel(tab->transfer);
        tab->disconnect_reason = g_strdup(_("tio no longer has the serial device open"));
        message = g_strdup(_("Device disconnected; waiting for tio to reconnect"));
        /* A sequence must never continue by surprise when a board reboots. */
        g_clear_pointer(&tab->sequence_runner, tio_sequence_runner_free);
        if (tab->quick_send_timer) { g_source_remove(tab->quick_send_timer); tab->quick_send_timer = 0; }
        g_clear_pointer(&tab->quick_pending, g_byte_array_unref);
    }
    if (tab->capture) {
        const char *event = connected ? device : tab->disconnect_reason;
        tio_capture_record(tab->capture, connected ? TIO_CAPTURE_CONNECT : TIO_CAPTURE_DISCONNECT,
                           (const guint8 *)event, strlen(event), g_get_real_time());
    }
    set_status(tab, message);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_entry), connected);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_button), connected);
    update_quick_buttons(tab);
    if (tab->config.connection_sound) gdk_display_beep(gdk_display_get_default());
    if (tab->config.connection_notify) {
        GtkApplication *application = gtk_window_get_application(GTK_WINDOW(tab->app->window));
        if (application) {
            g_autoptr(GNotification) notification = g_notification_new(_("Serial connection"));
            g_notification_set_body(notification, message);
            g_autofree gchar *id = g_strdup_printf("serial-%p", (void *)tab);
            g_application_send_notification(G_APPLICATION(application), id, notification);
        }
    }
}

static void update_session_label(TioTab *tab)
{
    const char *device = tab->observed_device ? tab->observed_device : selected_string(tab->device_dropdown);
    const char *baud = selected_baud(tab);
    const char *data_bits = selected_string(tab->data_bits_dropdown);
    const char *stop_bits = selected_string(tab->stop_bits_dropdown);
    const char *parity = selected_string(tab->parity_dropdown);
    const char *flow = selected_string(tab->flow_dropdown);

    GString *text = g_string_new(NULL);
    g_string_append(text, device != NULL ? device : _("no device"));
    g_string_append_printf(text,
                           " · %s %s%c%s · %s",
                           baud != NULL && baud[0] != '\0' ? baud : "?",
                           data_bits != NULL ? data_bits : "?",
                           parity_letter(parity),
                           stop_bits != NULL ? stop_bits : "?",
                           flow != NULL ? flow : "?");

    if (tab->observed_connected) {
        gint64 seconds = (g_get_monotonic_time() - tab->connected_at) / G_USEC_PER_SEC;
        g_string_append_printf(text,
                               " · %02d:%02d:%02d",
                               (int)(seconds / 3600),
                               (int)((seconds / 60) % 60),
                               (int)(seconds % 60));
    }

    if (tab->child_pid > 0 && !tab->observed_connected)
        g_string_append_printf(text, " · %s", _("waiting for device"));
    if (tab->reconnect_count)
        g_string_append_printf(text, _(" · reconnects: %u"), tab->reconnect_count);
    if (tab->disconnect_reason)
        gtk_widget_set_tooltip_text(GTK_WIDGET(tab->status_label), tab->disconnect_reason);

    if (tab->child_pid > 0 && tab->raw != NULL) {
        g_autofree gchar *received = g_format_size(tab->rx_bytes);
        g_string_append_printf(text, " · %s %s", _("rx"), received);
        g_string_append_printf(text, " · %" G_GUINT64_FORMAT " %s", tab->rx_lines, _("lines"));
        if (tab->rx_rate > 0) {
            g_autofree gchar *rate = g_format_size(tab->rx_rate);
            g_string_append_printf(text, " · %s/s", rate);
        }
    }

    if (tab->log_path != NULL) {
        GStatBuf info;
        if (g_stat(tab->log_path, &info) == 0) {
            g_autofree gchar *size = g_format_size((guint64)info.st_size);
            g_string_append_printf(text, " · %s %s", _("log"), size);
        } else {
            g_string_append_printf(text, " · %s", _("log pending"));
        }
    } else if (gtk_check_button_get_active(tab->log_check)) {
        g_string_append_printf(text, " · %s", _("log ready"));
    }

    gtk_label_set_text(tab->session_label, text->str);
    update_tab_label(tab);
    if (tab->log_path != NULL) {
        gtk_widget_set_tooltip_text(GTK_WIDGET(tab->session_label), tab->log_path);
    } else {
        gtk_widget_set_tooltip_text(GTK_WIDGET(tab->session_label), NULL);
    }
    g_string_free(text, TRUE);
}

static gboolean on_session_tick(gpointer user_data)
{
    TioTab *tab = user_data;

    observe_connection(tab);

    /* The tick is one second, so the byte delta is the rate. */
    tab->rx_rate = tab->rx_bytes - tab->rx_bytes_at_tick;
    tab->rx_bytes_at_tick = tab->rx_bytes;
    update_session_label(tab);

    if (tab->log_path != NULL && !tab->log_warning_shown && tab->app->settings.log_warning_mb > 0) {
        GStatBuf info;
        guint64 limit = (guint64)tab->app->settings.log_warning_mb * 1024U * 1024U;
        if (g_stat(tab->log_path, &info) == 0 && (guint64)info.st_size > limit) {
            tab->log_warning_shown = TRUE;
            g_autofree gchar *size = g_format_size((guint64)info.st_size);
            g_autofree gchar *message =
                g_strdup_printf(_("The session log has reached %s: %s"), size, tab->log_path);
            set_status(tab, message);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void stop_session_timer(TioTab *tab)
{
    if (tab->session_timer != 0) {
        g_source_remove(tab->session_timer);
        tab->session_timer = 0;
    }
}

static void on_child_exited(VteTerminal *terminal, gint status, gpointer user_data)
{
    (void)terminal;
    TioTab *tab = user_data;

    tab->child_pid = -1;
    tab->observed_connected = FALSE;
    g_free(tab->disconnect_reason);
    tab->disconnect_reason = g_strdup_printf(_("tio exited with status %d"), status);
    stop_session_timer(tab);
    raw_tap_stop(tab);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->connect_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_entry), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_button), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->hex_toggle), TRUE);
    update_quick_buttons(tab);
    gtk_button_set_label(tab->connect_button, _("Connect"));

    g_autofree gchar *message =
        g_strdup_printf(_("Disconnected (tio exit status: %d)"), status);
    set_status(tab, message);
    update_session_label(tab);
}

static void on_spawn_finished(VteTerminal *terminal, GPid pid, GError *error, gpointer user_data)
{
    (void)terminal;
    TioTab *tab = user_data;

    tab->spawn_pending = FALSE;
    g_clear_pointer(&tab->spawn_argv, g_strfreev);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->connect_button), TRUE);

    if (error != NULL) {
        g_autofree gchar *message =
            g_strdup_printf(_("Could not start tio: %s"), error->message);
        set_status(tab, message);
        tab->child_pid = -1;
        g_clear_pointer(&tab->log_path, g_free);
        raw_tap_stop(tab);
        gtk_button_set_label(tab->connect_button, _("Connect"));
        return;
    }

    tab->child_pid = pid;
    if (tab->close_requested) { (void)kill(pid, SIGHUP); return; }
    tab->connected_at = g_get_monotonic_time();
    raw_tap_start(tab);
    gtk_button_set_label(tab->connect_button, _("Disconnect"));
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->hex_toggle), FALSE);
    update_quick_buttons(tab);
    set_status(tab, _("Waiting for serial device…"));
    observe_connection(tab);
    stop_session_timer(tab);
    tab->session_timer = g_timeout_add_seconds(1, on_session_tick, tab);
    update_session_label(tab);
    focus_log_view(tab);
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

/* ------------------------------------------------------------- raw tap */

/* tio's --socket multiplexes the serial stream to every connected client, and
   that copy is the bytes as received: --output-mode and --timestamp change
   only what the terminal renders. Reading it here gives the raw stream without
   scraping the rendered screen and without a proxy process of our own.

   Three streams stay independent: VTE renders the pty, tio writes the log, and
   this tap feeds anything that needs the bytes themselves. */

#define TIO_GUI_RAW_BUFFER_SIZE 8192
#define TIO_GUI_RAW_CONNECT_INTERVAL_MS 50
/* tio creates the socket before it opens the device, but the GUI still has to
   wait for it. Give up after two seconds rather than retrying forever. */
#define TIO_GUI_RAW_CONNECT_ATTEMPTS 40

/* Refcounted so a read that is already in flight cannot outlive the session:
   the tab drops its reference and clears `tab`, and the pending callback then
   sees a detached tap and only releases the last reference. */
struct _TioRawTap {
    gint reference_count;
    TioTab *tab;
    GSocketConnection *connection;
    GCancellable *cancellable;
    guint8 buffer[TIO_GUI_RAW_BUFFER_SIZE];
    GBytes *sending;
    gboolean send_failed;
};

static void raw_tap_read(TioRawTap *tap);

static TioRawTap *raw_tap_ref(TioRawTap *tap)
{
    tap->reference_count++;
    return tap;
}

static void raw_tap_unref(TioRawTap *tap)
{
    if (--tap->reference_count > 0) {
        return;
    }
    g_clear_pointer(&tap->sending, g_bytes_unref);
    g_clear_object(&tap->cancellable);
    g_clear_object(&tap->connection);
    g_free(tap);
}

/* Detach the tap from its session. Any read still in flight is cancelled and
   releases its own reference when the callback runs. */
static void raw_tap_stop(TioTab *tab)
{
    tio_transfer_cancel(tab->transfer);
    if (tab->capture) {
        const char *message = "Session stopped";
        tio_capture_record(tab->capture, TIO_CAPTURE_DISCONNECT, (const guint8 *)message, strlen(message), g_get_real_time());
        tio_capture_stop(tab->capture);
    }
    if (tab->line_command_timer) { g_source_remove(tab->line_command_timer); tab->line_command_timer = 0; }
    g_clear_pointer(&tab->line_command_path, g_free);
    if (tab->line_script_paths) {
        for (guint i = 0; i < tab->line_script_paths->len; ++i)
            g_unlink(g_ptr_array_index(tab->line_script_paths, i));
        g_clear_pointer(&tab->line_script_paths, g_ptr_array_unref);
    }
    g_clear_pointer(&tab->sequence_runner, tio_sequence_runner_free);
    if (tab->quick_send_timer) {
        g_source_remove(tab->quick_send_timer);
        tab->quick_send_timer = 0;
    }
    g_clear_pointer(&tab->quick_pending, g_byte_array_unref);
    if (tab->raw_connect_timer != 0) {
        g_source_remove(tab->raw_connect_timer);
        tab->raw_connect_timer = 0;
    }
    if (tab->raw != NULL) {
        TioRawTap *tap = tab->raw;
        tab->raw = NULL;
        tap->tab = NULL;
        g_cancellable_cancel(tap->cancellable);
        raw_tap_unref(tap);
    }
    if (tab->socket_path != NULL) {
        g_unlink(tab->socket_path);
        g_clear_pointer(&tab->socket_path, g_free);
    }
}

static void on_raw_tap_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    TioRawTap *tap = user_data;
    g_autoptr(GError) error = NULL;
    gssize count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

    if (tap->tab == NULL) {
        raw_tap_unref(tap);
        return;
    }
    if (count <= 0) {
        /* Zero is EOF: tio exited or closed the socket. An error other than
           cancellation ends the tap too; the session itself is unaffected. */
        tap->tab->raw = NULL;
        tap->tab = NULL;
        g_cancellable_cancel(tap->cancellable);
        raw_tap_unref(tap); /* session ownership */
        raw_tap_unref(tap); /* read callback ownership */
        return;
    }

    tap->tab->rx_bytes += (guint64)count;
    for (gssize index = 0; index < count; ++index) {
        if (tap->buffer[index] == '\n') {
            tap->tab->rx_lines++;
        }
    }
    tio_highlighter_feed(tap->tab->highlighter, tap->buffer, (gsize)count);
    if (tap->tab->capture) tio_capture_record(tap->tab->capture, TIO_CAPTURE_RX, tap->buffer, (gsize)count, g_get_real_time());
    if (tap->tab->log_model) tio_log_model_feed(tap->tab->log_model, tap->buffer, (gsize)count, g_get_real_time());
    if (tap->tab->highlight_follow) {
        scroll_highlight_to_bottom(tap->tab);
    }
    tap->tab->highlight_pending = !tap->tab->highlight_follow;
    update_scroll_button(tap->tab);
    raw_tap_read(tap);
    raw_tap_unref(tap);
}

static void raw_tap_read(TioRawTap *tap)
{
    GInputStream *stream = g_io_stream_get_input_stream(G_IO_STREAM(tap->connection));
    g_input_stream_read_async(stream,
                              tap->buffer,
                              sizeof tap->buffer,
                              G_PRIORITY_DEFAULT,
                              tap->cancellable,
                              on_raw_tap_read,
                              raw_tap_ref(tap));
}

static gboolean raw_tap_try_connect(gpointer user_data)
{
    TioTab *tab = user_data;

    if (tab->socket_path == NULL || tab->child_pid <= 0) {
        tab->raw_connect_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (!g_file_test(tab->socket_path, G_FILE_TEST_EXISTS)) {
        if (++tab->raw_connect_attempts >= TIO_GUI_RAW_CONNECT_ATTEMPTS) {
            tab->raw_connect_timer = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(tab->socket_path);
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GError) error = NULL;
    /* A local unix socket connects immediately or not at all, so this does not
       block the main loop in any meaningful way. */
    GSocketConnection *connection =
        g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, &error);
    if (connection == NULL) {
        if (++tab->raw_connect_attempts >= TIO_GUI_RAW_CONNECT_ATTEMPTS) {
            tab->raw_connect_timer = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    TioRawTap *tap = g_new0(TioRawTap, 1);
    tap->reference_count = 1;
    tap->tab = tab;
    tap->connection = connection;
    tap->cancellable = g_cancellable_new();
    tab->raw = tap;
    raw_tap_read(tap);

    tab->raw_connect_timer = 0;
    return G_SOURCE_REMOVE;
}

/* Connect as soon as the session starts: the socket only carries what arrives
   after a client attaches, and the device banner is the part worth having. */
static void raw_tap_start(TioTab *tab)
{
    if (tab->socket_path == NULL) {
        return;
    }
    tab->raw_connect_attempts = 0;
    if (raw_tap_try_connect(tab)) {
        tab->raw_connect_timer =
            g_timeout_add(TIO_GUI_RAW_CONNECT_INTERVAL_MS, raw_tap_try_connect, tab);
    }
}

/* One socket per session, so several sessions never collide.

   Returns NULL when no usable path exists, and the session then runs without a
   tap: losing the byte counters is acceptable, refusing to connect is not. A
   unix socket path is capped at sizeof(struct sockaddr_un.sun_path), and tio
   refuses to start at all if it is handed a longer one. */
static gchar *build_socket_path(void)
{
    static guint serial = 0;
    g_autofree gchar *directory =
        g_build_filename(g_get_user_runtime_dir(), "tio-gui", NULL);
    if (g_mkdir_with_parents(directory, 0700) == -1) {
        return NULL;
    }
    g_autofree gchar *name = g_strdup_printf("s-%d-%u.sock", (int)getpid(), ++serial);
    gchar *path = g_build_filename(directory, name, NULL);
    if (strlen(path) >= sizeof(((struct sockaddr_un *)NULL)->sun_path)) {
        g_free(path);
        return NULL;
    }
    return path;
}

static gchar **build_tio_argv(const TioSessionConfig *config,
                              const char *log_path,
                              const char *socket_path)
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

    if (socket_path != NULL) {
        g_ptr_array_add(arguments, g_strdup("--socket"));
        g_ptr_array_add(arguments, g_strdup_printf("unix:%s", socket_path));
    }

    tio_serial_options_append(arguments, config);
    if (config->auto_connect == 0) g_ptr_array_add(arguments, g_strdup(config->device));
    g_ptr_array_add(arguments, NULL);
    return (gchar **)g_ptr_array_free(arguments, FALSE);
}

static void disconnect_tio(TioTab *tab)
{
    if (tab->child_pid <= 0) {
        return;
    }

    if (kill(tab->child_pid, SIGHUP) == -1 && errno != ESRCH) {
        g_autofree gchar *message =
            g_strdup_printf(_("Could not stop tio: %s"), g_strerror(errno));
        set_status(tab, message);
        return;
    }

    raw_tap_stop(tab);
    set_status(tab, _("Disconnecting…"));
}

static void connect_tio(TioTab *tab)
{
    const char *device = selected_string(tab->device_dropdown);
    const char *baud = selected_baud(tab);

    if ((device == NULL && gtk_drop_down_get_selected(tab->auto_connect_dropdown) == 0) || baud == NULL) {
        set_status(tab, _("Select a serial device and baud rate first"));
        return;
    }
    if (!baud_is_valid(baud)) {
        set_status(tab, _("Enter a valid baud rate from 1 to 4294967295"));
        gtk_widget_grab_focus(GTK_WIDGET(tab->custom_baud_entry));
        return;
    }

    g_autofree gchar *tio_path = g_find_program_in_path("tio");
    if (tio_path == NULL) {
        set_status(tab, _("tio was not found in PATH"));
        return;
    }

    if (tab_holding_device(tab->app, device, tab) != NULL) {
        g_autofree gchar *message =
            g_strdup_printf(_("Another session is already connected to %s"), device);
        set_status(tab, message);
        return;
    }

    capture_session_config(tab, &tab->config);
    g_autoptr(GError) options_error = NULL;
    if (!tio_serial_options_validate(&tab->config, &options_error)) {
        set_status(tab, options_error->message);
        return;
    }
    /* The last connection seeds whatever session is opened next. */
    tio_session_config_copy(&tab->app->settings.defaults, &tab->config);
    const TioSessionConfig *config = &tab->config;

    g_clear_pointer(&tab->log_path, g_free);
    tab->log_warning_shown = FALSE;
    if (config->logging) {
        if (config->log_directory[0] == '\0') {
            set_status(tab, _("Choose a log directory first"));
            return;
        }
        tab->log_path = build_log_path(config);
        /* Two sessions started in the same second, or sharing a custom
           filename template, would otherwise write to one file. */
        for (guint suffix = 2; suffix < 1000 &&
                               log_path_in_use(tab->app, tab->log_path, tab); ++suffix) {
            g_autofree gchar *taken = g_steal_pointer(&tab->log_path);
            const char *extension = strrchr(taken, '.');
            gsize stem = extension != NULL ? (gsize)(extension - taken) : strlen(taken);
            tab->log_path = g_strdup_printf("%.*s-%u%s",
                                            (int)stem,
                                            taken,
                                            suffix,
                                            extension != NULL ? extension : "");
        }
        g_autofree gchar *log_parent = g_path_get_dirname(tab->log_path);
        if (g_mkdir_with_parents(log_parent, 0750) == -1) {
            g_autofree gchar *message =
                g_strdup_printf(_("Could not create log directory: %s"), g_strerror(errno));
            set_status(tab, message);
            g_clear_pointer(&tab->log_path, g_free);
            return;
        }
    }

    raw_tap_stop(tab);
    tab->socket_path = build_socket_path();
    tab->rx_bytes = 0;
    tab->rx_lines = 0;
    tab->rx_bytes_at_tick = 0;
    tab->rx_rate = 0;
    tab->observed_connected = FALSE;
    tab->ever_connected = FALSE;
    tab->connection_checked = TRUE;
    tab->reconnect_count = 0;
    g_clear_pointer(&tab->observed_device, g_free);
    g_clear_pointer(&tab->disconnect_reason, g_free);

    g_free(tab->running_metadata);
    tab->running_metadata = g_strdup_printf("device=%s baud=%s data=%s stop=%s parity=%s flow=%s RS485=%u",
        config->device ? config->device : "auto", config->baud, config->data_bits, config->stop_bits,
        config->parity, config->flow, config->rs485);
    vte_terminal_reset(tab->terminal, TRUE, TRUE);
    tab->spawn_argv = build_tio_argv(config, tab->log_path, tab->socket_path);
    set_status(tab, _("Connecting…"));
    gtk_widget_set_sensitive(GTK_WIDGET(tab->connect_button), FALSE);

    tab->spawn_pending = TRUE;
    vte_terminal_spawn_async(tab->terminal,
                             VTE_PTY_DEFAULT,
                             NULL,
                             tab->spawn_argv,
                             NULL,
                             G_SPAWN_SEARCH_PATH,
                             NULL,
                             NULL,
                             NULL,
                             -1,
                             NULL,
                             on_spawn_finished,
                             tab);
}

static void on_connect_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;

    if (tab->child_pid > 0) {
        disconnect_tio(tab);
    } else {
        connect_tio(tab);
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
    TioTab *tab = user_data;

    vte_terminal_reset(tab->terminal, TRUE, TRUE);
    tio_highlighter_clear(tab->highlighter);
}

static void on_highlight_toggled(GtkCheckButton *button, gpointer user_data)
{
    TioTab *tab = user_data;
    gboolean active = gtk_check_button_get_active(button);
    gtk_stack_set_visible_child_name(tab->terminal_stack,
                                     active ? "highlight" : "terminal");
    update_scroll_button(tab);
    focus_log_view(tab);
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

static void send_bytes(TioTab *tab, const char *data, gsize length)
{
    if (tab->child_pid <= 0 || length == 0 || tio_transfer_active(tab->transfer)) {
        return;
    }
    if (tab->capture) tio_capture_record(tab->capture, TIO_CAPTURE_INPUT, (const guint8 *)data, length, g_get_real_time());
    vte_terminal_feed_child(tab->terminal, data, (gssize)length);
}

/* Binary payloads use the socket: the pty interprets Ctrl-T as a command. */
static void on_payload_written(GObject *source, GAsyncResult *result, gpointer data)
{
    TioRawTap *tap = data;
    g_autoptr(GError) error = NULL;
    gsize written = 0;
    gboolean ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, &written, &error);
    if (ok && tap->tab && tap->tab->capture) {
        gsize length;
        const guint8 *bytes = g_bytes_get_data(tap->sending, &length);
        tio_capture_record(tap->tab->capture, TIO_CAPTURE_TX, bytes, length, g_get_real_time());
    }
    if (ok && tap->tab && tap->tab->log_model) {
        gsize length;
        const guint8 *bytes = g_bytes_get_data(tap->sending, &length);
        GByteArray payload = {(guint8 *)bytes, (guint)length};
        g_autofree gchar *preview = tio_payload_preview(&payload);
        tio_log_model_command(tap->tab->log_model, preview, g_get_real_time());
    }
    g_clear_pointer(&tap->sending, g_bytes_unref);
    tap->send_failed = !ok;
    if (tap->tab) {
        if (ok) {
            g_autofree gchar *message = g_strdup_printf(_("Sent %zu bytes to tio"), written);
            set_status(tap->tab, message);
        } else {
            g_autofree gchar *message = g_strdup_printf(_("Send failed after %zu bytes: %s"), written, error->message);
            set_status(tap->tab, message);
        }
    }
    raw_tap_unref(tap);
}

static gboolean send_payload(TioTab *tab, const GByteArray *bytes)
{
    TioRawTap *tap = tab->raw;
    if (tio_transfer_active(tab->transfer)) { set_status(tab, _("Stop file transfer before sending commands")); return FALSE; }
    if (tab->child_pid <= 0 || !tap || (tab->connection_checked && !tab->observed_connected)) {
        set_status(tab, _("Serial data channel is not ready"));
        return FALSE;
    }
    if (tap->sending) {
        set_status(tab, _("A send is still in progress"));
        return FALSE;
    }
    if (!bytes->len) return TRUE;
    tap->sending = g_bytes_new(bytes->data, bytes->len);
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(tap->connection)),
        g_bytes_get_data(tap->sending, NULL), bytes->len, G_PRIORITY_DEFAULT,
        tap->cancellable, on_payload_written, raw_tap_ref(tap));
    return TRUE;
}

static void send_entry_contents(TioTab *tab)
{
    if (tab->child_pid <= 0) {
        return;
    }

    const char *text = gtk_editable_get_text(GTK_EDITABLE(tab->send_entry));
    if (text[0] == '\0') {
        return;
    }

    g_autofree gchar *payload = g_strdup(text);
    send_bytes(tab, payload, strlen(payload));
    const char *ending =
        line_ending_bytes(line_ending_values[gtk_drop_down_get_selected(tab->line_ending_dropdown)]);
    if (ending[0] != '\0') {
        send_bytes(tab, ending, strlen(ending));
    }

    tio_settings_push_history(&tab->app->settings, payload);
    if (tab->log_model) tio_log_model_command(tab->log_model, payload, g_get_real_time());
    refresh_history_ui(tab);
    tab->history_cursor = 0;
    g_clear_pointer(&tab->history_draft, g_free);

    tab->history_updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(tab->send_entry), "");
    tab->history_updating = FALSE;
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
    TioTab *tab = user_data;
    if (!tab->history_updating) {
        tab->history_cursor = 0;
    }
}

/* Cursor 0 is the line being typed; 1 is the newest stored entry. */
static void history_navigate(TioTab *tab, gint direction)
{
    GPtrArray *history = tab->app->settings.history;
    if (history->len == 0) {
        return;
    }

    gint cursor = (gint)tab->history_cursor + direction;
    cursor = CLAMP(cursor, 0, (gint)history->len);
    if (cursor == (gint)tab->history_cursor) {
        return;
    }

    if (tab->history_cursor == 0) {
        g_free(tab->history_draft);
        tab->history_draft = g_strdup(gtk_editable_get_text(GTK_EDITABLE(tab->send_entry)));
    }

    const char *text = cursor == 0
                           ? (tab->history_draft != NULL ? tab->history_draft : "")
                           : g_ptr_array_index(history, history->len - (guint)cursor);

    tab->history_updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(tab->send_entry), text);
    gtk_editable_set_position(GTK_EDITABLE(tab->send_entry), -1);
    tab->history_updating = FALSE;
    tab->history_cursor = (guint)cursor;
}

static gboolean on_send_entry_key(GtkEventControllerKey *controller,
                                  guint keyval,
                                  guint keycode,
                                  GdkModifierType state,
                                  gpointer user_data)
{
    (void)controller;
    (void)keycode;
    TioTab *tab = user_data;

    if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) != 0) {
        return GDK_EVENT_PROPAGATE;
    }
    if (keyval == GDK_KEY_Up) {
        history_navigate(tab, 1);
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_Down) {
        history_navigate(tab, -1);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static void on_history_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    (void)list;
    TioTab *tab = user_data;
    const char *text = g_object_get_data(G_OBJECT(row), "history-text");
    if (text == NULL) {
        return;
    }

    tab->history_updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(tab->send_entry), text);
    gtk_editable_set_position(GTK_EDITABLE(tab->send_entry), -1);
    tab->history_updating = FALSE;
    tab->history_cursor = 0;
    gtk_popover_popdown(tab->history_popover);
    gtk_widget_grab_focus(GTK_WIDGET(tab->send_entry));
}

static void refresh_history_ui(TioTab *tab)
{
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(tab->history_list))) != NULL) {
        gtk_list_box_remove(tab->history_list, child);
    }

    GPtrArray *history = tab->app->settings.history;
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
        gtk_list_box_append(tab->history_list, row);
    }

    gboolean has_history = history->len > 0;
    gtk_widget_set_visible(GTK_WIDGET(tab->history_list), has_history);
    gtk_widget_set_visible(GTK_WIDGET(tab->history_empty_label), !has_history);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->history_clear_button), has_history);
}

static void on_history_clear_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;

    tio_settings_clear_history(&tab->app->settings);
    tab->history_cursor = 0;
    g_clear_pointer(&tab->history_draft, g_free);
    refresh_history_ui(tab);
    gtk_popover_popdown(tab->history_popover);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&tab->app->settings, &error)) {
        g_warning("Could not save settings: %s", error->message);
    }
    set_status(tab, _("Send history cleared"));
}

/* ----------------------------------------------------------------- search */

static void update_search_regex(TioTab *tab)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(tab->search_entry));
    if (text == NULL || text[0] == '\0') {
        vte_terminal_search_set_regex(tab->terminal, NULL, 0);
        gtk_widget_remove_css_class(GTK_WIDGET(tab->search_entry), "error");
        g_clear_pointer(&tab->search_pattern, g_free);
        return;
    }

    g_autofree gchar *pattern = gtk_toggle_button_get_active(tab->search_regex_toggle)
                                    ? g_strdup(text)
                                    : g_regex_escape_string(text, -1);
    guint32 flags = PCRE2_MULTILINE | PCRE2_UTF | PCRE2_NO_UTF_CHECK;
    if (!gtk_toggle_button_get_active(tab->search_case_toggle)) {
        flags |= PCRE2_CASELESS;
    }

    /* Installing a regex restarts VTE's search from the current view, so an
       unchanged pattern has to be left alone for Find next to keep advancing. */
    if (flags == tab->search_flags && g_strcmp0(pattern, tab->search_pattern) == 0 &&
        vte_terminal_search_get_regex(tab->terminal) != NULL) {
        return;
    }

    g_autoptr(GError) error = NULL;
    VteRegex *regex = vte_regex_new_for_search(pattern, -1, flags, &error);
    if (regex == NULL) {
        vte_terminal_search_set_regex(tab->terminal, NULL, 0);
        g_clear_pointer(&tab->search_pattern, g_free);
        gtk_widget_add_css_class(GTK_WIDGET(tab->search_entry), "error");
        g_autofree gchar *message =
            g_strdup_printf(_("Invalid search pattern: %s"), error->message);
        set_status(tab, message);
        return;
    }

    gtk_widget_remove_css_class(GTK_WIDGET(tab->search_entry), "error");
    vte_terminal_search_set_regex(tab->terminal, regex, 0);
    vte_terminal_search_set_wrap_around(tab->terminal, TRUE);
    vte_regex_unref(regex);
    g_free(tab->search_pattern);
    tab->search_pattern = g_steal_pointer(&pattern);
    tab->search_flags = flags;
}

static void search_step(TioTab *tab, gboolean forward)
{
    update_search_regex(tab);
    if (vte_terminal_search_get_regex(tab->terminal) == NULL) {
        return;
    }

    gboolean found = forward ? vte_terminal_search_find_next(tab->terminal)
                             : vte_terminal_search_find_previous(tab->terminal);
    if (!found) {
        set_status(tab, _("No matches"));
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
    TioTab *tab = user_data;
    gtk_search_bar_set_search_mode(tab->search_bar, FALSE);
    focus_log_view(tab);
}

/* ------------------------------------------------------------- autoscroll */

static GtkAdjustment *terminal_adjustment(TioTab *tab)
{
    return gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->terminal));
}

static gboolean terminal_at_bottom(TioTab *tab)
{
    GtkAdjustment *adjustment = terminal_adjustment(tab);
    if (adjustment == NULL) {
        return TRUE;
    }
    double value = gtk_adjustment_get_value(adjustment);
    double upper = gtk_adjustment_get_upper(adjustment);
    double page = gtk_adjustment_get_page_size(adjustment);
    return value >= upper - page - 0.5;
}

static void scroll_terminal_to_bottom(TioTab *tab)
{
    GtkAdjustment *adjustment = terminal_adjustment(tab);
    if (adjustment == NULL) {
        return;
    }
    gtk_adjustment_set_value(adjustment,
                             gtk_adjustment_get_upper(adjustment) -
                                 gtk_adjustment_get_page_size(adjustment));
}

static void update_scroll_button(TioTab *tab)
{
    gboolean highlight = gtk_check_button_get_active(tab->highlight_toggle);
    gboolean pending = highlight ? tab->highlight_pending : tab->pending_output;
    gboolean follow = highlight ? tab->highlight_follow : tab->follow_output;
    gtk_button_set_label(tab->scroll_bottom_button,
                         pending ? _("New output ↓") : _("Back to bottom ↓"));
    gtk_widget_set_visible(GTK_WIDGET(tab->scroll_bottom_button), !follow);
}

static void on_terminal_scrolled(GtkAdjustment *adjustment, gpointer user_data)
{
    (void)adjustment;
    TioTab *tab = user_data;

    gboolean at_bottom = terminal_at_bottom(tab);
    if (at_bottom && vte_terminal_get_has_selection(tab->terminal)) {
        vte_terminal_unselect_all(tab->terminal);
    }
    if (at_bottom == tab->follow_output) {
        return;
    }
    tab->follow_output = at_bottom;
    if (at_bottom) {
        tab->pending_output = FALSE;
    }
    update_scroll_button(tab);
}

static void on_terminal_content_changed(GtkAdjustment *adjustment, gpointer user_data)
{
    (void)adjustment;
    TioTab *tab = user_data;

    if (vte_terminal_get_has_selection(tab->terminal)) {
        tab->follow_output = FALSE;
    }
    if (tab->follow_output) {
        scroll_terminal_to_bottom(tab);
        return;
    }
    if (!tab->pending_output) {
        tab->pending_output = TRUE;
        update_scroll_button(tab);
    }
}

static void on_scroll_bottom_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;

    if (gtk_check_button_get_active(tab->highlight_toggle)) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(tab->highlight_view);
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_place_cursor(buffer, &end);
        tab->highlight_follow = TRUE;
        tab->highlight_pending = FALSE;
        scroll_highlight_to_bottom(tab);
        update_scroll_button(tab);
        return;
    }
    vte_terminal_unselect_all(tab->terminal);
    scroll_terminal_to_bottom(tab);
    tab->follow_output = TRUE;
    tab->pending_output = FALSE;
    update_scroll_button(tab);
}

static void on_terminal_selection_changed(VteTerminal *terminal, gpointer user_data)
{
    TioTab *tab = user_data;
    tab->follow_output = !vte_terminal_get_has_selection(terminal) && terminal_at_bottom(tab);
    update_scroll_button(tab);
}

static void on_highlight_scrolled(GtkAdjustment *adjustment, gpointer user_data)
{
    TioTab *tab = user_data;
    /* GtkTextView also changes the adjustment while validating/reflowing
       text. Only an input event is allowed to turn following off. */
    if (tab->highlight_adjusting || tab->highlight_follow || tab->highlight_pointer_down) {
        return;
    }
    tab->highlight_follow =
        gtk_adjustment_get_value(adjustment) >=
            gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment) - 0.5;
    if (tab->highlight_follow) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(tab->highlight_view);
        if (gtk_text_buffer_get_has_selection(buffer)) {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            gtk_text_buffer_place_cursor(buffer, &end);
        }
        tab->highlight_pending = FALSE;
    }
    update_scroll_button(tab);
}

static void scroll_highlight_to_bottom(TioTab *tab)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(tab->highlight_view);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "tio-highlight-end");
    if (mark == NULL) {
        mark = gtk_text_buffer_create_mark(buffer, "tio-highlight-end", &end, FALSE);
    } else {
        gtk_text_buffer_move_mark(buffer, mark, &end);
    }
    /* Unlike scroll_to_iter, this remains pending until the text layout is
       valid. Bottom alignment includes the final line and the view margin. */
    tab->highlight_adjusting = TRUE;
    gtk_text_view_scroll_to_mark(tab->highlight_view, mark, 0.0, TRUE, 0.0, 1.0);
    GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->highlight_view));
    gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment) -
                                        gtk_adjustment_get_page_size(adjustment));
    tab->highlight_adjusting = FALSE;
}

static gboolean on_highlight_wheel(GtkEventControllerScroll *controller, double dx,
                                   double dy, gpointer user_data)
{
    (void)controller;
    (void)dx;
    TioTab *tab = user_data;
    if (dy < 0) {
        tab->highlight_follow = FALSE;
        update_scroll_button(tab);
    }
    return FALSE;
}

static void on_highlight_pointer_pressed(GtkGestureClick *gesture, int n_press,
                                         double x, double y, gpointer user_data)
{
    (void)gesture; (void)n_press; (void)x; (void)y;
    TioTab *tab = user_data;
    tab->highlight_pointer_down = TRUE;
    tab->highlight_follow = FALSE;
    update_scroll_button(tab);
}

static void on_highlight_pointer_released(GtkGestureClick *gesture, int n_press,
                                          double x, double y, gpointer user_data)
{
    (void)gesture; (void)n_press; (void)x; (void)y;
    TioTab *tab = user_data;
    tab->highlight_pointer_down = FALSE;
    if (!gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(tab->highlight_view))) {
        on_highlight_scrolled(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->highlight_view)), tab);
    }
}

static void on_highlight_pointer_stopped(GtkGestureClick *gesture, gpointer user_data)
{
    on_highlight_pointer_released(gesture, 0, 0, 0, user_data);
}

static void on_highlight_content_changed(GtkAdjustment *adjustment, gpointer user_data)
{
    (void)adjustment;
    TioTab *tab = user_data;
    if (gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(tab->highlight_view))) {
        tab->highlight_follow = FALSE;
    }
    if (tab->highlight_follow) {
        scroll_highlight_to_bottom(tab);
    }
    update_scroll_button(tab);
}

static void on_highlight_selection_changed(GObject *buffer, GParamSpec *pspec, gpointer user_data)
{
    (void)buffer;
    (void)pspec;
    TioTab *tab = user_data;
    gboolean selected = gtk_text_buffer_get_has_selection(GTK_TEXT_BUFFER(buffer));
    if (selected == tab->highlight_selected) {
        return;
    }
    tab->highlight_selected = selected;
    if (selected) {
        tab->highlight_follow = FALSE;
        update_scroll_button(tab);
    } else {
        on_highlight_scrolled(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->highlight_view)), tab);
    }
}

static gboolean on_log_return_pressed(GtkEventControllerKey *controller, guint keyval,
                                      guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
        !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_SHIFT_MASK))) {
        on_scroll_bottom_clicked(NULL, user_data);
    }
    /* Continue normal VTE input / send-entry activation after resuming. */
    return FALSE;
}

/* Let VTE encode keyboard events, including control keys, terminal escape
   sequences and input-method commits. The text view remains selection-only. */
static gboolean on_highlight_key_pressed(GtkEventControllerKey *controller, guint keyval,
                                         guint keycode, GdkModifierType state, gpointer user_data)
{
    TioTab *tab = user_data;
    guint lower = gdk_keyval_to_lower(keyval);
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    /* Keep application shortcuts available before forwarding terminal input. */
    if (keyval == GDK_KEY_F5 || keyval == GDK_KEY_F6 ||
        (ctrl && shift && (lower == GDK_KEY_c || lower == GDK_KEY_v ||
                           lower == GDK_KEY_f || lower == GDK_KEY_l)) ||
        (ctrl && (lower == GDK_KEY_t || lower == GDK_KEY_w ||
                  keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down))) {
        return FALSE;
    }
    on_log_return_pressed(controller, keyval, keycode, state, tab);
    gtk_event_controller_key_forward(controller, GTK_WIDGET(tab->terminal));
    return TRUE;
}

static void on_highlight_key_released(GtkEventControllerKey *controller, guint keyval,
                                      guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)keyval;
    (void)keycode;
    (void)state;
    TioTab *tab = user_data;
    gtk_event_controller_key_forward(controller, GTK_WIDGET(tab->terminal));
}

/* ---------------------------------------------------------- quick buttons */

static gboolean quick_send_delayed(gpointer data)
{
    TioTab *tab = data;
    tab->quick_send_timer = 0;
    send_payload(tab, tab->quick_pending);
    g_clear_pointer(&tab->quick_pending, g_byte_array_unref);
    return G_SOURCE_REMOVE;
}

static void on_quick_button_clicked(GtkButton *button, gpointer user_data)
{
    TioTab *tab = user_data;
    guint stored_index = GPOINTER_TO_UINT(
        g_object_get_data(G_OBJECT(button), "quick-button-index"));
    if (tab->child_pid <= 0 || stored_index == 0) {
        return;
    }

    guint index = stored_index - 1;
    g_autoptr(GError) error = NULL;
    g_autoptr(GByteArray) bytes = tio_payload_build(tab->config.quick_payloads[index],
        tab->config.quick_modes[index] != 0, tab->config.quick_endings[index],
        tab->config.quick_crcs[index], &error);
    if (!bytes) { set_status(tab, error->message); return; }
    if (tio_sequence_runner_active(tab->sequence_runner)) {
        set_status(tab, _("Stop the sequence before sending a quick command"));
        return;
    }
    if (tab->quick_send_timer) {
        set_status(tab, _("A delayed send is pending; disconnect to cancel"));
        return;
    }
    if (tab->config.quick_delays[index]) {
        tab->quick_pending = g_byte_array_ref(bytes);
        tab->quick_send_timer = g_timeout_add(tab->config.quick_delays[index], quick_send_delayed, tab);
        set_status(tab, _("Send scheduled; disconnect to cancel"));
    } else {
        send_payload(tab, bytes);
    }
}

static void update_quick_buttons(TioTab *tab)
{
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        GtkButton *button = tab->quick_buttons[index];
        if (button == NULL) {
            continue;
        }
        gtk_button_set_label(button, tab->config.quick_labels[index]);
        gtk_widget_set_tooltip_text(GTK_WIDGET(button),
                                    tab->config.quick_payloads[index]);
        gtk_widget_set_sensitive(GTK_WIDGET(button),
                                 tab->child_pid > 0 &&
                                     (!tab->connection_checked || tab->observed_connected) &&
                                     tab->config.quick_payloads[index][0] != '\0');
    }
}

static void quick_editor_preview(QuickButtonEditor *editor)
{
    gboolean valid = TRUE;
    for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
        g_autoptr(GError) error = NULL;
        g_autoptr(GByteArray) bytes = tio_payload_build(
            gtk_editable_get_text(GTK_EDITABLE(editor->payload_entries[i])),
            gtk_drop_down_get_selected(editor->modes[i]) == 1,
            gtk_drop_down_get_selected(editor->endings[i]),
            gtk_drop_down_get_selected(editor->crcs[i]), &error);
        g_autofree gchar *preview = bytes ? tio_payload_preview(bytes) : g_strdup(error->message);
        gtk_label_set_text(editor->previews[i], preview);
        if (!bytes) valid = FALSE;
    }
    gtk_widget_set_sensitive(editor->save, valid);
    if (editor->export_button) gtk_widget_set_sensitive(editor->export_button, valid);
}

static void quick_text_changed(GtkEditable *entry, gpointer data)
{
    (void)entry;
    quick_editor_preview(data);
}

static void quick_option_changed(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)object; (void)spec;
    quick_editor_preview(data);
}

static void quick_insert_control(GtkButton *button, gpointer data)
{
    QuickButtonEditor *editor = data;
    guint row = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "row"));
    const char *escaped = g_object_get_data(G_OBJECT(button), "escaped");
    const char *hex = g_object_get_data(G_OBJECT(button), "hex");
    GtkEditable *entry = GTK_EDITABLE(editor->payload_entries[row]);
    int start, end;
    if (gtk_editable_get_selection_bounds(entry, &start, &end))
        gtk_editable_delete_text(entry, start, end);
    int position = gtk_editable_get_position(entry);
    const char *insert = gtk_drop_down_get_selected(editor->modes[row]) ? hex : escaped;
    gtk_editable_insert_text(entry, insert, -1, &position);
    gtk_editable_set_position(entry, position);
    gtk_widget_grab_focus(GTK_WIDGET(entry));
}

static void quick_editor_capture(QuickButtonEditor *editor, TioSessionConfig *config)
{
    for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
        g_free(config->quick_labels[i]);
        g_free(config->quick_payloads[i]);
        config->quick_labels[i] = g_strdup(gtk_editable_get_text(GTK_EDITABLE(editor->label_entries[i])));
        config->quick_payloads[i] = g_strdup(gtk_editable_get_text(GTK_EDITABLE(editor->payload_entries[i])));
        config->quick_modes[i] = gtk_drop_down_get_selected(editor->modes[i]);
        config->quick_endings[i] = gtk_drop_down_get_selected(editor->endings[i]);
        config->quick_crcs[i] = gtk_drop_down_get_selected(editor->crcs[i]);
        config->quick_delays[i] = (guint)gtk_spin_button_get_value_as_int(editor->delays[i]);
    }
}

static void quick_file_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GtkWindow) window = data;
    QuickButtonEditor *editor = g_object_get_data(G_OBJECT(window), "quick-editor");
    gboolean exporting = GPOINTER_TO_INT(g_object_get_data(source, "exporting"));
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = exporting ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error)
                                    : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!gtk_widget_get_visible(GTK_WIDGET(window))) return;
    if (!file) {
        if (error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
            gtk_label_set_text(editor->file_status, error->message);
        return;
    }
    g_autofree gchar *path = g_file_get_path(file);
    if (!path) { gtk_label_set_text(editor->file_status, _("Choose a local file")); return; }
    TioSessionConfig config;
    tio_session_config_init(&config);
    if (exporting) {
        quick_editor_capture(editor, &config);
        gsize length = 0;
        g_autofree gchar *text = tio_quick_presets_encode(&config, &length);
        if (g_file_set_contents(path, text, (gssize)length, &error))
            gtk_label_set_text(editor->file_status, _("Button group exported"));
    } else {
        /* Bound the read before parsing even when the selected file is huge. */
        g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, &error);
        g_autofree gchar *text = g_malloc(1024 * 1024 + 1);
        gsize length = 0;
        if (stream && g_input_stream_read_all(G_INPUT_STREAM(stream), text, 1024 * 1024 + 1,
                                             &length, NULL, &error) &&
            tio_quick_presets_decode(&config, text, length, &error)) {
            for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
                gtk_editable_set_text(GTK_EDITABLE(editor->label_entries[i]), config.quick_labels[i]);
                gtk_editable_set_text(GTK_EDITABLE(editor->payload_entries[i]), config.quick_payloads[i]);
                gtk_drop_down_set_selected(editor->modes[i], config.quick_modes[i]);
                gtk_drop_down_set_selected(editor->endings[i], config.quick_endings[i]);
                gtk_drop_down_set_selected(editor->crcs[i], config.quick_crcs[i]);
                gtk_spin_button_set_value(editor->delays[i], config.quick_delays[i]);
            }
            gtk_label_set_text(editor->file_status, _("Button group loaded; Save to apply"));
        }
    }
    if (error) gtk_label_set_text(editor->file_status, error->message);
    tio_session_config_clear(&config);
}

static void quick_file_clicked(GtkButton *button, gpointer data)
{
    QuickButtonEditor *editor = data;
    gboolean exporting = GTK_WIDGET(button) == editor->export_button;
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    g_object_set_data(G_OBJECT(dialog), "exporting", GINT_TO_POINTER(exporting));
    if (exporting) {
        gtk_file_dialog_set_initial_name(dialog, "quick-buttons.ini");
        gtk_file_dialog_save(dialog, GTK_WINDOW(editor->window), NULL, quick_file_finished,
                             g_object_ref(editor->window));
    } else {
        gtk_file_dialog_open(dialog, GTK_WINDOW(editor->window), NULL, quick_file_finished,
                             g_object_ref(editor->window));
    }
}

static void on_quick_editor_save(GtkButton *button, gpointer user_data)
{
    (void)button;
    QuickButtonEditor *editor = user_data;
    TioSessionConfig *session = &editor->tab->config;

    quick_editor_capture(editor, session);
    capture_all_settings(editor->tab);
    update_quick_buttons(editor->tab);

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&editor->tab->app->settings, &error)) {
        gtk_label_set_text(editor->file_status, error->message);
        return;
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
    TioTab *tab = user_data;
    QuickButtonEditor *editor = g_new0(QuickButtonEditor, 1);
    editor->tab = tab;
    editor->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(editor->window), _("Customize quick buttons"));
    gtk_window_set_transient_for(GTK_WINDOW(editor->window), GTK_WINDOW(tab->app->window));
    gtk_window_set_modal(GTK_WINDOW(editor->window), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(editor->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(editor->window), 980, 620);
    g_object_set_data_full(G_OBJECT(editor->window), "quick-editor", editor, g_free);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), root);
    gtk_window_set_child(GTK_WINDOW(editor->window), scroll);

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
    gtk_grid_attach(GTK_GRID(grid), make_label(_("Mode")), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label(_("Line ending")), 4, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label(_("Checksum")), 5, 0, 1, 1);


    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_autofree gchar *number = g_strdup_printf("%u", index + 1);
        gtk_grid_attach(GTK_GRID(grid), make_label(number), 0, (gint)index * 3 + 1, 1, 1);

        editor->label_entries[index] = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_text(GTK_EDITABLE(editor->label_entries[index]),
                              tab->config.quick_labels[index]);
        gtk_grid_attach(GTK_GRID(grid),
                        GTK_WIDGET(editor->label_entries[index]),
                        1, (gint)index * 3 + 1, 1, 1);

        editor->payload_entries[index] = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_text(GTK_EDITABLE(editor->payload_entries[index]),
                              tab->config.quick_payloads[index]);
        gtk_widget_set_hexpand(GTK_WIDGET(editor->payload_entries[index]), TRUE);
        gtk_grid_attach(GTK_GRID(grid),
                        GTK_WIDGET(editor->payload_entries[index]),
                        2, (gint)index * 3 + 1, 1, 1);
        const char *modes[] = {_("Text"), "HEX", NULL};
        const char *endings[] = {_("None"), "LF", "CR", "CRLF", NULL};
        const char *crcs[] = {_("None"), "CRC-8/SMBUS", "CRC-16/MODBUS (LE)", "CRC-32/ISO-HDLC (LE)", NULL};
        editor->modes[index] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes));
        editor->endings[index] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(endings));
        editor->crcs[index] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(crcs));
        gtk_drop_down_set_selected(editor->modes[index], tab->config.quick_modes[index]);
        gtk_drop_down_set_selected(editor->endings[index], tab->config.quick_endings[index]);
        gtk_drop_down_set_selected(editor->crcs[index], tab->config.quick_crcs[index]);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(editor->modes[index]), 3, (gint)index * 3 + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(editor->endings[index]), 4, (gint)index * 3 + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(editor->crcs[index]), 5, (gint)index * 3 + 1, 1, 1);
        editor->previews[index] = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_xalign(editor->previews[index], 0);
        gtk_label_set_selectable(editor->previews[index], TRUE);
        gtk_label_set_ellipsize(editor->previews[index], PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(GTK_WIDGET(editor->previews[index]), "monospace");
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(editor->previews[index]), 1, (gint)index * 3 + 3, 5, 1);
        GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        const char *names[] = {"CR", "LF", "Tab", "Esc"};
        const char *escapes[] = {"\\r", "\\n", "\\t", "\\e"};
        const char *hex_bytes[] = {" 0D ", " 0A ", " 09 ", " 1B "};
        for (guint k = 0; k < 4; ++k) {
            GtkWidget *insert = gtk_button_new_with_label(names[k]);
            g_object_set_data(G_OBJECT(insert), "row", GUINT_TO_POINTER(index));
            g_object_set_data(G_OBJECT(insert), "escaped", (gpointer)escapes[k]);
            g_object_set_data(G_OBJECT(insert), "hex", (gpointer)hex_bytes[k]);
            g_signal_connect(insert, "clicked", G_CALLBACK(quick_insert_control), editor);
            gtk_box_append(GTK_BOX(controls), insert);
        }
        gtk_grid_attach(GTK_GRID(grid), controls, 2, (gint)index * 3 + 2, 2, 1);
        gtk_grid_attach(GTK_GRID(grid), make_label(_("Delay (ms)")), 4, (gint)index * 3 + 2, 1, 1);
        editor->delays[index] = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 60000, 10));
        gtk_spin_button_set_value(editor->delays[index], tab->config.quick_delays[index]);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(editor->delays[index]), 5, (gint)index * 3 + 2, 1, 1);
    }

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label(_("Cancel"));
    GtkWidget *save = gtk_button_new_with_label(_("Save"));
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), save);
    gtk_box_append(GTK_BOX(root), actions);

    editor->file_status = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(editor->file_status, TRUE);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(editor->file_status));
    GtkWidget *import_button = gtk_button_new_with_label(_("Import button group…"));
    editor->export_button = gtk_button_new_with_label(_("Export button group…"));
    gtk_box_prepend(GTK_BOX(actions), editor->export_button);
    gtk_box_prepend(GTK_BOX(actions), import_button);
    g_signal_connect(import_button, "clicked", G_CALLBACK(quick_file_clicked), editor);
    g_signal_connect(editor->export_button, "clicked", G_CALLBACK(quick_file_clicked), editor);
    editor->save = save;
    for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
        g_signal_connect(editor->payload_entries[i], "changed", G_CALLBACK(quick_text_changed), editor);
        g_signal_connect(editor->modes[i], "notify::selected", G_CALLBACK(quick_option_changed), editor);
        g_signal_connect(editor->endings[i], "notify::selected", G_CALLBACK(quick_option_changed), editor);
        g_signal_connect(editor->crcs[i], "notify::selected", G_CALLBACK(quick_option_changed), editor);
    }
    quick_editor_preview(editor);
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_quick_editor_cancel), editor);
    g_signal_connect(save, "clicked", G_CALLBACK(on_quick_editor_save), editor);
    gtk_window_present(GTK_WINDOW(editor->window));
}

/* ---------------------------------------------------------- send sequences */
typedef struct {
    GtkEntry *payload;
    GtkDropDown *mode, *ending, *crc;
    GtkSpinButton *delay;
    GtkWidget *box;
} SequenceRow;

typedef struct {
    TioTab *tab;
    GtkWidget *window;
    GtkEntry *name;
    GtkDropDown *saved;
    GtkBox *rows_box;
    GPtrArray *rows;
    GtkCheckButton *loop;
    GtkLabel *status;
    GtkButton *pause;
    guint timer;
} SequenceEditor;

static TioSequenceSendResult sequence_send(const GByteArray *bytes, gpointer data)
{
    TioTab *tab = data;
    if (tab->child_pid <= 0 || !tab->raw || tab->raw->send_failed ||
        (tab->connection_checked && !tab->observed_connected)) return TIO_SEQUENCE_ERROR;
    if (tab->raw->sending) return TIO_SEQUENCE_WAIT;
    if (!bytes->len) return TIO_SEQUENCE_ACCEPT;
    return send_payload(tab, bytes) ? TIO_SEQUENCE_ACCEPT : TIO_SEQUENCE_ERROR;
}

static void sequence_row_action(GtkButton *button, gpointer data)
{
    SequenceEditor *editor = data;
    SequenceRow *row = g_object_get_data(G_OBJECT(button), "row");
    gint direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
    guint index;
    if (!g_ptr_array_find(editor->rows, row, &index)) return;
    if (!direction) {
        gtk_box_remove(editor->rows_box, row->box);
        g_ptr_array_remove_index(editor->rows, index);
        return;
    }
    if ((direction < 0 && index == 0) || (direction > 0 && index + 1 == editor->rows->len)) return;
    guint other = direction < 0 ? index - 1 : index + 1;
    gpointer temporary = g_ptr_array_index(editor->rows, other);
    g_ptr_array_index(editor->rows, other) = row;
    g_ptr_array_index(editor->rows, index) = temporary;
    GtkWidget *previous = NULL;
    for (guint i = 0; i < editor->rows->len; ++i) {
        SequenceRow *item = g_ptr_array_index(editor->rows, i);
        gtk_box_reorder_child_after(editor->rows_box, item->box, previous);
        previous = item->box;
    }
}

static void sequence_add_row(SequenceEditor *editor, const TioSequenceStep *step)
{
    if (editor->rows->len >= 256) return;
    SequenceRow *row = g_new0(SequenceRow, 1);
    row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    row->payload = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(row->payload, _("Payload (text escapes or HEX)"));
    gtk_widget_set_hexpand(GTK_WIDGET(row->payload), TRUE);
    const char *modes[] = {_("Text"), "HEX", NULL};
    const char *endings[] = {_("None"), "LF", "CR", "CRLF", NULL};
    const char *crcs[] = {_("None"), "CRC-8", "CRC-16/MODBUS", "CRC-32", NULL};
    row->mode = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes));
    row->ending = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(endings));
    row->crc = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(crcs));
    row->delay = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 60000, 10));
    gtk_widget_set_tooltip_text(GTK_WIDGET(row->delay), _("Delay after sending (ms)"));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->payload));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->mode));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->ending));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->crc));
    gtk_box_append(GTK_BOX(row->box), GTK_WIDGET(row->delay));
    const char *icons[] = {"go-up-symbolic", "go-down-symbolic", "edit-delete-symbolic"};
    const char *tips[] = {_("Move step up"), _("Move step down"), _("Remove step")};
    const gint directions[] = {-1, 1, 0};
    for (guint i = 0; i < 3; ++i) {
        GtkWidget *action = gtk_button_new_from_icon_name(icons[i]);
        gtk_widget_set_tooltip_text(action, tips[i]);
        g_object_set_data(G_OBJECT(action), "row", row);
        g_object_set_data(G_OBJECT(action), "direction", GINT_TO_POINTER(directions[i]));
        g_signal_connect(action, "clicked", G_CALLBACK(sequence_row_action), editor);
        gtk_box_append(GTK_BOX(row->box), action);
    }
    if (step) {
        gtk_editable_set_text(GTK_EDITABLE(row->payload), step->payload);
        gtk_drop_down_set_selected(row->mode, step->mode);
        gtk_drop_down_set_selected(row->ending, step->ending);
        gtk_drop_down_set_selected(row->crc, step->crc);
        gtk_spin_button_set_value(row->delay, step->delay_ms);
    } else gtk_spin_button_set_value(row->delay, 100);
    gtk_box_append(editor->rows_box, row->box);
    g_ptr_array_add(editor->rows, row);
}

static TioSequence *sequence_capture(SequenceEditor *editor)
{
    TioSequence *sequence = tio_sequence_new(gtk_editable_get_text(GTK_EDITABLE(editor->name)));
    for (guint i = 0; i < editor->rows->len; ++i) {
        SequenceRow *row = g_ptr_array_index(editor->rows, i);
        tio_sequence_add(sequence, gtk_editable_get_text(GTK_EDITABLE(row->payload)),
            gtk_drop_down_get_selected(row->mode), gtk_drop_down_get_selected(row->ending),
            gtk_drop_down_get_selected(row->crc), (guint)gtk_spin_button_get_value_as_int(row->delay));
    }
    g_autoptr(GError) error = NULL;
    if (!tio_sequence_validate(sequence, &error)) {
        gtk_label_set_text(editor->status, error->message);
        tio_sequence_free(sequence);
        return NULL;
    }
    return sequence;
}

static void sequence_refresh_saved(SequenceEditor *editor)
{
    GtkStringList *names = gtk_string_list_new(NULL);
    GPtrArray *saved = editor->tab->app->settings.sequences;
    for (guint i = 0; i < saved->len; ++i) {
        const char *text = g_ptr_array_index(saved, i);
        TioSequence *sequence = tio_sequence_decode(text, strlen(text), NULL);
        gtk_string_list_append(names, sequence ? sequence->name : _("Invalid sequence"));
        tio_sequence_free(sequence);
    }
    gtk_drop_down_set_model(editor->saved, G_LIST_MODEL(names));
    g_object_unref(names);
}

static void sequence_clear_rows(SequenceEditor *editor)
{
    for (guint i = 0; i < editor->rows->len; ++i) {
        SequenceRow *row = g_ptr_array_index(editor->rows, i);
        gtk_box_remove(editor->rows_box, row->box);
    }
    g_ptr_array_set_size(editor->rows, 0);
}

static void sequence_action(GtkButton *button, gpointer data)
{
    SequenceEditor *editor = data;
    TioTab *tab = editor->tab;
    const char *action = g_object_get_data(G_OBJECT(button), "sequence-action");
    GPtrArray *saved = tab->app->settings.sequences;
    guint selected = gtk_drop_down_get_selected(editor->saved);
    if (g_str_equal(action, "add")) { sequence_add_row(editor, NULL); return; }
    if (g_str_equal(action, "remove")) {
        if (editor->rows->len) {
            SequenceRow *row = g_ptr_array_index(editor->rows, editor->rows->len - 1);
            gtk_box_remove(editor->rows_box, row->box);
            g_ptr_array_remove_index(editor->rows, editor->rows->len - 1);
        }
        return;
    }
    if (g_str_equal(action, "new")) {
        sequence_clear_rows(editor);
        gtk_editable_set_text(GTK_EDITABLE(editor->name), _("New sequence"));
        sequence_add_row(editor, NULL);
        return;
    }
    if (g_str_equal(action, "load")) {
        if (selected >= saved->len) return;
        const char *text = g_ptr_array_index(saved, selected);
        g_autoptr(GError) error = NULL;
        TioSequence *sequence = tio_sequence_decode(text, strlen(text), &error);
        if (!sequence) { gtk_label_set_text(editor->status, error->message); return; }
        sequence_clear_rows(editor);
        gtk_editable_set_text(GTK_EDITABLE(editor->name), sequence->name);
        for (guint i = 0; i < sequence->steps->len; ++i)
            sequence_add_row(editor, g_ptr_array_index(sequence->steps, i));
        tio_sequence_free(sequence);
        return;
    }
    if (g_str_equal(action, "stop")) {
        g_clear_pointer(&tab->sequence_runner, tio_sequence_runner_free);
        gtk_label_set_text(editor->status, _("Stopped; bytes already sent cannot be recalled"));
        return;
    }
    if (g_str_equal(action, "pause")) {
        tab->sequence_paused = !tab->sequence_paused;
        tio_sequence_runner_pause(tab->sequence_runner, tab->sequence_paused);
        gtk_button_set_label(editor->pause, tab->sequence_paused ? _("Resume") : _("Pause"));
        return;
    }
    if (g_str_equal(action, "delete")) {
        if (selected >= saved->len) return;
        g_ptr_array_remove_index(saved, selected);
        sequence_refresh_saved(editor);
    } else {
        TioSequence *sequence = sequence_capture(editor);
        if (!sequence) return;
        if (g_str_equal(action, "run")) {
            if (tio_sequence_runner_active(tab->sequence_runner) || tab->quick_send_timer) {
                gtk_label_set_text(editor->status, _("Stop the current send before starting another"));
            } else if (!tab->raw || tab->child_pid <= 0) {
                gtk_label_set_text(editor->status, _("Connect this session before running a sequence"));
            } else {
                g_clear_pointer(&tab->sequence_runner, tio_sequence_runner_free);
                tab->sequence_paused = FALSE;
                gtk_button_set_label(editor->pause, _("Pause"));
                tab->sequence_runner = tio_sequence_runner_new(sequence,
                    gtk_check_button_get_active(editor->loop), sequence_send, tab, NULL);
            }
            tio_sequence_free(sequence);
            return;
        }
        guint index;
        for (index = 0; index < saved->len; ++index) {
            const char *text = g_ptr_array_index(saved, index);
            TioSequence *old = tio_sequence_decode(text, strlen(text), NULL);
            gboolean match = old && g_str_equal(old->name, sequence->name);
            tio_sequence_free(old);
            if (match) break;
        }
        if (index == saved->len && saved->len >= 100) {
            gtk_label_set_text(editor->status, _("At most 100 sequences can be saved"));
            tio_sequence_free(sequence);
            return;
        }
        gchar *encoded = tio_sequence_encode(sequence, NULL);
        tio_sequence_free(sequence);
        if (index < saved->len) {
            g_free(g_ptr_array_index(saved, index));
            g_ptr_array_index(saved, index) = encoded;
        } else g_ptr_array_add(saved, encoded);
        sequence_refresh_saved(editor);
        gtk_drop_down_set_selected(editor->saved, index);
    }
    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&tab->app->settings, &error)) gtk_label_set_text(editor->status, error->message);
    else gtk_label_set_text(editor->status, _("Sequences saved"));
}

static gboolean sequence_ui_tick(gpointer data)
{
    SequenceEditor *editor = data;
    TioSequenceRunner *runner = editor->tab->sequence_runner;
    if (!runner) return G_SOURCE_CONTINUE;
    if (!tio_sequence_runner_active(runner)) {
        gtk_label_set_text(editor->status, tio_sequence_runner_failed(runner)
            ? _("Sequence stopped: transport failed") : _("Sequence completed"));
        g_clear_pointer(&editor->tab->sequence_runner, tio_sequence_runner_free);
    } else {
        g_autofree gchar *status = g_strdup_printf(editor->tab->sequence_paused
            ? _("Paused after step %u") : _("Running: %u steps sent in this pass"),
            tio_sequence_runner_step(runner));
        gtk_label_set_text(editor->status, status);
    }
    return G_SOURCE_CONTINUE;
}

static void sequence_editor_free(gpointer data)
{
    SequenceEditor *editor = data;
    if (editor->timer) g_source_remove(editor->timer);
    g_ptr_array_unref(editor->rows);
    g_free(editor);
}

static GtkWidget *sequence_button(SequenceEditor *editor, const char *label, const char *action)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(button), "sequence-action", (gpointer)action);
    g_signal_connect(button, "clicked", G_CALLBACK(sequence_action), editor);
    return button;
}

static void on_analyzer_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    TioTab *tab = data;
    if (!tab->analyzer_window) {
        tab->analyzer_window = tio_analyzer_new(GTK_WINDOW(tab->app->window), tab->log_model);
        g_object_add_weak_pointer(G_OBJECT(tab->analyzer_window), (gpointer *)&tab->analyzer_window);
    } else gtk_window_present(GTK_WINDOW(tab->analyzer_window));
}

static void on_sequences_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    TioTab *tab = data;
    if (tab->sequence_window) { gtk_window_present(GTK_WINDOW(tab->sequence_window)); return; }
    SequenceEditor *editor = g_new0(SequenceEditor, 1);
    editor->tab = tab;
    editor->rows = g_ptr_array_new_with_free_func(g_free);
    editor->window = gtk_window_new();
    tab->sequence_window = editor->window;
    g_object_add_weak_pointer(G_OBJECT(editor->window), (gpointer *)&tab->sequence_window);
    gtk_window_set_title(GTK_WINDOW(editor->window), _("Send sequences"));
    gtk_window_set_transient_for(GTK_WINDOW(editor->window), GTK_WINDOW(tab->app->window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(editor->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(editor->window), 980, 520);
    g_object_set_data_full(G_OBJECT(editor->window), "sequence-editor", editor, sequence_editor_free);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_window_set_child(GTK_WINDOW(editor->window), root);
    GtkWidget *saved = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    editor->saved = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(editor->saved), TRUE);
    gtk_box_append(GTK_BOX(saved), GTK_WIDGET(editor->saved));
    gtk_box_append(GTK_BOX(saved), sequence_button(editor, _("Load"), "load"));
    gtk_box_append(GTK_BOX(saved), sequence_button(editor, _("New"), "new"));
    gtk_box_append(GTK_BOX(saved), sequence_button(editor, _("Delete sequence"), "delete"));
    gtk_box_append(GTK_BOX(root), saved);
    editor->name = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_text(GTK_EDITABLE(editor->name), _("New sequence"));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(editor->name));
    GtkWidget *hint = gtk_label_new(_("Steps run top to bottom: payload, mode, line ending, CRC, delay after send (ms)."));
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_append(GTK_BOX(root), hint);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    editor->rows_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(editor->rows_box));
    gtk_box_append(GTK_BOX(root), scroll);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Add step"), "add"));
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Remove last"), "remove"));
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Save sequence"), "save"));
    editor->loop = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Loop")));
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(editor->loop));
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Run"), "run"));
    editor->pause = GTK_BUTTON(sequence_button(editor, _("Pause"), "pause"));
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(editor->pause));
    gtk_box_append(GTK_BOX(actions), sequence_button(editor, _("Stop"), "stop"));
    gtk_box_append(GTK_BOX(root), actions);
    editor->status = GTK_LABEL(gtk_label_new(_("Disconnect stops the sequence. Closing this editor leaves it running.")));
    gtk_label_set_wrap(editor->status, TRUE);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(editor->status));
    sequence_add_row(editor, NULL);
    sequence_refresh_saved(editor);
    editor->timer = g_timeout_add(100, sequence_ui_tick, editor);
    gtk_window_present(GTK_WINDOW(editor->window));
}

/* --------------------------------------------------------- serial lines */
static gboolean line_command_prompt(gpointer data)
{
    TioTab *tab = data;
    glong column, row;
    vte_terminal_get_cursor_position(tab->terminal, &column, &row);
#if VTE_CHECK_VERSION(0, 78, 0)
    g_autofree gchar *text = vte_terminal_get_text_range_format(tab->terminal,
        VTE_FORMAT_TEXT, MAX(0, row - 1), 0, row, column, NULL);
#else
    g_autofree gchar *text = vte_terminal_get_text_range(tab->terminal,
        MAX(0, row - 1), 0, row, column, NULL, NULL, NULL);
#endif
    if (text && strstr(text, "Enter file name:")) {
        g_autofree gchar *response = g_strconcat(tab->line_command_path, "\r", NULL);
        send_bytes(tab, response, strlen(response));
        g_clear_pointer(&tab->line_command_path, g_free);
        tab->line_command_timer = 0;
        set_status(tab, _("Line command submitted; inspect tio response"));
        return G_SOURCE_REMOVE;
    }
    if (++tab->line_command_attempts >= 100 || tab->child_pid <= 0) {
        g_clear_pointer(&tab->line_command_path, g_free);
        tab->line_command_timer = 0;
        set_status(tab, _("Could not recognize tio prompt; disconnect to cancel the command"));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void on_line_control(GtkButton *button, gpointer data)
{
    TioTab *tab = data;
    if (tab->child_pid <= 0) { set_status(tab, _("Connect before controlling serial lines")); return; }
    if (tio_transfer_active(tab->transfer) || tab->line_command_timer || tio_sequence_runner_active(tab->sequence_runner) ||
        tab->quick_send_timer || (tab->raw && tab->raw->sending)) {
        set_status(tab, _("Stop pending sends before controlling serial lines"));
        return;
    }
    const char *action = g_object_get_data(G_OBJECT(button), "line-action");
    if (g_str_equal(action, "break")) { send_bytes(tab, "\x14" "b", 2); return; }
    if (g_str_equal(action, "dtr-pulse")) { send_bytes(tab, "\x14" "p0", 3); return; }
    if (g_str_equal(action, "rts-pulse")) { send_bytes(tab, "\x14" "p1", 3); return; }
    guint line = g_str_has_prefix(action, "dtr") ? 0u : 1u;
    gboolean high = g_str_has_suffix(action, "high");
    g_autofree gchar *script = tio_line_script(line, high);
    g_autofree gchar *directory = g_build_filename(g_get_user_runtime_dir(), "tio-gui", NULL);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        set_status(tab, _("Could not create runtime directory")); return;
    }
    g_autofree gchar *path = g_build_filename(directory, "line-XXXXXX.lua", NULL);
    /* mkstemp requires the random suffix at the end. tio does not need .lua. */
    path[strlen(path) - 4] = '\0';
    if (strpbrk(path, "\r\n") || strlen(path) >= 4000) {
        set_status(tab, _("Runtime path cannot be used in a tio command")); return;
    }
    int fd = g_mkstemp(path);
    if (fd < 0) { set_status(tab, _("Could not create line command")); return; }
    close(fd);
    g_autoptr(GError) error = NULL;
    if (!g_file_set_contents(path, script, -1, &error)) {
        g_unlink(path); set_status(tab, error->message); return;
    }
    if (!tab->line_script_paths) tab->line_script_paths = g_ptr_array_new_with_free_func(g_free);
    /* These tiny files remain until disconnect so tio can open them after its prompt. */
    if (tab->line_script_paths->len >= 1024) {
        g_unlink(path); set_status(tab, _("Reconnect before issuing more line commands")); return;
    }
    g_ptr_array_add(tab->line_script_paths, g_strdup(path));
    tab->line_command_path = g_steal_pointer(&path);
    tab->line_command_attempts = 0;
    send_bytes(tab, "\x14" "r", 2);
    tab->line_command_timer = g_timeout_add(50, line_command_prompt, tab);
}

/* ----------------------------------------------------------- file transfer */
typedef struct { GWeakRef window; guint64 tab_id; gboolean receive; } TransferRequest;
static gboolean transfer_tick(gpointer data)
{
    TioTab *tab = data;
    gtk_label_set_text(tab->transfer_status, tio_transfer_status(tab->transfer));
    if (tio_transfer_active(tab->transfer)) return G_SOURCE_CONTINUE;
    vte_terminal_set_input_enabled(tab->terminal, TRUE);
    tab->transfer_timer = 0;
    return G_SOURCE_REMOVE;
}
static void transfer_file_selected(GObject *source, GAsyncResult *result, gpointer data)
{
    TransferRequest *request = data;
    g_autoptr(GObject) window = g_weak_ref_get(&request->window);
    guint64 id = request->tab_id; gboolean receive = request->receive;
    g_weak_ref_clear(&request->window); g_free(request);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = receive ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error)
                                  : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!window || !gtk_widget_get_visible(GTK_WIDGET(window)) || !file) return;
    TioApp *app = g_object_get_data(window, "tio-gui");
    TioTab *tab = NULL;
    for (guint i = 0; app && i < app->tabs->len; ++i) {
        TioTab *candidate = g_ptr_array_index(app->tabs, i);
        if (candidate->id == id) { tab = candidate; break; }
    }
    if (!tab) return;
    if (!tab->raw || !tab->observed_connected || tio_transfer_active(tab->transfer) ||
        tab->line_command_timer || tab->quick_send_timer || tab->raw->sending || tio_sequence_runner_active(tab->sequence_runner)) {
        set_status(tab, _("Connect and stop pending sends before transferring files")); return;
    }
    g_autofree gchar *path = g_file_get_path(file);
    g_clear_pointer(&tab->transfer, tio_transfer_free);
    tab->transfer = tio_transfer_start(tab->socket_path, path,
        gtk_drop_down_get_selected(tab->transfer_protocol), receive,
        (guint)gtk_spin_button_get_value_as_int(tab->transfer_timeout), &error);
    if (!tab->transfer) { gtk_label_set_text(tab->transfer_status, error->message); return; }
    vte_terminal_set_input_enabled(tab->terminal, FALSE);
    if (tab->transfer_timer) g_source_remove(tab->transfer_timer);
    tab->transfer_timer = g_timeout_add(100, transfer_tick, tab);
    transfer_tick(tab);
    tio_log_model_command(tab->log_model, receive ? "Receive file transfer started" : "Send file transfer started", g_get_real_time());
}
static void transfer_choose(GtkButton *button, gpointer data)
{
    TioTab *tab = data;
    if (!tab->observed_connected) { set_status(tab, _("Connect before transferring files")); return; }
    TransferRequest *request = g_new0(TransferRequest, 1);
    request->tab_id = tab->id;
    request->receive = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "receive"));
    g_weak_ref_init(&request->window, tab->app->window);
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, request->receive ? _("Choose an empty receive directory") : _("Choose a file to send"));
    if (request->receive) gtk_file_dialog_select_folder(dialog, GTK_WINDOW(tab->app->window), NULL, transfer_file_selected, request);
    else gtk_file_dialog_open(dialog, GTK_WINDOW(tab->app->window), NULL, transfer_file_selected, request);
}
static void transfer_cancel_clicked(GtkButton *button, gpointer data)
{
    (void)button; TioTab *tab = data; tio_transfer_cancel(tab->transfer);
}
static GtkWidget *transfer_controls_new(TioTab *tab)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *protocols[] = {"XMODEM-CRC", "YMODEM", "ZMODEM", NULL};
    tab->transfer_protocol = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(protocols));
    tab->transfer_timeout = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5, 86400, 5));
    gtk_spin_button_set_value(tab->transfer_timeout, 300);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(tab->transfer_protocol));
    gtk_box_append(GTK_BOX(row), gtk_label_new(_("Timeout (s)")));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(tab->transfer_timeout));
    const char *labels[] = {_("Send file…"), _("Receive files…"), _("Cancel transfer")};
    for (guint i = 0; i < 3; ++i) {
        GtkWidget *button = gtk_button_new_with_label(labels[i]);
        g_object_set_data(G_OBJECT(button), "receive", GINT_TO_POINTER(i == 1));
        g_signal_connect(button, "clicked", i == 2 ? G_CALLBACK(transfer_cancel_clicked) : G_CALLBACK(transfer_choose), tab);
        gtk_box_append(GTK_BOX(row), button);
    }
    gtk_box_append(GTK_BOX(box), row);
    tab->transfer_status = GTK_LABEL(gtk_label_new(_("Start the matching protocol on the peer. XMODEM receives as received.bin; its final block may contain padding.")));
    gtk_label_set_wrap(tab->transfer_status, TRUE);
    gtk_label_set_xalign(tab->transfer_status, 0);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->transfer_status));
    return box;
}

/* -------------------------------------------------------------- recording */
typedef struct { GWeakRef window; guint64 tab_id; } CaptureRequest;

static gboolean capture_ui_tick(gpointer data)
{
    TioTab *tab = data;
    const char *error = tio_capture_error(tab->capture);
    const char *path = tio_capture_path(tab->capture);
    g_autofree gchar *message = error ? g_strdup_printf(_("Recording failed: %s"), error)
        : g_strdup_printf(tio_capture_finished(tab->capture) ? _("Recording saved: %s") : _("Recording: %s"), path ? path : "");
    gtk_label_set_text(tab->capture_label, message);
    if (tio_capture_finished(tab->capture)) {
        gtk_button_set_label(tab->capture_button, _("Start recording…"));
        tab->capture_timer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void capture_file_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    CaptureRequest *request = data;
    g_autoptr(GObject) window = g_weak_ref_get(&request->window);
    guint64 id = request->tab_id;
    g_weak_ref_clear(&request->window); g_free(request);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!window || !gtk_widget_get_visible(GTK_WIDGET(window))) return;
    TioApp *app = g_object_get_data(window, "tio-gui");
    if (!app || !file) return;
    TioTab *tab = NULL;
    for (guint i = 0; i < app->tabs->len; ++i) {
        TioTab *candidate = g_ptr_array_index(app->tabs, i);
        if (candidate->id == id) { tab = candidate; break; }
    }
    if (!tab) return;
    if (tab->child_pid <= 0) { set_status(tab, _("Connect before starting a recording")); return; }
    if (tab->capture && !tio_capture_finished(tab->capture)) { set_status(tab, _("A recording is already active")); return; }
    g_autofree gchar *path = g_file_get_path(file);
    if (!path) { set_status(tab, _("Choose a local capture file")); return; }
    guint part = (guint)gtk_spin_button_get_value_as_int(tab->capture_part_spin);
    guint seconds = (guint)gtk_spin_button_get_value_as_int(tab->capture_time_spin);
    guint keep = (guint)gtk_spin_button_get_value_as_int(tab->capture_keep_spin);
    guint disk = (guint)gtk_spin_button_get_value_as_int(tab->capture_disk_spin);
    TioCapture *capture = tio_capture_new(path, (guint64)part * 1024 * 1024, seconds, keep,
        (guint64)disk * 1024 * 1024, tab->running_metadata, &error);
    if (!capture) { gtk_label_set_text(tab->capture_label, error->message); return; }
    g_clear_pointer(&tab->capture, tio_capture_unref);
    tab->capture = capture;
    tab->config.capture_part_mb = part; tab->config.capture_part_seconds = seconds;
    tab->config.capture_keep_files = keep; tab->config.capture_disk_mb = disk;
    const char *metadata = tab->running_metadata ? tab->running_metadata : "";
    tio_capture_record(capture, TIO_CAPTURE_PARAMETERS, (const guint8 *)metadata, strlen(metadata), g_get_real_time());
    if (tab->observed_device) tio_capture_record(capture, TIO_CAPTURE_CONNECT,
        (const guint8 *)tab->observed_device, strlen(tab->observed_device), g_get_real_time());
    gtk_button_set_label(tab->capture_button, _("Stop recording"));
    if (tab->capture_timer) g_source_remove(tab->capture_timer);
    tab->capture_timer = g_timeout_add(200, capture_ui_tick, tab);
}

static void on_capture_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    TioTab *tab = data;
    if (tab->capture && !tio_capture_finished(tab->capture)) {
        tio_capture_stop(tab->capture);
        gtk_button_set_label(tab->capture_button, _("Finishing recording…"));
        return;
    }
    if (tab->child_pid <= 0) { set_status(tab, _("Connect before starting a recording")); return; }
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_autofree gchar *name = g_strdup_printf("serial-%s.tiocap", stamp);
    gtk_file_dialog_set_initial_name(dialog, name);
    CaptureRequest *request = g_new0(CaptureRequest, 1);
    g_weak_ref_init(&request->window, tab->app->window); request->tab_id = tab->id;
    gtk_file_dialog_save(dialog, GTK_WINDOW(tab->app->window), NULL, capture_file_finished, request);
}

static void on_terminal_commit(VteTerminal *terminal, const char *text, guint length, gpointer data)
{
    (void)terminal;
    TioTab *tab = data;
    if (tab->capture) tio_capture_record(tab->capture, TIO_CAPTURE_INPUT, (const guint8 *)text, length, g_get_real_time());
}

static GtkWidget *capture_controls_new(TioTab *tab)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *limits = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tab->capture_part_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 128, 1));
    tab->capture_time_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 86400, 60));
    tab->capture_keep_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 1000, 1));
    tab->capture_disk_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 1048576, 16));
    GtkSpinButton *spins[] = {tab->capture_part_spin, tab->capture_time_spin, tab->capture_keep_spin, tab->capture_disk_spin};
    const char *labels[] = {_("Part MiB"), _("Part seconds (0=off)"), _("Keep parts"), _("Disk MiB")};
    for (guint i = 0; i < 4; ++i) {
        gtk_box_append(GTK_BOX(limits), gtk_label_new(labels[i]));
        gtk_box_append(GTK_BOX(limits), GTK_WIDGET(spins[i]));
    }
    gtk_box_append(GTK_BOX(box), limits);
    tab->capture_button = GTK_BUTTON(gtk_button_new_with_label(_("Start recording…")));
    g_signal_connect(tab->capture_button, "clicked", G_CALLBACK(on_capture_clicked), tab);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->capture_button));
    tab->capture_label = GTK_LABEL(gtk_label_new(_("Records exact RX/TX bytes, terminal input, connection events and serial parameters. Old closed parts expire at the configured limits.")));
    gtk_label_set_wrap(tab->capture_label, TRUE); gtk_label_set_selectable(tab->capture_label, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->capture_label));
    return box;
}

static GtkWidget *reconnect_controls_new(TioTab *tab)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tab->reconnect_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Automatic reconnect")));
    const char *strategies[] = {_("Same device"), _("Next new device"), _("Latest device"), NULL};
    tab->auto_connect_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(strategies));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(tab->reconnect_check));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(tab->auto_connect_dropdown));
    gtk_box_append(GTK_BOX(box), row);
    tab->exclude_devices_entry = GTK_ENTRY(gtk_entry_new());
    tab->exclude_drivers_entry = GTK_ENTRY(gtk_entry_new());
    tab->exclude_tids_entry = GTK_ENTRY(gtk_entry_new());
    GtkEntry *entries[] = {tab->exclude_devices_entry, tab->exclude_drivers_entry, tab->exclude_tids_entry};
    const char *labels[] = {_("Exclude devices"), _("Exclude drivers"), _("Exclude topology IDs")};
    for (guint i = 0; i < 3; ++i) {
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(row), make_label(labels[i]));
        gtk_widget_set_hexpand(GTK_WIDGET(entries[i]), TRUE);
        gtk_entry_set_placeholder_text(entries[i], _("Comma-separated patterns; * and ? supported"));
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(entries[i]));
        gtk_box_append(GTK_BOX(box), row);
    }
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tab->connection_notify_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Desktop notifications")));
    tab->connection_sound_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Connection sound")));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(tab->connection_notify_check));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(tab->connection_sound_check));
    gtk_box_append(GTK_BOX(box), row);
    GtkWidget *hint = gtk_label_new(_("Strategy changes apply on the next connection. New/latest lets tio choose a port. Reconnect counts reflect observed transitions."));
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_append(GTK_BOX(box), hint);
    return box;
}

static GtkWidget *line_controls_new(TioTab *tab)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *defaults = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *states[] = {_("Unchanged"), _("Low"), _("High"), NULL};
    tab->dtr_default = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(states));
    tab->rts_default = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(states));
    tab->line_pulse_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 10000, 10));
    gtk_box_append(GTK_BOX(defaults), make_label(_("DTR on connect")));
    gtk_box_append(GTK_BOX(defaults), GTK_WIDGET(tab->dtr_default));
    gtk_box_append(GTK_BOX(defaults), make_label(_("RTS on connect")));
    gtk_box_append(GTK_BOX(defaults), GTK_WIDGET(tab->rts_default));
    gtk_box_append(GTK_BOX(defaults), make_label(_("Pulse (ms)")));
    gtk_box_append(GTK_BOX(defaults), GTK_WIDGET(tab->line_pulse_spin));
    gtk_box_append(GTK_BOX(box), defaults);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *names[] = {_("Send Break"), _("DTR Low"), _("DTR High"), _("DTR Pulse"),
                           _("RTS Low"), _("RTS High"), _("RTS Pulse")};
    const char *ids[] = {"break", "dtr-low", "dtr-high", "dtr-pulse", "rts-low", "rts-high", "rts-pulse"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) {
        GtkWidget *button = gtk_button_new_with_label(names[i]);
        g_object_set_data(G_OBJECT(button), "line-action", (gpointer)ids[i]);
        g_signal_connect(button, "clicked", G_CALLBACK(on_line_control), tab);
        gtk_box_append(GTK_BOX(actions), button);
    }
    gtk_box_append(GTK_BOX(box), actions);
    GtkWidget *rs485 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tab->rs485_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("RS-485")));
    tab->rs485_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(tab->rs485_entry, "RTS_ON_SEND=1,RTS_AFTER_SEND=0,RX_DURING_TX");
    gtk_widget_set_hexpand(GTK_WIDGET(tab->rs485_entry), TRUE);
    gtk_box_append(GTK_BOX(rs485), GTK_WIDGET(tab->rs485_check));
    gtk_box_append(GTK_BOX(rs485), GTK_WIDGET(tab->rs485_entry));
    gtk_box_append(GTK_BOX(box), rs485);
    GtkWidget *hint = gtk_label_new(_("Defaults, pulse duration and RS-485 apply on the next connection. Hardware support varies by adapter."));
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_append(GTK_BOX(box), hint);
    return box;
}

/* ---------------------------------------------------------------- logging */

static void on_log_toggled(GtkCheckButton *button, gpointer user_data)
{
    TioTab *tab = user_data;
    gboolean enabled = gtk_check_button_get_active(button);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->log_directory_entry), enabled);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->choose_log_directory_button), enabled);
    if (enabled) {
        gtk_check_button_set_active(tab->timestamp_check, TRUE);
    }
    update_session_label(tab);
}

static void on_open_log_directory(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;

    const char *directory = gtk_editable_get_text(GTK_EDITABLE(tab->log_directory_entry));
    if (directory[0] == '\0') {
        set_status(tab, _("Choose a log directory first"));
        return;
    }
    if (g_mkdir_with_parents(directory, 0750) == -1) {
        g_autofree gchar *message =
            g_strdup_printf(_("Could not create log directory: %s"), g_strerror(errno));
        set_status(tab, message);
        return;
    }

    g_autoptr(GFile) file = g_file_new_for_path(directory);
    g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, GTK_WINDOW(tab->app->window), NULL, NULL, NULL);
}

/* -------------------------------------------------------------- shortcuts */

static void action_clear_terminal(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    if (tab == NULL) {
        return;
    }
    vte_terminal_reset(tab->terminal, TRUE, TRUE);
    tio_highlighter_clear(tab->highlighter);
}

static void action_copy(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    if (tab == NULL) {
        return;
    }
    if (gtk_check_button_get_active(tab->highlight_toggle)) {
        gtk_text_buffer_copy_clipboard(gtk_text_view_get_buffer(tab->highlight_view),
                                       gtk_widget_get_clipboard(GTK_WIDGET(tab->highlight_view)));
    } else {
        vte_terminal_copy_clipboard_format(tab->terminal, VTE_FORMAT_TEXT);
    }
}

static void action_paste(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    if (tab == NULL) {
        return;
    }
    vte_terminal_paste_clipboard(tab->terminal);
}

static void action_connect(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    if (tab == NULL) {
        return;
    }
    if (tab->child_pid <= 0) {
        connect_tio(tab);
    }
}

static void action_disconnect(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    if (app->active != NULL) {
        disconnect_tio(app->active);
    }
}

static void action_search(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    if (tab == NULL) {
        return;
    }

    gboolean active = !gtk_search_bar_get_search_mode(tab->search_bar);
    gtk_search_bar_set_search_mode(tab->search_bar, active);
    if (active) {
        gtk_widget_grab_focus(GTK_WIDGET(tab->search_entry));
    } else {
        focus_log_view(tab);
    }
}

static void install_shortcuts(GtkApplication *application, TioApp *app)
{
    static const GActionEntry entries[] = {
        {.name = "clear-terminal", .activate = action_clear_terminal},
        {.name = "copy", .activate = action_copy},
        {.name = "paste", .activate = action_paste},
        {.name = "connect", .activate = action_connect},
        {.name = "disconnect", .activate = action_disconnect},
        {.name = "search", .activate = action_search},
        {.name = "new-tab", .activate = action_new_tab},
        {.name = "close-tab", .activate = action_close_tab},
        {.name = "next-tab", .activate = action_next_tab},
        {.name = "previous-tab", .activate = action_previous_tab},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app->window),
                                    entries,
                                    G_N_ELEMENTS(entries),
                                    app);

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
        {"win.new-tab", "<Control>t"},
        {"win.close-tab", "<Control>w"},
        {"win.next-tab", "<Control>Page_Down"},
        {"win.previous-tab", "<Control>Page_Up"},
    };
    for (gsize index = 0; index < G_N_ELEMENTS(accelerators); ++index) {
        const char *keys[] = {accelerators[index].accelerator, NULL};
        gtk_application_set_accels_for_action(application, accelerators[index].action, keys);
    }
}

/* ------------------------------------------------------------------- exit */

static gboolean close_after_capture(gpointer data)
{
    TioApp *app = data;
    for (guint i = 0; i < app->tabs->len; ++i) {
        TioTab *tab = g_ptr_array_index(app->tabs, i);
        if (tab->spawn_pending || !tio_capture_finished(tab->capture)) return G_SOURCE_CONTINUE;
    }
    app->close_capture_timer = 0;
    gtk_widget_set_sensitive(app->window, TRUE);
    gtk_window_close(GTK_WINDOW(app->window));
    return G_SOURCE_REMOVE;
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data)
{
    (void)window;
    TioApp *app = user_data;

    /* Stop every session, not just the visible one. */
    for (guint index = 0; index < app->tabs->len; ++index) {
        TioTab *tab = g_ptr_array_index(app->tabs, index);
        tab->close_requested = TRUE;
        if (tab->child_pid > 0) {
            (void)kill(tab->child_pid, SIGHUP);
        }
        stop_session_timer(tab);
        raw_tap_stop(tab);
        if (!tab->spawn_pending) g_clear_pointer(&tab->spawn_argv, g_strfreev);
    }

    gboolean draining = FALSE;
    for (guint i = 0; i < app->tabs->len; ++i) {
        TioTab *tab = g_ptr_array_index(app->tabs, i);
        draining |= tab->spawn_pending || !tio_capture_finished(tab->capture);
    }
    if (draining) {
        gtk_widget_set_sensitive(app->window, FALSE);
        if (!app->close_capture_timer) app->close_capture_timer = g_timeout_add(50, close_after_capture, app);
        return TRUE;
    }

    if (app->active != NULL) {
        capture_all_settings(app->active);
    }

    /* Record the sessions themselves, in page order, so they can come back. */
    tio_settings_clear_tabs(&app->settings);
    if (app->settings.restore_tabs) {
        for (guint index = 0; index < app->tabs->len; ++index) {
            TioTab *tab = g_ptr_array_index(app->tabs, index);
            TioSessionConfig snapshot;
            tio_session_config_init(&snapshot);
            capture_session_config(tab, &snapshot);
            tio_settings_add_tab(&app->settings, &snapshot);
            tio_session_config_clear(&snapshot);
        }
    }

    g_autoptr(GError) error = NULL;
    if (!tio_settings_save(&app->settings, &error)) {
        g_warning("Could not save settings: %s", error->message);
    }
    return FALSE;
}

static void on_window_destroy(GtkWidget *window, gpointer user_data)
{
    (void)window;
    g_object_set_data(G_OBJECT(user_data), "tio-gui-window", NULL);
}

static void tio_tab_free(TioTab *tab)
{
    if (tab == NULL) {
        return;
    }

    if (tab->sequence_window) gtk_window_destroy(GTK_WINDOW(tab->sequence_window));
    if (tab->analyzer_window) gtk_window_destroy(GTK_WINDOW(tab->analyzer_window));
    g_clear_pointer(&tab->log_model, tio_log_model_free);
    stop_session_timer(tab);
    raw_tap_stop(tab);
    if (tab->deferred_close_timer) g_source_remove(tab->deferred_close_timer);
    if (tab->transfer_timer) g_source_remove(tab->transfer_timer);
    g_clear_pointer(&tab->transfer, tio_transfer_free);
    if (tab->capture_timer) g_source_remove(tab->capture_timer);
    g_clear_pointer(&tab->capture, tio_capture_unref);
    g_clear_pointer(&tab->running_metadata, g_free);
    g_clear_pointer(&tab->highlighter, tio_highlighter_free);
    g_clear_pointer(&tab->spawn_argv, g_strfreev);
    g_clear_pointer(&tab->log_path, g_free);
    g_clear_pointer(&tab->history_draft, g_free);
    g_clear_pointer(&tab->search_pattern, g_free);
    g_clear_pointer(&tab->pending_profile_delete, g_free);
    g_clear_pointer(&tab->observed_device, g_free);
    g_clear_pointer(&tab->disconnect_reason, g_free);
    tio_session_config_clear(&tab->config);
    g_free(tab);
}

/* Owned by the window, so it runs after every session has been torn down. */
static void tio_app_free(gpointer data)
{
    TioApp *app = data;

    if (app->close_capture_timer) g_source_remove(app->close_capture_timer);
    if (app->settings_preview_timer != 0) {
        g_source_remove(app->settings_preview_timer);
        app->settings_preview_timer = 0;
    }
    if (app->tabs != NULL) {
        for (guint index = 0; index < app->tabs->len; ++index) {
            tio_tab_free(g_ptr_array_index(app->tabs, index));
        }
        g_clear_pointer(&app->tabs, g_ptr_array_unref);
    }
    app->active = NULL;
    g_clear_pointer(&app->update_url, g_free);
    g_clear_pointer(&app->latest_version, g_free);
    tio_settings_clear(&app->settings);
    g_free(app);
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
        "textview.highlight-view, textview.highlight-view text {"
        "  background-color: #0d1117;"
        "  color: #c9d1d9;"
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

static void capture_all_settings(TioTab *tab)
{
    capture_session_config(tab, &tab->config);
    tio_session_config_copy(&tab->app->settings.defaults, &tab->config);
    tab->app->settings.show_all_ttys = gtk_check_button_get_active(tab->app->show_all_ttys_check);
    tab->app->settings.restore_tabs =
        gtk_check_button_get_active(tab->app->restore_tabs_check);
    tab->app->settings.advanced_expanded =
        gtk_expander_get_expanded(GTK_EXPANDER(tab->session_settings_expander));
    tab->app->settings.log_warning_mb =
        (guint)gtk_spin_button_get_value_as_int(tab->app->log_warning_spin);
}

static void on_github_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    GtkUriLauncher *launcher =
        gtk_uri_launcher_new("https://github.com/keithxc/tio-gui");
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(tab->app->window), NULL, NULL, NULL);
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
    TioApp *app = g_object_get_data(G_OBJECT(check->window), "tio-gui");
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) body =
        soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
    if (app == NULL || body == NULL ||
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
        g_free(app->latest_version);
        app->latest_version = g_strdup(tag);
        g_free(app->update_url);
        app->update_url = g_strdup(url);
        g_autofree gchar *message = g_strdup_printf(_("Version %s is available"), tag);
        gtk_label_set_text(app->update_available_label, message);
        gtk_widget_set_visible(GTK_WIDGET(app->update_available_label), TRUE);
        gtk_widget_set_visible(GTK_WIDGET(app->download_update_button), TRUE);
    }
    update_check_free(check);
}

static void on_settings_popover_visible(GObject *object,
                                        GParamSpec *pspec,
                                        gpointer user_data)
{
    (void)pspec;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    if (!gtk_widget_get_visible(GTK_WIDGET(object)) || tab->app->update_check_started) {
        return;
    }
    tab->app->update_check_started = TRUE;
    SoupSession *session = soup_session_new();
    SoupMessage *message = soup_message_new(
        "GET", "https://api.github.com/repos/keithxc/tio-gui/releases/latest");
    SoupMessageHeaders *headers = soup_message_get_request_headers(message);
    soup_message_headers_append(headers, "Accept", "application/vnd.github+json");
    soup_message_headers_append(headers, "User-Agent", "tio-gui");
    soup_message_headers_append(headers, "X-GitHub-Api-Version", "2022-11-28");
    UpdateCheck *check = g_new0(UpdateCheck, 1);
    check->window = g_object_ref(tab->app->window);
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
    TioApp *app = user_data;
    TioTab *tab = app->active;
    if (tab->app->update_url == NULL) {
        return;
    }
    GtkUriLauncher *launcher = gtk_uri_launcher_new(tab->app->update_url);
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(tab->app->window), NULL, NULL, NULL);
    g_object_unref(launcher);
}
/* -------------------------------------------------------------- diagnostics */
typedef struct {
    GtkWindow *window;
    GSubprocess *process;
    guint timeout;
} AboutVersion;

static gchar *build_diagnostics(const char *tio_version)
{
    struct utsname system = {0};
    (void)uname(&system);
    return g_strdup_printf("tio-gui %s\n%s\nGTK %u.%u.%u\nVTE %u.%u.%u\n%s %s (%s)\nLicense: GPL-3.0-only\n",
        TIO_GUI_VERSION, tio_version, gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(),
        vte_get_major_version(), vte_get_minor_version(), vte_get_micro_version(),
        system.sysname, system.release, system.machine);
}

static void about_set_diagnostics(GtkWindow *window, const char *version)
{
    GtkTextBuffer *buffer = g_object_get_data(G_OBJECT(window), "diagnostic-buffer");
    g_autofree gchar *text = build_diagnostics(version);
    gtk_text_buffer_set_text(buffer, text, -1);
}

static gboolean about_version_timeout(gpointer data)
{
    AboutVersion *check = data;
    check->timeout = 0;
    g_subprocess_force_exit(check->process);
    return G_SOURCE_REMOVE;
}

static void about_version_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    AboutVersion *check = data;
    g_autofree gchar *output = NULL;
    g_autofree gchar *errors = NULL;
    g_autoptr(GError) error = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, &errors, &error);
    if (check->timeout) g_source_remove(check->timeout);
    if (gtk_widget_get_visible(GTK_WIDGET(check->window))) {
        if (ok && g_subprocess_get_successful(check->process) && output && strlen(output) < 1024)
            about_set_diagnostics(check->window, g_strstrip(output));
        else about_set_diagnostics(check->window, _("tio version unavailable"));
    }
    g_object_unref(check->process);
    g_object_unref(check->window);
    g_free(check);
}

static void copy_diagnostics(GtkButton *button, gpointer data)
{
    (void)button;
    GtkWindow *window = data;
    GtkTextBuffer *buffer = g_object_get_data(G_OBJECT(window), "diagnostic-buffer");
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    g_autofree gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(window)), text);
}

static void on_about_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    TioApp *app = data;
    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), _("About tio-gui"));
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(app->window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 340);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(window), box);
    GtkWidget *title = gtk_label_new("tio-gui");
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), gtk_label_new(_("Serial debugging for embedded development")));
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    g_object_set_data(G_OBJECT(window), "diagnostic-buffer", buffer);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_box_append(GTK_BOX(box), scroll);
    GtkWidget *links = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(links), gtk_link_button_new_with_label("https://github.com/keithxc/tio-gui", _("GitHub project")));
    gtk_box_append(GTK_BOX(links), gtk_link_button_new_with_label("https://www.gnu.org/licenses/gpl-3.0.html", "GPL-3.0-only"));
    GtkWidget *copy = gtk_button_new_with_label(_("Copy diagnostics"));
    gtk_box_append(GTK_BOX(links), copy);
    g_signal_connect(copy, "clicked", G_CALLBACK(copy_diagnostics), window);
    gtk_box_append(GTK_BOX(box), links);
    about_set_diagnostics(GTK_WINDOW(window), _("Checking tio version…"));
    gtk_window_present(GTK_WINDOW(window));
    g_autoptr(GError) error = NULL;
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                          &error, "tio", "--version", NULL);
    if (!process) { about_set_diagnostics(GTK_WINDOW(window), _("tio was not found in PATH")); return; }
    AboutVersion *check = g_new0(AboutVersion, 1);
    check->window = g_object_ref(GTK_WINDOW(window));
    check->process = process;
    check->timeout = g_timeout_add_seconds(5, about_version_timeout, check);
    g_subprocess_communicate_utf8_async(process, NULL, NULL, about_version_finished, check);
}

static void on_export_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GtkWindow) window = user_data;
    TioApp *app = g_object_get_data(G_OBJECT(window), "tio-gui");
    TioTab *tab = app ? app->active : NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!app || !gtk_widget_get_visible(GTK_WIDGET(window)) || !tab) return;
    if (file == NULL) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            set_status(tab, error->message);
        }
        return;
    }

    g_autofree gchar *path = g_file_get_path(file);
    if (path == NULL) {
        set_status(tab, _("Choose a local file for settings export"));
        return;
    }
    capture_all_settings(tab);
    gboolean portable = GPOINTER_TO_INT(g_object_get_data(source, "portable"));
    gboolean saved = portable ? tio_settings_export_portable(&app->settings, path, &error)
                              : tio_settings_save_to_file(&app->settings, path, &error);
    if (!saved) {
        set_status(tab, error->message);
        return;
    }
    set_status(tab, _("Settings exported"));
}

static void on_export_settings_clicked(GtkButton *button, gpointer user_data)
{

    TioApp *app = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gboolean portable = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "portable"));
    g_object_set_data(G_OBJECT(dialog), "portable", GINT_TO_POINTER(portable));
    gtk_file_dialog_set_title(dialog, portable ? _("Export portable settings") : _("Export settings"));
    gtk_file_dialog_set_initial_name(dialog, "tio-gui-settings.ini");
    gtk_file_dialog_save(dialog,
                         GTK_WINDOW(app->window),
                         NULL,
                         on_export_finished,
                         g_object_ref(app->window));
    g_object_unref(dialog);
}

static void apply_imported_settings(TioTab *tab)
{
    apply_theme(tab->app->settings.theme);
    gtk_drop_down_set_selected(tab->app->theme_dropdown,
                               value_index(theme_values, tab->app->settings.theme, 0));
    gtk_drop_down_set_selected(tab->app->language_dropdown,
                               value_index(language_values, tab->app->settings.language, 0));
    gtk_check_button_set_active(tab->app->show_all_ttys_check, tab->app->settings.show_all_ttys);
    gtk_check_button_set_active(tab->app->restore_tabs_check, tab->app->settings.restore_tabs);
    gtk_spin_button_set_value(tab->app->log_warning_spin, tab->app->settings.log_warning_mb);
    /* An imported file replaces the defaults; the open session adopts them. */
    tio_session_config_copy(&tab->config, &tab->app->settings.defaults);
    apply_session_config(tab, &tab->config);
    update_quick_buttons(tab);
    refresh_profile_ui(tab);
    refresh_history_ui(tab);
    refresh_devices(tab);
    g_unsetenv("TIO_GUI_LANGUAGE");
    if (apply_language(tab->app->settings.language)) {
        retranslate_ui(tab);
    }
}

static void on_import_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GtkWindow) window = user_data;
    TioApp *app = g_object_get_data(G_OBJECT(window), "tio-gui");
    TioTab *tab = app ? app->active : NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!app || !gtk_widget_get_visible(GTK_WIDGET(window)) || !tab) return;
    if (file == NULL) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            set_status(tab, error->message);
        }
        return;
    }

    g_autofree gchar *path = g_file_get_path(file);
    if (path == NULL) {
        set_status(tab, _("Choose a local settings file"));
        return;
    }

    for (guint i = 0; i < app->tabs->len; ++i) {
        TioTab *other = g_ptr_array_index(app->tabs, i);
        if (other->child_pid > 0) { set_status(tab, _("Disconnect all sessions before importing settings")); return; }
    }
    TioSettings imported;
    tio_settings_init(&imported);
    if (!tio_settings_load_from_file(&imported, path, &error)) {
        tio_settings_clear(&imported);
        set_status(tab, error->message);
        return;
    }
    tio_settings_clear(&tab->app->settings);
    tab->app->settings = imported;
    for (guint i = 0; i < app->tabs->len; ++i) {
        TioTab *other = g_ptr_array_index(app->tabs, i);
        if (other->sequence_window) gtk_window_destroy(GTK_WINDOW(other->sequence_window));
        apply_imported_settings(other);
    }
    if (!tio_settings_save(&tab->app->settings, &error)) {
        set_status(tab, error->message);
        return;
    }
    set_status(tab, _("Settings imported"));
}

static void on_import_settings_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioApp *app = user_data;
    TioTab *tab = app->active;
    for (guint i = 0; i < app->tabs->len; ++i) {
        TioTab *other = g_ptr_array_index(app->tabs, i);
        if (other->child_pid > 0) { set_status(tab, _("Disconnect all sessions before importing settings")); return; }
    }
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Import settings"));
    gtk_file_dialog_open(dialog,
                         GTK_WINDOW(tab->app->window),
                         NULL,
                         on_import_finished,
                         g_object_ref(app->window));
    g_object_unref(dialog);
}

static void on_log_directory_finished(GObject *source,
                                      GAsyncResult *result,
                                      gpointer user_data)
{
    TioTab *tab = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) folder =
        gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
    if (folder == NULL) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            set_status(tab, error->message);
        }
        return;
    }
    g_autofree gchar *path = g_file_get_path(folder);
    if (path == NULL) {
        set_status(tab, _("Choose a local log directory"));
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(tab->log_directory_entry), path);
    set_status(tab, _("Log directory selected"));
}

static void on_choose_log_directory(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Choose log directory"));
    const char *current = gtk_editable_get_text(GTK_EDITABLE(tab->log_directory_entry));
    g_autoptr(GFile) initial = NULL;
    if (current[0] != '\0') {
        initial = g_file_new_for_path(current);
        gtk_file_dialog_set_initial_folder(dialog, initial);
    }
    gtk_file_dialog_select_folder(dialog,
                                  GTK_WINDOW(tab->app->window),
                                  NULL,
                                  on_log_directory_finished,
                                  tab);
    g_object_unref(dialog);
}

static gchar *timestamp_preview(TioTab *tab)
{
    guint selected = gtk_drop_down_get_selected(tab->timestamp_format_dropdown);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    if (selected == 0) {
        g_autofree gchar *base = g_date_time_format(now, "%H:%M:%S");
        return g_strdup_printf("[%s.%03d]", base, g_date_time_get_microsecond(now) / 1000);
    }
    if (selected == 1) {
        gint64 elapsed = g_get_monotonic_time() - tab->app->preview_started_at;
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

/* The previews belong to the session on screen, so the window-level timer
   forwards to whichever that currently is. */
static gboolean tick_settings_previews(gpointer user_data)
{
    TioApp *app = user_data;
    if (app->active == NULL) {
        return G_SOURCE_CONTINUE;
    }
    return update_settings_previews(app->active);
}

static gboolean update_settings_previews(gpointer user_data)
{
    TioTab *tab = user_data;
    g_autofree gchar *timestamp = timestamp_preview(tab);
    g_autofree gchar *timestamp_text = g_strdup_printf(_("Preview: %s"), timestamp);
    gtk_label_set_text(tab->timestamp_preview_label, timestamp_text);

    TioSessionConfig preview;
    tio_session_config_init(&preview);
    g_free(preview.device);
    preview.device = g_strdup(selected_string(tab->device_dropdown));
    g_free(preview.log_directory);
    preview.log_directory = g_strdup("/");
    g_free(preview.log_file);
    preview.log_file = g_strdup(gtk_editable_get_text(GTK_EDITABLE(tab->log_file_entry)));
    g_autofree gchar *path = build_log_path(&preview);
    g_autofree gchar *basename = g_path_get_basename(path);
    g_autofree gchar *filename_text = g_strdup_printf(_("Preview: %s"), basename);
    gtk_label_set_text(tab->log_filename_preview_label, filename_text);
    tio_session_config_clear(&preview);
    return G_SOURCE_CONTINUE;
}

static void on_log_filename_format_changed(GtkDropDown *dropdown,
                                           GParamSpec *pspec,
                                           gpointer user_data)
{
    (void)pspec;
    TioTab *tab = user_data;
    guint selected = gtk_drop_down_get_selected(dropdown);
    gboolean custom = selected == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX;
    gtk_widget_set_visible(GTK_WIDGET(tab->log_file_entry), custom);
    if (!custom && selected < G_N_ELEMENTS(log_filename_templates) - 1) {
        gtk_editable_set_text(GTK_EDITABLE(tab->log_file_entry),
                              log_filename_templates[selected]);
    }
    update_settings_previews(tab);
}

static void on_timestamp_format_changed(GtkDropDown *dropdown,
                                        GParamSpec *pspec,
                                        gpointer user_data)
{
    (void)dropdown;
    (void)pspec;
    update_settings_previews(user_data);
}

/* The controls for one connection. Built per session and parented into that
   session's expander, so each tab configures its own link. */
static void build_session_settings(TioTab *tab)
{
    tab->session_section_label = GTK_LABEL(make_label(_("Session")));
    gtk_widget_add_css_class(GTK_WIDGET(tab->session_section_label), "settings-section-title");

    GtkWidget *session_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(session_card, "settings-card");

    GtkWidget *timestamp_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    tab->timestamp_format_label = GTK_LABEL(make_label(_("Timestamp format")));
    gtk_widget_set_hexpand(GTK_WIDGET(tab->timestamp_format_label), TRUE);
    const char *timestamp_names[] = {
        _("24-hour"), _("Since start"), _("Since previous"), _("ISO 8601"),
        _("Unix epoch"), NULL,
    };
    tab->timestamp_format_dropdown = make_string_dropdown(
        timestamp_names,
        value_index(timestamp_format_values, tab->config.timestamp_format, 3));
    gtk_box_append(GTK_BOX(timestamp_row), GTK_WIDGET(tab->timestamp_format_label));
    gtk_box_append(GTK_BOX(timestamp_row), GTK_WIDGET(tab->timestamp_format_dropdown));
    gtk_box_append(GTK_BOX(session_card), timestamp_row);
    tab->timestamp_format_hint =
        GTK_LABEL(make_label(_("Applied when line timestamps are enabled")));
    gtk_label_set_wrap(tab->timestamp_format_hint, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(tab->timestamp_format_hint), "settings-hint");
    gtk_box_append(GTK_BOX(session_card), GTK_WIDGET(tab->timestamp_format_hint));

    tab->timestamp_preview_label = GTK_LABEL(make_label(""));
    gtk_widget_add_css_class(GTK_WIDGET(tab->timestamp_preview_label), "settings-hint");
    gtk_box_append(GTK_BOX(session_card), GTK_WIDGET(tab->timestamp_preview_label));

    gtk_box_append(GTK_BOX(session_card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *filename_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    tab->log_filename_format_label = GTK_LABEL(make_label(_("Log filename")));
    gtk_widget_set_hexpand(GTK_WIDGET(tab->log_filename_format_label), TRUE);
    const char *filename_names[] = {
        _("tio-gui default"), _("Date first"), _("Device first"), _("Custom…"), NULL,
    };
    guint filename_index = tab->config.log_file[0] == '\0'
                               ? 0
                               : value_index(log_filename_templates,
                                             tab->config.log_file,
                                             TIO_GUI_CUSTOM_LOG_FILENAME_INDEX);
    tab->log_filename_dropdown = make_string_dropdown(filename_names, filename_index);
    gtk_box_append(GTK_BOX(filename_row), GTK_WIDGET(tab->log_filename_format_label));
    gtk_box_append(GTK_BOX(filename_row), GTK_WIDGET(tab->log_filename_dropdown));
    gtk_box_append(GTK_BOX(session_card), filename_row);
    tab->log_file_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(tab->log_file_entry,
                                   _("Example: {device}-{date}-{time}.log"));
    gtk_editable_set_text(GTK_EDITABLE(tab->log_file_entry),
                          filename_index == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX
                              ? tab->config.log_file
                              : log_filename_templates[filename_index]);
    gtk_widget_set_visible(GTK_WIDGET(tab->log_file_entry),
                           filename_index == TIO_GUI_CUSTOM_LOG_FILENAME_INDEX);
    gtk_box_append(GTK_BOX(session_card), GTK_WIDGET(tab->log_file_entry));
    tab->log_filename_preview_label = GTK_LABEL(make_label(""));
    gtk_widget_add_css_class(GTK_WIDGET(tab->log_filename_preview_label), "settings-hint");
    gtk_box_append(GTK_BOX(session_card), GTK_WIDGET(tab->log_filename_preview_label));
    tab->session_settings_card = session_card;

    tab->connection_section_label = GTK_LABEL(make_label(_("Connection and logging")));
    tab->connection_settings_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_widget_add_css_class(GTK_WIDGET(tab->connection_settings_box), "settings-card");

    g_signal_connect(tab->timestamp_format_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_timestamp_format_changed),
                     tab);
    g_signal_connect(tab->log_filename_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_log_filename_format_changed),
                     tab);
    g_signal_connect_swapped(tab->log_file_entry,
                             "changed",
                             G_CALLBACK(update_settings_previews),
                             tab);
}

static GtkWidget *build_settings_popover(TioApp *app)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_size_request(root, 360, -1);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    app->settings_title_label = GTK_LABEL(make_label(_("Settings")));
    gtk_widget_add_css_class(GTK_WIDGET(app->settings_title_label), "settings-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(app->settings_title_label));

    app->appearance_section_label = GTK_LABEL(make_label(_("Appearance")));
    gtk_widget_add_css_class(GTK_WIDGET(app->appearance_section_label),
                             "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(app->appearance_section_label));

    GtkWidget *appearance_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(appearance_card, "settings-card");

    GtkWidget *theme_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->theme_label = GTK_LABEL(make_label(_("Theme")));
    gtk_widget_set_hexpand(GTK_WIDGET(app->theme_label), TRUE);
    const char *theme_names[] = {_("Follow system"), _("Light"), _("Dark"), NULL};
    app->theme_dropdown = make_string_dropdown(theme_names,
                                                value_index(theme_values,
                                                            app->settings.theme,
                                                            0));
    gtk_box_append(GTK_BOX(theme_row), GTK_WIDGET(app->theme_label));
    gtk_box_append(GTK_BOX(theme_row), GTK_WIDGET(app->theme_dropdown));
    gtk_box_append(GTK_BOX(appearance_card), theme_row);

    GtkWidget *language_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->language_label = GTK_LABEL(make_label(_("Language")));
    gtk_widget_set_hexpand(GTK_WIDGET(app->language_label), TRUE);
    const char *language_names[] = {
        _("System default"), "简体中文", "繁體中文", "English", "日本語", "Deutsch", NULL,
    };
    app->language_dropdown = make_string_dropdown(
        language_names, value_index(language_values, app->settings.language, 0));
    gtk_box_append(GTK_BOX(language_row), GTK_WIDGET(app->language_label));
    gtk_box_append(GTK_BOX(language_row), GTK_WIDGET(app->language_dropdown));
    gtk_box_append(GTK_BOX(appearance_card), language_row);
    gtk_box_append(GTK_BOX(root), appearance_card);


    /* What is left in the popover applies to the whole window. */
    app->general_section_label = GTK_LABEL(make_label(_("General")));
    gtk_widget_add_css_class(GTK_WIDGET(app->general_section_label),
                             "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(app->general_section_label));
    app->general_settings_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_widget_add_css_class(GTK_WIDGET(app->general_settings_box), "settings-card");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(app->general_settings_box));

    app->show_all_ttys_check =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Show all TTY devices")));
    gtk_check_button_set_active(app->show_all_ttys_check, app->settings.show_all_ttys);
    GtkWidget *general_row = make_settings_row(GTK_WIDGET(app->general_settings_box));
    gtk_box_append(GTK_BOX(general_row), GTK_WIDGET(app->show_all_ttys_check));

    app->restore_tabs_check =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Reopen sessions on startup")));
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->restore_tabs_check),
                                _("Reopen the tabs that were open last time, without connecting"));
    gtk_check_button_set_active(app->restore_tabs_check, app->settings.restore_tabs);
    GtkWidget *restore_row = make_settings_row(GTK_WIDGET(app->general_settings_box));
    gtk_box_append(GTK_BOX(restore_row), GTK_WIDGET(app->restore_tabs_check));

    app->log_warning_label = GTK_LABEL(make_label(_("Warn above (MB)")));
    app->log_warning_spin =
        make_spin_button(app->settings.log_warning_mb, 65536.0, 16.0);
    GtkWidget *warning_row = make_settings_row(GTK_WIDGET(app->general_settings_box));
    append_labelled(warning_row,
                    app->log_warning_label,
                    GTK_WIDGET(app->log_warning_spin));

    app->backup_section_label = GTK_LABEL(make_label(_("Backup")));
    gtk_widget_add_css_class(GTK_WIDGET(app->backup_section_label), "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(app->backup_section_label));

    app->export_settings_button = GTK_BUTTON(gtk_button_new_with_label(_("Export settings…")));
    app->import_settings_button = GTK_BUTTON(gtk_button_new_with_label(_("Import settings…")));
    GtkWidget *backup_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(backup_card, "settings-card");
    gtk_widget_set_hexpand(GTK_WIDGET(app->export_settings_button), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(app->import_settings_button), TRUE);
    gtk_box_append(GTK_BOX(backup_card), GTK_WIDGET(app->export_settings_button));
    gtk_box_append(GTK_BOX(backup_card), GTK_WIDGET(app->import_settings_button));
    GtkWidget *portable_button = gtk_button_new_with_label(_("Export portable settings…"));
    gtk_widget_set_tooltip_text(portable_button, _("Profiles, buttons and sequences; excludes local paths, device identities and send history"));
    g_object_set_data(G_OBJECT(portable_button), "portable", GINT_TO_POINTER(TRUE));
    g_signal_connect(portable_button, "clicked", G_CALLBACK(on_export_settings_clicked), app);
    gtk_box_append(GTK_BOX(backup_card), portable_button);
    gtk_box_append(GTK_BOX(root), backup_card);

    app->about_section_label = GTK_LABEL(make_label(_("About")));
    gtk_widget_add_css_class(GTK_WIDGET(app->about_section_label), "settings-section-title");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(app->about_section_label));

    GtkWidget *about_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(about_card, "settings-card");
    GtkWidget *about_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    g_autofree gchar *version = g_strdup_printf(_("Version %s"), TIO_GUI_VERSION);
    app->version_label = GTK_LABEL(gtk_label_new(version));
    gtk_widget_add_css_class(GTK_WIDGET(app->version_label), "settings-version");
    gtk_label_set_xalign(app->version_label, 0.0F);
    gtk_widget_set_hexpand(GTK_WIDGET(app->version_label), TRUE);
    app->github_button = GTK_BUTTON(gtk_button_new_with_label(_("GitHub project")));
    gtk_widget_add_css_class(GTK_WIDGET(app->github_button), "flat");
    gtk_box_append(GTK_BOX(about_header), GTK_WIDGET(app->version_label));
    gtk_box_append(GTK_BOX(about_header), GTK_WIDGET(app->github_button));
    GtkWidget *about_button = gtk_button_new_with_label(_("About / diagnostics…"));
    g_signal_connect(about_button, "clicked", G_CALLBACK(on_about_clicked), app);
    gtk_box_append(GTK_BOX(about_header), about_button);
    gtk_box_append(GTK_BOX(about_card), about_header);
    app->about_description_label =
        GTK_LABEL(make_label(_("A lightweight, reliable GUI for tio")));
    gtk_widget_add_css_class(GTK_WIDGET(app->about_description_label), "settings-hint");
    gtk_box_append(GTK_BOX(about_card), GTK_WIDGET(app->about_description_label));
    GtkWidget *update_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->update_available_label = GTK_LABEL(make_label(""));
    gtk_widget_add_css_class(GTK_WIDGET(app->update_available_label), "settings-hint");
    gtk_widget_set_hexpand(GTK_WIDGET(app->update_available_label), TRUE);
    app->download_update_button =
        GTK_BUTTON(gtk_button_new_with_label(_("Download update")));
    gtk_widget_add_css_class(GTK_WIDGET(app->download_update_button), "suggested-action");
    gtk_widget_set_visible(GTK_WIDGET(app->update_available_label), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(app->download_update_button), FALSE);
    gtk_box_append(GTK_BOX(update_row), GTK_WIDGET(app->update_available_label));
    gtk_box_append(GTK_BOX(update_row), GTK_WIDGET(app->download_update_button));
    gtk_box_append(GTK_BOX(about_card), update_row);
    gtk_box_append(GTK_BOX(root), about_card);

    g_signal_connect(app->theme_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_theme_changed),
                     app);
    g_signal_connect(app->language_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_language_changed),
                     app);
    g_signal_connect(app->github_button, "clicked", G_CALLBACK(on_github_clicked), app);
    g_signal_connect(app->download_update_button,
                     "clicked",
                     G_CALLBACK(on_download_update_clicked),
                     app);
    g_signal_connect(app->export_settings_button,
                     "clicked",
                     G_CALLBACK(on_export_settings_clicked),
                     app);
    g_signal_connect(app->import_settings_button,
                     "clicked",
                     G_CALLBACK(on_import_settings_clicked),
                     app);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 560);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), root);
    return scroll;
}

static GtkWidget *build_profile_popover(TioTab *tab)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(root, 240, -1);

    tab->profile_empty_label = GTK_LABEL(make_label(_("No saved profiles")));
    gtk_widget_add_css_class(GTK_WIDGET(tab->profile_empty_label), "dim-label");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(tab->profile_empty_label));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    tab->profile_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(tab->profile_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(tab->profile_list));
    gtk_box_append(GTK_BOX(root), scroll);

    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    tab->profile_save_button = GTK_BUTTON(gtk_button_new_with_label(_("Save as new profile…")));
    tab->profile_update_button = GTK_BUTTON(gtk_button_new_with_label(_("Update this profile")));
    tab->profile_duplicate_button = GTK_BUTTON(gtk_button_new_with_label(_("Duplicate…")));
    tab->profile_delete_button = GTK_BUTTON(gtk_button_new_with_label(_("Delete profile")));
    gtk_widget_add_css_class(GTK_WIDGET(tab->profile_delete_button), "destructive-action");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(tab->profile_save_button));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(tab->profile_update_button));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(tab->profile_duplicate_button));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(tab->profile_delete_button));

    g_signal_connect(tab->profile_list, "row-activated", G_CALLBACK(on_profile_row_activated), tab);
    g_signal_connect(tab->profile_save_button, "clicked", G_CALLBACK(on_profile_save_clicked), tab);
    g_signal_connect(tab->profile_update_button,
                     "clicked",
                     G_CALLBACK(on_profile_update_clicked),
                     tab);
    g_signal_connect(tab->profile_duplicate_button,
                     "clicked",
                     G_CALLBACK(on_profile_duplicate_clicked),
                     tab);
    g_signal_connect(tab->profile_delete_button,
                     "clicked",
                     G_CALLBACK(on_profile_delete_clicked),
                     tab);
    return root;
}

static GtkWidget *build_history_popover(TioTab *tab)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(root, 280, -1);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 8);
    gtk_widget_set_margin_start(root, 8);
    gtk_widget_set_margin_end(root, 8);

    tab->history_empty_label = GTK_LABEL(make_label(_("No history yet")));
    gtk_widget_add_css_class(GTK_WIDGET(tab->history_empty_label), "dim-label");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(tab->history_empty_label));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 260);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    tab->history_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(tab->history_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(tab->history_list));
    gtk_box_append(GTK_BOX(root), scroll);

    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    tab->history_clear_button = GTK_BUTTON(gtk_button_new_with_label(_("Clear history")));
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(tab->history_clear_button));

    g_signal_connect(tab->history_list, "row-activated", G_CALLBACK(on_history_row_activated), tab);
    g_signal_connect(tab->history_clear_button,
                     "clicked",
                     G_CALLBACK(on_history_clear_clicked),
                     tab);
    return root;
}

static GtkWidget *build_search_bar(TioTab *tab)
{
    tab->search_bar = GTK_SEARCH_BAR(gtk_search_bar_new());
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(box, "compact-controls");

    tab->search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(tab->search_entry, _("Search terminal…"));
    gtk_widget_set_hexpand(GTK_WIDGET(tab->search_entry), TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->search_entry));

    tab->search_previous_button =
        GTK_BUTTON(gtk_button_new_from_icon_name("go-up-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->search_previous_button), _("Find previous"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->search_previous_button));

    tab->search_next_button = GTK_BUTTON(gtk_button_new_from_icon_name("go-down-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->search_next_button), _("Find next"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->search_next_button));

    tab->search_case_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Aa"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->search_case_toggle), _("Match case"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->search_case_toggle));

    tab->search_regex_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(".*"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->search_regex_toggle), _("Regular expression"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->search_regex_toggle));

    gtk_search_bar_set_child(tab->search_bar, box);
    gtk_search_bar_connect_entry(tab->search_bar, GTK_EDITABLE(tab->search_entry));
    gtk_search_bar_set_show_close_button(tab->search_bar, TRUE);

    g_signal_connect(tab->search_entry, "search-changed", G_CALLBACK(on_search_changed), tab);
    g_signal_connect(tab->search_entry, "activate", G_CALLBACK(on_search_activate), tab);
    g_signal_connect(tab->search_entry, "stop-search", G_CALLBACK(on_search_stopped), tab);
    g_signal_connect(tab->search_next_button, "clicked", G_CALLBACK(on_search_next), tab);
    g_signal_connect(tab->search_previous_button,
                     "clicked",
                     G_CALLBACK(on_search_previous),
                     tab);
    g_signal_connect(tab->search_case_toggle,
                     "toggled",
                     G_CALLBACK(on_search_option_toggled),
                     tab);
    g_signal_connect(tab->search_regex_toggle,
                     "toggled",
                     G_CALLBACK(on_search_option_toggled),
                     tab);
    return GTK_WIDGET(tab->search_bar);
}

/* Builds one session: its own toolbar, terminal, send bar and settings, in a
   widget the notebook can page. Everything window-wide is reached through
   tab->app, so nothing here assumes it is the only session. */
static TioTab *tio_tab_new(TioApp *app)
{
    TioTab *tab = g_new0(TioTab, 1);
    tab->app = app;
    static guint64 next_tab_id = 0;
    tab->id = ++next_tab_id;
    tab->log_model = tio_log_model_new();
    tab->child_pid = -1;
    tab->follow_output = TRUE;
    tio_session_config_init(&tab->config);
    tab->highlight_follow = TRUE;
    tio_session_config_copy(&tab->config, &app->settings.defaults);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root, 8);
    gtk_widget_set_margin_bottom(root, 8);
    gtk_widget_set_margin_start(root, 8);
    gtk_widget_set_margin_end(root, 8);
    tab->content = root;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(toolbar, "compact-controls");
    gtk_box_append(GTK_BOX(root), toolbar);

    /* The session's own controls, parented into its expander further down. */
    build_session_settings(tab);

    tab->profile_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_label(tab->profile_button, _("Profiles"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->profile_button), _("Connection profiles"));
    tab->profile_popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_position(tab->profile_popover, GTK_POS_BOTTOM);
    gtk_widget_set_halign(GTK_WIDGET(tab->profile_popover), GTK_ALIGN_START);
    gtk_popover_set_offset(tab->profile_popover, 8, 0);
    gtk_popover_set_child(tab->profile_popover, build_profile_popover(tab));
    gtk_menu_button_set_popover(tab->profile_button, GTK_WIDGET(tab->profile_popover));
    gtk_widget_set_hexpand(GTK_WIDGET(tab->profile_button), TRUE);
    gtk_box_append(tab->connection_settings_box, GTK_WIDGET(tab->profile_button));

    tab->device_label = GTK_LABEL(make_label(_("Device")));
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(tab->device_label));
    tab->device_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    GtkListItemFactory *device_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(device_factory, "setup", G_CALLBACK(on_device_item_setup), tab);
    g_signal_connect(device_factory, "bind", G_CALLBACK(on_device_item_bind), tab);
    gtk_drop_down_set_list_factory(tab->device_dropdown, device_factory);
    g_object_unref(device_factory);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->device_dropdown), TRUE);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(tab->device_dropdown));

    tab->refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(tab->refresh_button, _("Refresh serial devices"));
    gtk_box_append(GTK_BOX(toolbar), tab->refresh_button);

    tab->baud_label = GTK_LABEL(make_label(_("Baud")));
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(tab->baud_label));
    GtkStringList *baud_model = gtk_string_list_new(baud_rates);
    gtk_string_list_append(baud_model, _("Custom…"));
    tab->baud_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(baud_model), NULL));
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(tab->baud_dropdown));
    tab->custom_baud_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(tab->custom_baud_entry, _("Custom baud"));
    gtk_entry_set_input_purpose(tab->custom_baud_entry, GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_max_length(tab->custom_baud_entry, 10);
    gtk_editable_set_width_chars(GTK_EDITABLE(tab->custom_baud_entry), 10);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(tab->custom_baud_entry));

    tab->connect_button = GTK_BUTTON(gtk_button_new_with_label(_("Connect")));
    gtk_widget_add_css_class(GTK_WIDGET(tab->connect_button), "suggested-action");
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(tab->connect_button));

    /* Keep only frequently used switches on the main workspace. */
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(options, "compact-controls");

    tab->timestamp_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Timestamps")));
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(tab->timestamp_check));

    tab->log_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Log session")));
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(tab->log_check));
    tab->highlight_toggle =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Highlight view")));
    gtk_check_button_set_active(tab->highlight_toggle, TRUE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->highlight_toggle),
                                _("Show a semantic view of the raw serial log"));
    gtk_box_append(GTK_BOX(options), GTK_WIDGET(tab->highlight_toggle));
    gtk_box_append(GTK_BOX(root), options);

    tab->log_directory_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(tab->log_directory_entry, _("Log directory"));
    gtk_widget_set_hexpand(GTK_WIDGET(tab->log_directory_entry), TRUE);
    tab->choose_log_directory_button =
        GTK_BUTTON(gtk_button_new_from_icon_name("folder-open-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->choose_log_directory_button),
                                _("Choose log directory"));
    GtkWidget *advanced_box = GTK_WIDGET(tab->connection_settings_box);
    gtk_widget_add_css_class(advanced_box, "compact-controls");

    tab->data_bits_dropdown = make_string_dropdown(data_bits_values, 3);
    tab->stop_bits_dropdown = make_string_dropdown(stop_bits_values, 0);
    tab->parity_dropdown = make_string_dropdown(parity_values, 0);
    tab->flow_dropdown = make_string_dropdown(flow_values, 0);
    tab->local_echo_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Local echo")));
    tab->output_delay_spin = make_spin_button(0, 10000.0, 1.0);
    tab->output_line_delay_spin = make_spin_button(0, 10000.0, 1.0);
    tab->log_append_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Append")));
    tab->log_strip_check =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Strip control characters")));
    tab->open_log_directory_button = GTK_BUTTON(gtk_button_new_with_label(_("Open folder")));

    tab->data_bits_label = GTK_LABEL(make_label(_("Data bits")));
    tab->stop_bits_label = GTK_LABEL(make_label(_("Stop bits")));
    tab->parity_label = GTK_LABEL(make_label(_("Parity")));
    tab->flow_label = GTK_LABEL(make_label(_("Flow control")));
    tab->output_delay_label = GTK_LABEL(make_label(_("Character delay (ms)")));
    tab->output_line_delay_label = GTK_LABEL(make_label(_("Line delay (ms)")));

    GtkWidget *framing_row = make_settings_row(advanced_box);
    append_labelled(framing_row, tab->data_bits_label, GTK_WIDGET(tab->data_bits_dropdown));
    append_labelled(framing_row, tab->stop_bits_label, GTK_WIDGET(tab->stop_bits_dropdown));

    GtkWidget *protocol_row = make_settings_row(advanced_box);
    append_labelled(protocol_row, tab->parity_label, GTK_WIDGET(tab->parity_dropdown));
    append_labelled(protocol_row, tab->flow_label, GTK_WIDGET(tab->flow_dropdown));

    GtkWidget *behaviour_row = make_settings_row(advanced_box);
    gtk_box_append(GTK_BOX(behaviour_row), GTK_WIDGET(tab->local_echo_check));

    GtkWidget *delay_row = make_settings_row(advanced_box);
    append_labelled(delay_row, tab->output_delay_label, GTK_WIDGET(tab->output_delay_spin));
    append_labelled(delay_row,
                    tab->output_line_delay_label,
                    GTK_WIDGET(tab->output_line_delay_spin));

    GtkWidget *log_directory_row = make_settings_row(advanced_box);
    gtk_box_append(GTK_BOX(log_directory_row), GTK_WIDGET(tab->log_directory_entry));
    gtk_box_append(GTK_BOX(log_directory_row), GTK_WIDGET(tab->choose_log_directory_button));

    GtkWidget *log_option_row = make_settings_row(advanced_box);
    gtk_box_append(GTK_BOX(log_option_row), GTK_WIDGET(tab->log_append_check));
    gtk_box_append(GTK_BOX(log_option_row), GTK_WIDGET(tab->log_strip_check));

    GtkWidget *log_limit_row = make_settings_row(advanced_box);
    gtk_box_append(GTK_BOX(log_limit_row), GTK_WIDGET(tab->open_log_directory_button));

    /* One collapsed place for everything that belongs to this session only. */
    tab->session_settings_expander = gtk_expander_new(NULL);
    gtk_expander_set_label_widget(GTK_EXPANDER(tab->session_settings_expander),
                                  GTK_WIDGET(tab->connection_section_label));
    GtkWidget *session_settings = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(session_settings, 6);
    gtk_box_append(GTK_BOX(session_settings), GTK_WIDGET(tab->session_section_label));
    gtk_box_append(GTK_BOX(session_settings), tab->session_settings_card);
    gtk_box_append(GTK_BOX(session_settings), GTK_WIDGET(tab->connection_settings_box));
    GtkWidget *line_expander = gtk_expander_new(_("Serial lines / RS-485"));
    gtk_expander_set_child(GTK_EXPANDER(line_expander), line_controls_new(tab));
    gtk_box_append(GTK_BOX(session_settings), line_expander);
    GtkWidget *reconnect_expander = gtk_expander_new(_("Reconnect strategy"));
    gtk_expander_set_child(GTK_EXPANDER(reconnect_expander), reconnect_controls_new(tab));
    gtk_box_append(GTK_BOX(session_settings), reconnect_expander);
    GtkWidget *transfer_expander = gtk_expander_new(_("File transfer"));
    gtk_expander_set_child(GTK_EXPANDER(transfer_expander), transfer_controls_new(tab));
    gtk_box_append(GTK_BOX(session_settings), transfer_expander);
    GtkWidget *capture_expander = gtk_expander_new(_("Recording / safe rotation"));
    gtk_expander_set_child(GTK_EXPANDER(capture_expander), capture_controls_new(tab));
    gtk_box_append(GTK_BOX(session_settings), capture_expander);
    GtkWidget *settings_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(settings_scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(settings_scroll), 300);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(settings_scroll), session_settings);
    gtk_expander_set_child(GTK_EXPANDER(tab->session_settings_expander), settings_scroll);
    gtk_expander_set_expanded(GTK_EXPANDER(tab->session_settings_expander),
                              app->settings.advanced_expanded);
    gtk_box_append(GTK_BOX(root), tab->session_settings_expander);

    /* Status bar. */
    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(status_bar, "compact-controls");
    tab->status_label = GTK_LABEL(gtk_label_new(_("Ready")));
    gtk_label_set_xalign(tab->status_label, 0.0F);
    gtk_label_set_ellipsize(tab->status_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->status_label), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(tab->status_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(tab->status_label), "session-status");
    gtk_box_append(GTK_BOX(status_bar), GTK_WIDGET(tab->status_label));

    tab->session_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(tab->session_label, 1.0F);
    gtk_widget_add_css_class(GTK_WIDGET(tab->session_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(tab->session_label), "session-status");
    gtk_box_append(GTK_BOX(status_bar), GTK_WIDGET(tab->session_label));

    tab->clear_terminal_button = GTK_BUTTON(gtk_button_new_with_label(_("Clear")));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->clear_terminal_button),
                                _("Clear the terminal and its scrollback history"));
    gtk_box_append(GTK_BOX(status_bar), GTK_WIDGET(tab->clear_terminal_button));
    gtk_box_append(GTK_BOX(root), status_bar);

    gtk_box_append(GTK_BOX(root), build_search_bar(tab));

    /* Terminal. */
    tab->terminal = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scrollback_lines(tab->terminal, 10000);
    vte_terminal_set_mouse_autohide(tab->terminal, TRUE);
    vte_terminal_set_scroll_on_output(tab->terminal, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->terminal), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(tab->terminal), TRUE);

    GtkWidget *terminal_frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(terminal_frame, "terminal-frame");
    GtkWidget *terminal_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(terminal_scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(terminal_scroll),
                                  GTK_WIDGET(tab->terminal));

    GtkWidget *terminal_overlay = gtk_overlay_new();
    tab->scroll_bottom_button =
        GTK_BUTTON(gtk_button_new_with_label(_("Back to bottom ↓")));
    gtk_widget_add_css_class(GTK_WIDGET(tab->scroll_bottom_button), "scroll-bottom");
    gtk_widget_add_css_class(GTK_WIDGET(tab->scroll_bottom_button), "suggested-action");
    gtk_widget_set_halign(GTK_WIDGET(tab->scroll_bottom_button), GTK_ALIGN_END);
    gtk_widget_set_valign(GTK_WIDGET(tab->scroll_bottom_button), GTK_ALIGN_END);
    gtk_widget_set_visible(GTK_WIDGET(tab->scroll_bottom_button), FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(terminal_overlay),
                            GTK_WIDGET(tab->scroll_bottom_button));

    tab->highlight_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_widget_add_css_class(GTK_WIDGET(tab->highlight_view), "highlight-view");
    gtk_text_view_set_editable(tab->highlight_view, FALSE);
    gtk_text_view_set_cursor_visible(tab->highlight_view, FALSE);
    gtk_text_view_set_monospace(tab->highlight_view, TRUE);
    gtk_text_view_set_wrap_mode(tab->highlight_view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(tab->highlight_view, 8);
    gtk_text_view_set_right_margin(tab->highlight_view, 8);
    gtk_text_view_set_top_margin(tab->highlight_view, 6);
    gtk_text_view_set_bottom_margin(tab->highlight_view, 6);
    tab->highlighter =
        tio_highlighter_new(gtk_text_view_get_buffer(tab->highlight_view));
    GtkWidget *highlight_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(highlight_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(highlight_scroll),
                                  GTK_WIDGET(tab->highlight_view));
    GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
    g_signal_connect(wheel, "scroll", G_CALLBACK(on_highlight_wheel), tab);
    gtk_widget_add_controller(highlight_scroll, wheel);
    GtkGesture *pointer = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pointer), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(pointer), GTK_PHASE_CAPTURE);
    g_signal_connect(pointer, "pressed", G_CALLBACK(on_highlight_pointer_pressed), tab);
    g_signal_connect(pointer, "released", G_CALLBACK(on_highlight_pointer_released), tab);
    g_signal_connect(pointer, "stopped", G_CALLBACK(on_highlight_pointer_stopped), tab);
    gtk_widget_add_controller(highlight_scroll, GTK_EVENT_CONTROLLER(pointer));

    tab->terminal_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_add_named(tab->terminal_stack, terminal_scroll, "terminal");
    gtk_stack_add_named(tab->terminal_stack, highlight_scroll, "highlight");
    gtk_stack_set_visible_child_name(tab->terminal_stack, "highlight");
    gtk_overlay_set_child(GTK_OVERLAY(terminal_overlay), GTK_WIDGET(tab->terminal_stack));
    gtk_frame_set_child(GTK_FRAME(terminal_frame), terminal_overlay);
    gtk_widget_set_hexpand(terminal_frame, TRUE);
    gtk_widget_set_vexpand(terminal_frame, TRUE);
    gtk_box_append(GTK_BOX(root), terminal_frame);

    /* Send bar. */
    GtkWidget *send_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(send_bar, "compact-controls");

    tab->history_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(tab->history_button, "document-open-recent-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->history_button), _("Send history"));
    tab->history_popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_position(tab->history_popover, GTK_POS_TOP);
    gtk_widget_set_halign(GTK_WIDGET(tab->history_popover), GTK_ALIGN_START);
    gtk_popover_set_offset(tab->history_popover, 8, 0);
    gtk_popover_set_child(tab->history_popover, build_history_popover(tab));
    gtk_menu_button_set_popover(tab->history_button, GTK_WIDGET(tab->history_popover));
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(tab->history_button));

    tab->send_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(tab->send_entry, _("Send text…"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->send_entry),
                                _("Press Up and Down to browse the send history"));
    gtk_widget_set_hexpand(GTK_WIDGET(tab->send_entry), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_entry), FALSE);
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(tab->send_entry));

    const char *line_ending_names[] = {
        _("No line ending"), "LF (\\n)", "CR (\\r)", "CR+LF (\\r\\n)", NULL,
    };
    tab->line_ending_dropdown = make_string_dropdown(line_ending_names, 2);
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->line_ending_dropdown), _("Line ending"));
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(tab->line_ending_dropdown));

    tab->send_button = GTK_BUTTON(gtk_button_new_with_label(_("Send")));
    gtk_widget_set_sensitive(GTK_WIDGET(tab->send_button), FALSE);
    gtk_box_append(GTK_BOX(send_bar), GTK_WIDGET(tab->send_button));
    gtk_box_append(GTK_BOX(root), send_bar);

    /* Quick buttons. */
    GtkWidget *quick_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(quick_bar, "compact-controls");
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        tab->quick_buttons[index] = GTK_BUTTON(gtk_button_new_with_label(""));
        g_object_set_data(G_OBJECT(tab->quick_buttons[index]),
                          "quick-button-index",
                          GUINT_TO_POINTER(index + 1));
        gtk_widget_set_sensitive(GTK_WIDGET(tab->quick_buttons[index]), FALSE);
        gtk_box_append(GTK_BOX(quick_bar), GTK_WIDGET(tab->quick_buttons[index]));
        g_signal_connect(tab->quick_buttons[index],
                         "clicked",
                         G_CALLBACK(on_quick_button_clicked),
                         tab);
    }
    tab->customize_quick_buttons =
        GTK_BUTTON(gtk_button_new_with_label(_("Customize…")));
    gtk_widget_set_hexpand(GTK_WIDGET(tab->customize_quick_buttons), TRUE);
    gtk_widget_set_halign(GTK_WIDGET(tab->customize_quick_buttons), GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(quick_bar), GTK_WIDGET(tab->customize_quick_buttons));
    GtkWidget *sequences_button = gtk_button_new_with_label(_("Sequences…"));
    gtk_box_append(GTK_BOX(quick_bar), sequences_button);
    g_signal_connect(sequences_button, "clicked", G_CALLBACK(on_sequences_clicked), tab);
    GtkWidget *analyzer_button = gtk_button_new_with_label(_("Analyze…"));
    gtk_box_append(GTK_BOX(quick_bar), analyzer_button);
    g_signal_connect(analyzer_button, "clicked", G_CALLBACK(on_analyzer_clicked), tab);
    tab->hex_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("HEX"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->hex_toggle),
                                _("Display incoming bytes as 16-byte hex rows"));
    gtk_box_append(GTK_BOX(quick_bar), GTK_WIDGET(tab->hex_toggle));
    gtk_box_append(GTK_BOX(root), quick_bar);

    /* Signals. */
    g_signal_connect(tab->refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), tab);
    g_signal_connect(tab->baud_dropdown, "notify::selected", G_CALLBACK(on_baud_changed), tab);
    g_signal_connect_swapped(tab->custom_baud_entry,
                             "changed",
                             G_CALLBACK(update_session_label),
                             tab);
    g_signal_connect(tab->device_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     tab);
    g_signal_connect(tab->data_bits_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     tab);
    g_signal_connect(tab->stop_bits_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     tab);
    g_signal_connect(tab->parity_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     tab);
    g_signal_connect(tab->flow_dropdown,
                     "notify::selected",
                     G_CALLBACK(on_serial_setting_changed),
                     tab);
    g_signal_connect(tab->clear_terminal_button,
                     "clicked",
                     G_CALLBACK(on_clear_terminal_clicked),
                     tab);
    g_signal_connect(tab->connect_button, "clicked", G_CALLBACK(on_connect_clicked), tab);
    g_signal_connect(tab->log_check, "toggled", G_CALLBACK(on_log_toggled), tab);
    g_signal_connect(tab->highlight_toggle,
                     "toggled",
                     G_CALLBACK(on_highlight_toggled),
                     tab);
    g_signal_connect(tab->open_log_directory_button,
                     "clicked",
                     G_CALLBACK(on_open_log_directory),
                     tab);
    g_signal_connect(tab->choose_log_directory_button,
                     "clicked",
                     G_CALLBACK(on_choose_log_directory),
                     tab);
    g_signal_connect(app->show_all_ttys_check,
                     "toggled",
                     G_CALLBACK(on_show_all_ttys_toggled),
                     tab);
    g_signal_connect(tab->terminal, "child-exited", G_CALLBACK(on_child_exited), tab);
    g_signal_connect(tab->terminal, "commit", G_CALLBACK(on_terminal_commit), tab);
    g_signal_connect(tab->terminal, "selection-changed", G_CALLBACK(on_terminal_selection_changed), tab);
    g_signal_connect(gtk_text_view_get_buffer(tab->highlight_view), "notify::has-selection",
                     G_CALLBACK(on_highlight_selection_changed), tab);
    GtkAdjustment *highlight_adjustment =
        gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->highlight_view));
    g_signal_connect(highlight_adjustment, "value-changed", G_CALLBACK(on_highlight_scrolled), tab);
    g_signal_connect(highlight_adjustment, "changed", G_CALLBACK(on_highlight_content_changed), tab);
    g_signal_connect(tab->send_button, "clicked", G_CALLBACK(on_send_clicked), tab);
    g_signal_connect(tab->send_entry, "activate", G_CALLBACK(on_send_activate), tab);
    g_signal_connect(tab->send_entry, "changed", G_CALLBACK(on_send_entry_changed), tab);
    g_signal_connect(tab->customize_quick_buttons,
                     "clicked",
                     G_CALLBACK(on_customize_quick_buttons),
                     tab);
    g_signal_connect(tab->scroll_bottom_button,
                     "clicked",
                     G_CALLBACK(on_scroll_bottom_clicked),
                     tab);
    GtkEventControllerKey *send_keys = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
    GtkEventController *highlight_keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(highlight_keys, GTK_PHASE_CAPTURE);
    g_signal_connect(highlight_keys, "key-pressed", G_CALLBACK(on_highlight_key_pressed), tab);
    g_signal_connect(highlight_keys, "key-released", G_CALLBACK(on_highlight_key_released), tab);
    gtk_widget_add_controller(GTK_WIDGET(tab->highlight_view), highlight_keys);
    GtkWidget *return_targets[] = {GTK_WIDGET(tab->terminal), GTK_WIDGET(tab->send_entry)};
    for (guint i = 0; i < G_N_ELEMENTS(return_targets); ++i) {
        GtkEventController *keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_log_return_pressed), tab);
        gtk_widget_add_controller(return_targets[i], keys);
    }
    g_signal_connect(send_keys, "key-pressed", G_CALLBACK(on_send_entry_key), tab);
    gtk_widget_add_controller(GTK_WIDGET(tab->send_entry),
                              GTK_EVENT_CONTROLLER(send_keys));

    GtkAdjustment *adjustment = terminal_adjustment(tab);
    if (adjustment != NULL) {
        g_signal_connect(adjustment, "value-changed", G_CALLBACK(on_terminal_scrolled), tab);
        g_signal_connect(adjustment, "changed", G_CALLBACK(on_terminal_content_changed), tab);
    }

    apply_session_config(tab, &tab->config);
    update_quick_buttons(tab);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->log_directory_entry),
                             tab->config.logging);
    gtk_widget_set_sensitive(GTK_WIDGET(tab->choose_log_directory_button),
                             tab->config.logging);
    refresh_profile_ui(tab);
    refresh_history_ui(tab);
    refresh_devices(tab);

    return tab;
}

/* ------------------------------------------------------ tab management */

static void tio_app_close_tab(TioApp *app, TioTab *tab);

static void on_tab_close_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    TioTab *tab = user_data;
    tio_app_close_tab(tab->app, tab);
}

/* Name the tab after what it is talking to, so several sessions stay apart at
   a glance. Falls back to a number before a device has been picked. */
static void update_tab_label(TioTab *tab)
{
    if (tab->tab_label == NULL) {
        return;
    }
    const char *device = tab->observed_device ? tab->observed_device : selected_string(tab->device_dropdown);
    const char *baud = selected_baud(tab);
    g_autofree gchar *text = NULL;
    if (device != NULL && device[0] != '\0') {
        g_autofree gchar *base = g_path_get_basename(device);
        text = baud != NULL && baud[0] != '\0' ? g_strdup_printf("%s · %s", base, baud)
                                               : g_strdup(base);
    } else {
        guint position = 1;
        for (guint index = 0; index < tab->app->tabs->len; ++index) {
            if (g_ptr_array_index(tab->app->tabs, index) == tab) {
                position = index + 1;
                break;
            }
        }
        text = g_strdup_printf(_("Session %u"), position);
    }
    gtk_label_set_text(tab->tab_label, text);
    gtk_widget_set_tooltip_text(GTK_WIDGET(tab->tab_label), text);
}

static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page,
                                    guint number, gpointer user_data)
{
    (void)notebook; (void)number;
    TioApp *app = user_data;
    for (guint i = 0; i < app->tabs->len; ++i) {
        TioTab *tab = g_ptr_array_index(app->tabs, i);
        if (tab->content == page) { app->active = tab; update_settings_previews(tab); return; }
    }
}

static void on_notebook_reordered(GtkNotebook *notebook, GtkWidget *page, guint number, gpointer data)
{
    (void)page; (void)number;
    TioApp *app = data;
    GPtrArray *ordered = g_ptr_array_new();
    for (gint i = 0; i < gtk_notebook_get_n_pages(notebook); ++i) {
        GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
        for (guint j = 0; j < app->tabs->len; ++j) {
            TioTab *tab = g_ptr_array_index(app->tabs, j);
            if (tab->content == child) { g_ptr_array_add(ordered, tab); break; }
        }
    }
    g_ptr_array_unref(app->tabs); app->tabs = ordered;
    gint current = gtk_notebook_get_current_page(notebook);
    if (current >= 0 && (guint)current < app->tabs->len) app->active = g_ptr_array_index(app->tabs, (guint)current);
}

static TioTab *tio_app_add_tab(TioApp *app)
{
    TioTab *tab = tio_tab_new(app);
    g_ptr_array_add(app->tabs, tab);

    GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tab->tab_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(tab->tab_label, PANGO_ELLIPSIZE_MIDDLE);
    /* An ellipsizing label will otherwise shrink to the ellipsis itself. */
    gtk_label_set_width_chars(tab->tab_label, 16);
    gtk_label_set_max_width_chars(tab->tab_label, 22);
    gtk_box_append(GTK_BOX(label_box), GTK_WIDGET(tab->tab_label));

    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    gtk_widget_set_tooltip_text(close, _("Close this session"));
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close_clicked), tab);
    gtk_box_append(GTK_BOX(label_box), close);

    gint page = gtk_notebook_append_page(app->notebook, tab->content, label_box);
    gtk_notebook_set_tab_reorderable(app->notebook, tab->content, TRUE);
    update_tab_label(tab);
    gtk_notebook_set_current_page(app->notebook, page);
    app->active = tab;
    return tab;
}

static void tio_app_finish_close_tab(TioApp *app, TioTab *tab);
static gboolean close_tab_after_io(gpointer data)
{
    TioTab *tab = data;
    if (tab->spawn_pending || !tio_capture_finished(tab->capture)) return G_SOURCE_CONTINUE;
    tab->deferred_close_timer = 0;
    tio_app_finish_close_tab(tab->app, tab);
    return G_SOURCE_REMOVE;
}

static void tio_app_finish_close_tab(TioApp *app, TioTab *tab)
{
    tab->close_requested = TRUE;
    raw_tap_stop(tab);
    if (tab->spawn_pending || !tio_capture_finished(tab->capture)) {
        gtk_widget_set_sensitive(tab->content, FALSE);
        if (!tab->deferred_close_timer) tab->deferred_close_timer = g_timeout_add(50, close_tab_after_io, tab);
        return;
    }
    g_signal_handlers_disconnect_by_data(tab->terminal, tab);
    g_signal_handlers_disconnect_by_data(app->show_all_ttys_check, tab);
    gint page = gtk_notebook_page_num(app->notebook, tab->content);
    g_ptr_array_remove(app->tabs, tab);
    if (page >= 0) {
        gtk_notebook_remove_page(app->notebook, page);
    }
    if (app->active == tab) {
        app->active = NULL;
    }
    tio_tab_free(tab);

    gint current = gtk_notebook_get_current_page(app->notebook);
    if (current >= 0 && (guint)current < app->tabs->len) {
        app->active = g_ptr_array_index(app->tabs, (guint)current);
    }
    /* The last session closing takes the window with it. */
    if (app->tabs->len == 0) {
        gtk_window_close(GTK_WINDOW(app->window));
    } else {
        for (guint index = 0; index < app->tabs->len; ++index) {
            update_tab_label(g_ptr_array_index(app->tabs, index));
        }
    }
}

static void on_close_tab_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    TioTab *tab = user_data;
    g_autoptr(GError) error = NULL;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    if (choice != 1) {
        return;
    }
    if (tab->child_pid > 0) {
        (void)kill(tab->child_pid, SIGHUP);
    }
    tio_app_finish_close_tab(tab->app, tab);
}

/* A session that is still connected or still writing a log is not closed on a
   stray click. */
static void tio_app_close_tab(TioApp *app, TioTab *tab)
{
    if (tab->child_pid <= 0 && tab->log_path == NULL) {
        tio_app_finish_close_tab(app, tab);
        return;
    }

    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", _("Close this session?"));
    const char *device = selected_string(tab->device_dropdown);
    g_autofree gchar *detail =
        tab->child_pid > 0
            ? g_strdup_printf(_("tio is still connected to %s."),
                              device != NULL ? device : _("the serial device"))
            : g_strdup(_("The session log is still open."));
    gtk_alert_dialog_set_detail(dialog, detail);
    const char *buttons[] = {_("Cancel"), _("Close"), NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog,
                            GTK_WINDOW(app->window),
                            NULL,
                            on_close_tab_response,
                            tab);
    g_object_unref(dialog);
}

static void action_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    (void)tio_app_add_tab(user_data);
}

static void action_close_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    if (app->active != NULL) {
        tio_app_close_tab(app, app->active);
    }
}

static void action_next_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    gtk_notebook_next_page(app->notebook);
}

static void action_previous_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    TioApp *app = user_data;
    gtk_notebook_prev_page(app->notebook);
}

static void activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;

    /* A second launch must reuse the running session instead of opening a
       window that cannot own the same serial device. */
    GtkWidget *existing = g_object_get_data(G_OBJECT(application), "tio-gui-window");
    if (existing != NULL) {
        gtk_window_present(GTK_WINDOW(existing));
        TioApp *running = g_object_get_data(G_OBJECT(existing), "tio-gui");
        if (running != NULL) {
            (void)tio_app_add_tab(running);
        }
        return;
    }

    TioApp *app = g_new0(TioApp, 1);
    app->tabs = g_ptr_array_new();
    tio_settings_init(&app->settings);
    tio_settings_load(&app->settings);

    const char *development_language = g_getenv("TIO_GUI_LANGUAGE");
    if (development_language != NULL && development_language[0] != '\0') {
        g_free(app->settings.language);
        app->settings.language = g_strdup(development_language);
    }
    apply_theme(app->settings.theme);

    install_css();

    app->window = gtk_application_window_new(application);
    g_object_set_data_full(G_OBJECT(app->window), "tio-gui", app, tio_app_free);
    g_object_set_data(G_OBJECT(application), "tio-gui-window", app->window);
    gtk_window_set_title(GTK_WINDOW(app->window),
                         g_getenv("TIO_GUI_NON_UNIQUE") != NULL
                             ? "tio-gui — Preview"
                             : "tio-gui");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1000, 660);

    /* The window owns the menu and the notebook; every session lives on a page
       inside it. */
    app->notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_scrollable(app->notebook, TRUE);
    g_signal_connect(app->notebook, "page-reordered", G_CALLBACK(on_notebook_reordered), app);
    gtk_notebook_set_show_border(app->notebook, FALSE);
    gtk_widget_set_vexpand(GTK_WIDGET(app->notebook), TRUE);
    gtk_window_set_child(GTK_WINDOW(app->window), GTK_WIDGET(app->notebook));

    app->settings_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(app->settings_button, "emblem-system-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->settings_button), _("Application settings"));
    app->settings_popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_position(app->settings_popover, GTK_POS_BOTTOM);
    gtk_widget_set_halign(GTK_WIDGET(app->settings_popover), GTK_ALIGN_START);
    gtk_popover_set_offset(app->settings_popover, 12, 0);
    gtk_popover_set_child(app->settings_popover, build_settings_popover(app));
    g_signal_connect(app->settings_popover,
                     "notify::visible",
                     G_CALLBACK(on_settings_popover_visible),
                     app);
    gtk_menu_button_set_popover(app->settings_button, GTK_WIDGET(app->settings_popover));

    app->new_tab_button = GTK_BUTTON(gtk_button_new_from_icon_name("list-add-symbolic"));
    gtk_button_set_has_frame(app->new_tab_button, FALSE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->new_tab_button), _("New session"));
    g_signal_connect_swapped(app->new_tab_button, "clicked", G_CALLBACK(tio_app_add_tab), app);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(app->settings_button));
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(app->new_tab_button));
    gtk_notebook_set_action_widget(app->notebook, actions, GTK_PACK_START);

    g_signal_connect(app->notebook, "switch-page", G_CALLBACK(on_notebook_switch_page), app);

    install_shortcuts(application, app);

    /* Reopen the recorded sessions, configured but not connected: reconnecting
       serial ports unasked is not a safe default. */
    if (app->settings.restore_tabs && app->settings.tab_configs->len > 0) {
        for (guint index = 0; index < app->settings.tab_configs->len; ++index) {
            const TioSessionConfig *session =
                g_ptr_array_index(app->settings.tab_configs, index);
            TioTab *restored = tio_app_add_tab(app);
            tio_session_config_copy(&restored->config, session);
            apply_session_config(restored, &restored->config);
            update_quick_buttons(restored);
            update_tab_label(restored);
        }
        gtk_notebook_set_current_page(app->notebook, 0);
    } else {
        (void)tio_app_add_tab(app);
    }

    g_signal_connect(app->window,
                     "close-request",
                     G_CALLBACK(on_window_close_request),
                     app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), application);

    app->preview_started_at = g_get_monotonic_time();
    tick_settings_previews(app);
    app->settings_preview_timer = g_timeout_add(250, tick_settings_previews, app);
    gtk_window_present(GTK_WINDOW(app->window));
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
