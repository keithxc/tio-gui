/* SPDX-License-Identifier: GPL-3.0-only */

#include "settings.h"

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>

#define TIO_GUI_DEFAULTS_GROUP "defaults"
/* 0.2.x wrote the same fields under [session]. Read-only, for migration. */
#define TIO_GUI_LEGACY_SESSION_GROUP "session"
#define TIO_GUI_PROFILE_PREFIX "profile:"
#define TIO_GUI_TAB_PREFIX "tab:"

static gchar *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "tio-gui", "config.ini", NULL);
}

static void replace_string_from_key(GKeyFile *key_file,
                                    const char *group,
                                    const char *key,
                                    gchar **destination)
{
    g_autoptr(GError) error = NULL;
    gchar *value = g_key_file_get_string(key_file, group, key, &error);
    if (error == NULL && value != NULL && value[0] != '\0') {
        g_free(*destination);
        *destination = value;
    } else {
        g_free(value);
    }
}

/* Quick-button labels and log filenames are allowed to be empty, so they need
   a setter that does not treat "" as "keep the default". */
static void replace_optional_string_from_key(GKeyFile *key_file,
                                             const char *group,
                                             const char *key,
                                             gchar **destination)
{
    g_autoptr(GError) error = NULL;
    gchar *value = g_key_file_get_string(key_file, group, key, &error);
    if (error == NULL && value != NULL) {
        g_free(*destination);
        *destination = value;
    } else {
        g_free(value);
    }
}

static void replace_boolean_from_key(GKeyFile *key_file,
                                     const char *group,
                                     const char *key,
                                     gboolean *destination)
{
    g_autoptr(GError) error = NULL;
    gboolean value = g_key_file_get_boolean(key_file, group, key, &error);
    if (error == NULL) {
        *destination = value;
    }
}

static void replace_uint_from_key(GKeyFile *key_file,
                                  const char *group,
                                  const char *key,
                                  guint *destination,
                                  guint maximum)
{
    g_autoptr(GError) error = NULL;
    gint value = g_key_file_get_integer(key_file, group, key, &error);
    if (error == NULL && value >= 0 && (guint)value <= maximum) {
        *destination = (guint)value;
    }
}

static void replace_string(gchar **destination, const char *value)
{
    gchar *copy = g_strdup(value);
    g_free(*destination);
    *destination = copy;
}

void tio_session_config_init(TioSessionConfig *config)
{
    g_return_if_fail(config != NULL);

    const char *documents = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    g_autofree gchar *fallback_documents = NULL;
    if (documents == NULL || documents[0] == '\0') {
        fallback_documents = g_build_filename(g_get_home_dir(), "Documents", NULL);
        documents = fallback_documents;
    }

    *config = (TioSessionConfig){
        .baud = g_strdup("115200"),
        .capture_part_mb = 16,
        .capture_part_seconds = 0,
        .capture_keep_files = 8,
        .capture_disk_mb = 128,
        .reconnect = TRUE,
        .exclude_devices = g_strdup(""),
        .exclude_drivers = g_strdup(""),
        .exclude_tids = g_strdup(""),
        .line_pulse_ms = 100,
        .rs485_config = g_strdup(""),
        .data_bits = g_strdup("8"),
        .stop_bits = g_strdup("1"),
        .parity = g_strdup("none"),
        .flow = g_strdup("none"),
        .timestamp_format = g_strdup("iso8601"),
        .line_ending = g_strdup("cr"),
        .log_directory = g_build_filename(documents, "tio-gui", NULL),
        .log_file = g_strdup(""),
        .log_strip = TRUE,
    };

    static const char *const default_labels[TIO_GUI_QUICK_BUTTON_COUNT] = {
        "Ctrl-C", "Enter", "Tab", "Escape",
    };
    static const char *const default_payloads[TIO_GUI_QUICK_BUTTON_COUNT] = {
        "\\x03", "\\r", "\\t", "\\x1b",
    };
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        config->quick_labels[index] = g_strdup(default_labels[index]);
        config->quick_payloads[index] = g_strdup(default_payloads[index]);
    }
}

void tio_session_config_copy(TioSessionConfig *destination, const TioSessionConfig *source)
{
    g_return_if_fail(destination != NULL);
    g_return_if_fail(source != NULL);

    if (destination == source) {
        return;
    }

    replace_string(&destination->device, source->device);
    replace_string(&destination->device_id, source->device_id);
    replace_string(&destination->baud, source->baud);
    replace_string(&destination->data_bits, source->data_bits);
    replace_string(&destination->stop_bits, source->stop_bits);
    replace_string(&destination->parity, source->parity);
    replace_string(&destination->flow, source->flow);
    replace_string(&destination->line_ending, source->line_ending);
    replace_string(&destination->timestamp_format, source->timestamp_format);
    replace_string(&destination->log_directory, source->log_directory);
    replace_string(&destination->log_file, source->log_file);
    replace_string(&destination->rs485_config, source->rs485_config);
    destination->rs485 = source->rs485;
    destination->capture_part_mb = source->capture_part_mb;
    destination->capture_part_seconds = source->capture_part_seconds;
    destination->capture_keep_files = source->capture_keep_files;
    destination->capture_disk_mb = source->capture_disk_mb;
    destination->reconnect = source->reconnect;
    destination->connection_notify = source->connection_notify;
    destination->connection_sound = source->connection_sound;
    destination->auto_connect = source->auto_connect;
    replace_string(&destination->exclude_devices, source->exclude_devices);
    replace_string(&destination->exclude_drivers, source->exclude_drivers);
    replace_string(&destination->exclude_tids, source->exclude_tids);

    destination->dtr_default = source->dtr_default;
    destination->rts_default = source->rts_default;
    destination->line_pulse_ms = source->line_pulse_ms;
    destination->local_echo = source->local_echo;
    destination->hex_output = source->hex_output;
    destination->timestamps = source->timestamps;
    destination->output_delay = source->output_delay;
    destination->output_line_delay = source->output_line_delay;
    destination->logging = source->logging;
    destination->log_append = source->log_append;
    destination->log_strip = source->log_strip;
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        replace_string(&destination->quick_labels[index], source->quick_labels[index]);
        replace_string(&destination->quick_payloads[index], source->quick_payloads[index]);
        destination->quick_delays[index] = source->quick_delays[index];
        destination->quick_modes[index] = source->quick_modes[index];
        destination->quick_endings[index] = source->quick_endings[index];
        destination->quick_crcs[index] = source->quick_crcs[index];

    }
}

void tio_session_config_clear(TioSessionConfig *config)
{
    if (config == NULL) {
        return;
    }

    g_clear_pointer(&config->rs485_config, g_free);
    g_clear_pointer(&config->exclude_devices, g_free);
    g_clear_pointer(&config->exclude_drivers, g_free);
    g_clear_pointer(&config->exclude_tids, g_free);

    g_clear_pointer(&config->device, g_free);
    g_clear_pointer(&config->device_id, g_free);
    g_clear_pointer(&config->baud, g_free);
    g_clear_pointer(&config->data_bits, g_free);
    g_clear_pointer(&config->stop_bits, g_free);
    g_clear_pointer(&config->parity, g_free);
    g_clear_pointer(&config->flow, g_free);
    g_clear_pointer(&config->line_ending, g_free);
    g_clear_pointer(&config->timestamp_format, g_free);
    g_clear_pointer(&config->log_directory, g_free);
    g_clear_pointer(&config->log_file, g_free);
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_clear_pointer(&config->quick_labels[index], g_free);
        g_clear_pointer(&config->quick_payloads[index], g_free);
    }
}

static void session_config_free(gpointer data)
{
    TioSessionConfig *config = data;
    if (config == NULL) {
        return;
    }
    tio_session_config_clear(config);
    g_free(config);
}

static void profile_free(gpointer data)
{
    TioProfile *profile = data;
    if (profile == NULL) {
        return;
    }
    g_free(profile->name);
    tio_session_config_clear(&profile->session);
    g_free(profile);
}

static void session_config_read(GKeyFile *key_file,
                                const char *group,
                                TioSessionConfig *config)
{
    replace_string_from_key(key_file, group, "device", &config->device);
    replace_string_from_key(key_file, group, "device-id", &config->device_id);
    replace_string_from_key(key_file, group, "baud", &config->baud);
    replace_string_from_key(key_file, group, "data-bits", &config->data_bits);
    replace_string_from_key(key_file, group, "stop-bits", &config->stop_bits);
    replace_string_from_key(key_file, group, "parity", &config->parity);
    replace_string_from_key(key_file, group, "flow", &config->flow);
    replace_string_from_key(key_file, group, "line-ending", &config->line_ending);
    replace_uint_from_key(key_file, group, "capture-part-mb", &config->capture_part_mb, 128);
    replace_uint_from_key(key_file, group, "capture-part-seconds", &config->capture_part_seconds, 86400);
    replace_uint_from_key(key_file, group, "capture-keep-files", &config->capture_keep_files, 1000);
    replace_uint_from_key(key_file, group, "capture-disk-mb", &config->capture_disk_mb, 1048576);
    replace_boolean_from_key(key_file, group, "reconnect", &config->reconnect);
    replace_boolean_from_key(key_file, group, "connection-notify", &config->connection_notify);
    replace_boolean_from_key(key_file, group, "connection-sound", &config->connection_sound);
    replace_uint_from_key(key_file, group, "auto-connect", &config->auto_connect, 2);
    replace_optional_string_from_key(key_file, group, "exclude-devices", &config->exclude_devices);
    replace_optional_string_from_key(key_file, group, "exclude-drivers", &config->exclude_drivers);
    replace_optional_string_from_key(key_file, group, "exclude-tids", &config->exclude_tids);
    replace_uint_from_key(key_file, group, "dtr-default", &config->dtr_default, 2);
    replace_uint_from_key(key_file, group, "rts-default", &config->rts_default, 2);
    replace_uint_from_key(key_file, group, "line-pulse-ms", &config->line_pulse_ms, 10000);
    replace_boolean_from_key(key_file, group, "rs485", &config->rs485);
    replace_optional_string_from_key(key_file, group, "rs485-config", &config->rs485_config);
    replace_boolean_from_key(key_file, group, "local-echo", &config->local_echo);
    replace_boolean_from_key(key_file, group, "hex-output", &config->hex_output);
    replace_boolean_from_key(key_file, group, "timestamps", &config->timestamps);
    replace_string_from_key(key_file, group, "timestamp-format", &config->timestamp_format);
    replace_uint_from_key(key_file, group, "output-delay", &config->output_delay, 10000);
    replace_uint_from_key(key_file,
                          group,
                          "output-line-delay",
                          &config->output_line_delay,
                          10000);
    replace_boolean_from_key(key_file, group, "logging", &config->logging);
    replace_string_from_key(key_file, group, "log-directory", &config->log_directory);
    replace_optional_string_from_key(key_file, group, "log-file", &config->log_file);
    replace_boolean_from_key(key_file, group, "log-append", &config->log_append);
    replace_boolean_from_key(key_file, group, "log-strip", &config->log_strip);
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_autofree gchar *label_key = g_strdup_printf("label-%u", index + 1);
        g_autofree gchar *payload_key = g_strdup_printf("payload-%u", index + 1);
        g_autofree gchar *modes_key = g_strdup_printf("quick-modes-%u", index + 1);
        replace_uint_from_key(key_file, group, modes_key, &config->quick_modes[index], 1);
        g_autofree gchar *delay_key = g_strdup_printf("quick-delay-%u", index + 1);
        replace_uint_from_key(key_file, group, delay_key, &config->quick_delays[index], 60000);
        g_autofree gchar *endings_key = g_strdup_printf("quick-endings-%u", index + 1);
        replace_uint_from_key(key_file, group, endings_key, &config->quick_endings[index], 3);
        g_autofree gchar *crcs_key = g_strdup_printf("quick-crcs-%u", index + 1);
        replace_uint_from_key(key_file, group, crcs_key, &config->quick_crcs[index], 3);

        replace_optional_string_from_key(key_file,
                                         group,
                                         label_key,
                                         &config->quick_labels[index]);
        replace_optional_string_from_key(key_file,
                                         group,
                                         payload_key,
                                         &config->quick_payloads[index]);
    }
}

static void session_config_write(GKeyFile *key_file,
                                 const char *group,
                                 const TioSessionConfig *config)
{
    if (config->device != NULL) {
        g_key_file_set_string(key_file, group, "device", config->device);
    }
    if (config->device_id != NULL) {
        g_key_file_set_string(key_file, group, "device-id", config->device_id);
    }
    g_key_file_set_string(key_file, group, "baud", config->baud);
    g_key_file_set_string(key_file, group, "data-bits", config->data_bits);
    g_key_file_set_string(key_file, group, "stop-bits", config->stop_bits);
    g_key_file_set_string(key_file, group, "parity", config->parity);
    g_key_file_set_string(key_file, group, "flow", config->flow);
    g_key_file_set_string(key_file, group, "line-ending", config->line_ending);
    g_key_file_set_integer(key_file, group, "capture-part-mb", (gint)config->capture_part_mb);
    g_key_file_set_integer(key_file, group, "capture-part-seconds", (gint)config->capture_part_seconds);
    g_key_file_set_integer(key_file, group, "capture-keep-files", (gint)config->capture_keep_files);
    g_key_file_set_integer(key_file, group, "capture-disk-mb", (gint)config->capture_disk_mb);
    g_key_file_set_boolean(key_file, group, "reconnect", config->reconnect);
    g_key_file_set_boolean(key_file, group, "connection-notify", config->connection_notify);
    g_key_file_set_boolean(key_file, group, "connection-sound", config->connection_sound);
    g_key_file_set_integer(key_file, group, "auto-connect", (gint)config->auto_connect);
    g_key_file_set_string(key_file, group, "exclude-devices", config->exclude_devices);
    g_key_file_set_string(key_file, group, "exclude-drivers", config->exclude_drivers);
    g_key_file_set_string(key_file, group, "exclude-tids", config->exclude_tids);
    g_key_file_set_integer(key_file, group, "dtr-default", (gint)config->dtr_default);
    g_key_file_set_integer(key_file, group, "rts-default", (gint)config->rts_default);
    g_key_file_set_integer(key_file, group, "line-pulse-ms", (gint)config->line_pulse_ms);
    g_key_file_set_boolean(key_file, group, "rs485", config->rs485);
    g_key_file_set_string(key_file, group, "rs485-config", config->rs485_config);
    g_key_file_set_boolean(key_file, group, "local-echo", config->local_echo);
    g_key_file_set_boolean(key_file, group, "hex-output", config->hex_output);
    g_key_file_set_boolean(key_file, group, "timestamps", config->timestamps);
    g_key_file_set_string(key_file, group, "timestamp-format", config->timestamp_format);
    g_key_file_set_integer(key_file, group, "output-delay", (gint)config->output_delay);
    g_key_file_set_integer(key_file,
                           group,
                           "output-line-delay",
                           (gint)config->output_line_delay);
    g_key_file_set_boolean(key_file, group, "logging", config->logging);
    g_key_file_set_string(key_file, group, "log-directory", config->log_directory);
    g_key_file_set_string(key_file, group, "log-file", config->log_file);
    g_key_file_set_boolean(key_file, group, "log-append", config->log_append);
    g_key_file_set_boolean(key_file, group, "log-strip", config->log_strip);
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_autofree gchar *label_key = g_strdup_printf("label-%u", index + 1);
        g_autofree gchar *payload_key = g_strdup_printf("payload-%u", index + 1);
        g_autofree gchar *modes_key = g_strdup_printf("quick-modes-%u", index + 1);
        g_key_file_set_integer(key_file, group, modes_key, (gint)config->quick_modes[index]);
        g_autofree gchar *delay_key = g_strdup_printf("quick-delay-%u", index + 1);
        g_key_file_set_integer(key_file, group, delay_key, (gint)config->quick_delays[index]);
        g_autofree gchar *endings_key = g_strdup_printf("quick-endings-%u", index + 1);
        g_key_file_set_integer(key_file, group, endings_key, (gint)config->quick_endings[index]);
        g_autofree gchar *crcs_key = g_strdup_printf("quick-crcs-%u", index + 1);
        g_key_file_set_integer(key_file, group, crcs_key, (gint)config->quick_crcs[index]);

        g_key_file_set_string(key_file, group, label_key, config->quick_labels[index]);
        g_key_file_set_string(key_file, group, payload_key, config->quick_payloads[index]);
    }
}

/* config.ini written by 0.1.x spread the session over four groups. Read it once
   so an upgrade keeps the user's device, baud rate and quick buttons. */
static void session_config_read_legacy(GKeyFile *key_file, TioSessionConfig *config)
{
    replace_string_from_key(key_file, "serial", "device", &config->device);
    replace_string_from_key(key_file, "serial", "baud", &config->baud);
    replace_string_from_key(key_file, "serial", "data-bits", &config->data_bits);
    replace_string_from_key(key_file, "serial", "stop-bits", &config->stop_bits);
    replace_string_from_key(key_file, "serial", "parity", &config->parity);
    replace_string_from_key(key_file, "serial", "flow", &config->flow);
    replace_boolean_from_key(key_file, "serial", "local-echo", &config->local_echo);
    replace_boolean_from_key(key_file, "display", "timestamps", &config->timestamps);
    replace_boolean_from_key(key_file, "display", "hex-output", &config->hex_output);
    replace_boolean_from_key(key_file, "logging", "enabled", &config->logging);
    replace_string_from_key(key_file, "logging", "directory", &config->log_directory);
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_autofree gchar *label_key = g_strdup_printf("label-%u", index + 1);
        g_autofree gchar *payload_key = g_strdup_printf("payload-%u", index + 1);
        replace_optional_string_from_key(key_file,
                                         "quick-buttons",
                                         label_key,
                                         &config->quick_labels[index]);
        replace_optional_string_from_key(key_file,
                                         "quick-buttons",
                                         payload_key,
                                         &config->quick_payloads[index]);
    }
}

void tio_settings_init(TioSettings *settings)
{
    g_return_if_fail(settings != NULL);

    *settings = (TioSettings){
        .language = g_strdup("system"),
        .theme = g_strdup("system"),
        .log_warning_mb = 256,
        .profiles = g_ptr_array_new_with_free_func(profile_free),
        .history = g_ptr_array_new_with_free_func(g_free),
        .sequences = g_ptr_array_new_with_free_func(g_free),
        .tab_configs = g_ptr_array_new_with_free_func(session_config_free),
    };
    tio_session_config_init(&settings->defaults);
}

gboolean tio_settings_load_from_file(TioSettings *settings, const char *path, GError **error)
{
    g_return_val_if_fail(settings != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    g_autoptr(GKeyFile) key_file = g_key_file_new();
    if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, error)) {
        return FALSE;
    }

    if (g_key_file_has_group(key_file, TIO_GUI_DEFAULTS_GROUP)) {
        session_config_read(key_file, TIO_GUI_DEFAULTS_GROUP, &settings->defaults);
    } else if (g_key_file_has_group(key_file, TIO_GUI_LEGACY_SESSION_GROUP)) {
        session_config_read(key_file, TIO_GUI_LEGACY_SESSION_GROUP, &settings->defaults);
    } else {
        session_config_read_legacy(key_file, &settings->defaults);
    }

    replace_string_from_key(key_file, "general", "language", &settings->language);
    replace_string_from_key(key_file, "general", "theme", &settings->theme);
    replace_string_from_key(key_file, "general", "active-profile", &settings->active_profile);
    replace_boolean_from_key(key_file, "general", "show-all-ttys", &settings->show_all_ttys);
    replace_boolean_from_key(key_file,
                             "general",
                             "advanced-expanded",
                             &settings->advanced_expanded);
    replace_uint_from_key(key_file, "general", "log-warning-mb", &settings->log_warning_mb, 65536);
    replace_boolean_from_key(key_file, "general", "restore-tabs", &settings->restore_tabs);
    /* 0.1.x kept these two in other groups. */
    replace_string_from_key(key_file, "display", "language", &settings->language);
    replace_boolean_from_key(key_file, "serial", "show-all-ttys", &settings->show_all_ttys);

    gsize sequence_count = 0;
    g_auto(GStrv) sequences = g_key_file_get_string_list(key_file, "send", "sequences", &sequence_count, NULL);
    g_ptr_array_set_size(settings->sequences, 0);
    for (gsize i = 0; i < MIN(sequence_count, 100u); ++i)
        g_ptr_array_add(settings->sequences, g_strdup(sequences[i]));

    gsize history_length = 0;
    g_auto(GStrv) history =
        g_key_file_get_string_list(key_file, "send", "history", &history_length, NULL);
    for (gsize index = 0; index < history_length; ++index) {
        if (history[index][0] != '\0') {
            g_ptr_array_add(settings->history, g_strdup(history[index]));
        }
    }

    gsize group_count = 0;
    g_auto(GStrv) groups = g_key_file_get_groups(key_file, &group_count);
    for (gsize index = 0; index < group_count; ++index) {
        if (!g_str_has_prefix(groups[index], TIO_GUI_PROFILE_PREFIX)) {
            continue;
        }
        const char *name = groups[index] + strlen(TIO_GUI_PROFILE_PREFIX);
        if (name[0] == '\0') {
            continue;
        }
        TioProfile *profile = g_new0(TioProfile, 1);
        profile->name = g_strdup(name);
        tio_session_config_init(&profile->session);
        session_config_read(key_file, groups[index], &profile->session);
        g_ptr_array_add(settings->profiles, profile);
    }

    /* Read [tab:N] by index rather than by file order, so the tabs come back
       in the order they were in. */
    for (guint position = 0; position < 64; ++position) {
        g_autofree gchar *group = g_strdup_printf("%s%u", TIO_GUI_TAB_PREFIX, position);
        if (!g_key_file_has_group(key_file, group)) {
            continue;
        }
        TioSessionConfig *session = g_new0(TioSessionConfig, 1);
        tio_session_config_init(session);
        session_config_read(key_file, group, session);
        g_ptr_array_add(settings->tab_configs, session);
    }
    return TRUE;
}

void tio_settings_load(TioSettings *settings)
{
    g_return_if_fail(settings != NULL);

    g_autofree gchar *path = settings_path();
    (void)tio_settings_load_from_file(settings, path, NULL);
}

gboolean tio_settings_save_to_file(const TioSettings *settings, const char *path, GError **error)
{
    g_return_val_if_fail(settings != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    g_autoptr(GKeyFile) key_file = g_key_file_new();
    g_key_file_set_string(key_file, "general", "language", settings->language);
    g_key_file_set_string(key_file, "general", "theme", settings->theme);
    g_key_file_set_string(key_file,
                          "general",
                          "active-profile",
                          settings->active_profile != NULL ? settings->active_profile : "");
    g_key_file_set_boolean(key_file, "general", "show-all-ttys", settings->show_all_ttys);
    g_key_file_set_boolean(key_file,
                           "general",
                           "advanced-expanded",
                           settings->advanced_expanded);
    g_key_file_set_integer(key_file, "general", "log-warning-mb", (gint)settings->log_warning_mb);
    g_key_file_set_boolean(key_file, "general", "restore-tabs", settings->restore_tabs);
    session_config_write(key_file, TIO_GUI_DEFAULTS_GROUP, &settings->defaults);

    if (settings->sequences->len)
        g_key_file_set_string_list(key_file, "send", "sequences",
            (const gchar *const *)settings->sequences->pdata, settings->sequences->len);

    if (settings->history->len > 0) {
        g_key_file_set_string_list(key_file,
                                   "send",
                                   "history",
                                   (const gchar *const *)settings->history->pdata,
                                   settings->history->len);
    }

    for (guint index = 0; index < settings->profiles->len; ++index) {
        const TioProfile *profile = g_ptr_array_index(settings->profiles, index);
        g_autofree gchar *group =
            g_strconcat(TIO_GUI_PROFILE_PREFIX, profile->name, NULL);
        session_config_write(key_file, group, &profile->session);
    }

    for (guint index = 0; index < settings->tab_configs->len; ++index) {
        const TioSessionConfig *session = g_ptr_array_index(settings->tab_configs, index);
        g_autofree gchar *group = g_strdup_printf("%s%u", TIO_GUI_TAB_PREFIX, index);
        session_config_write(key_file, group, session);
    }

    gsize length = 0;
    g_autofree gchar *contents = g_key_file_to_data(key_file, &length, error);
    if (contents == NULL) {
        return FALSE;
    }

    g_autofree gchar *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) == -1) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "Could not create settings directory: %s",
                    g_strerror(errno));
        return FALSE;
    }

    if (!g_file_set_contents(path, contents, (gssize)length, error)) {
        return FALSE;
    }
    if (g_chmod(path, 0600) == -1) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "Could not protect settings file: %s",
                    g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

gboolean tio_settings_save(const TioSettings *settings, GError **error)
{
    g_autofree gchar *path = settings_path();
    return tio_settings_save_to_file(settings, path, error);
}

void tio_settings_clear(TioSettings *settings)
{
    if (settings == NULL) {
        return;
    }

    tio_session_config_clear(&settings->defaults);
    g_clear_pointer(&settings->language, g_free);
    g_clear_pointer(&settings->theme, g_free);
    g_clear_pointer(&settings->active_profile, g_free);
    g_clear_pointer(&settings->profiles, g_ptr_array_unref);
    g_clear_pointer(&settings->history, g_ptr_array_unref);
    g_clear_pointer(&settings->sequences, g_ptr_array_unref);
    g_clear_pointer(&settings->tab_configs, g_ptr_array_unref);
}

void tio_settings_clear_tabs(TioSettings *settings)
{
    g_return_if_fail(settings != NULL);
    g_ptr_array_set_size(settings->tab_configs, 0);
}

void tio_settings_add_tab(TioSettings *settings, const TioSessionConfig *session)
{
    g_return_if_fail(settings != NULL);
    g_return_if_fail(session != NULL);

    TioSessionConfig *copy = g_new0(TioSessionConfig, 1);
    tio_session_config_init(copy);
    tio_session_config_copy(copy, session);
    g_ptr_array_add(settings->tab_configs, copy);
}

TioProfile *tio_settings_find_profile(const TioSettings *settings, const char *name)
{
    g_return_val_if_fail(settings != NULL, NULL);

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (guint index = 0; index < settings->profiles->len; ++index) {
        TioProfile *profile = g_ptr_array_index(settings->profiles, index);
        if (g_strcmp0(profile->name, name) == 0) {
            return profile;
        }
    }
    return NULL;
}

void tio_settings_store_profile(TioSettings *settings,
                                const char *name,
                                const TioSessionConfig *session)
{
    g_return_if_fail(settings != NULL);
    g_return_if_fail(name != NULL && name[0] != '\0');
    g_return_if_fail(session != NULL);

    TioProfile *profile = tio_settings_find_profile(settings, name);
    if (profile == NULL) {
        profile = g_new0(TioProfile, 1);
        profile->name = g_strdup(name);
        tio_session_config_init(&profile->session);
        g_ptr_array_add(settings->profiles, profile);
    }
    tio_session_config_copy(&profile->session, session);
}

void tio_settings_remove_profile(TioSettings *settings, const char *name)
{
    g_return_if_fail(settings != NULL);

    TioProfile *profile = tio_settings_find_profile(settings, name);
    if (profile != NULL) {
        g_ptr_array_remove(settings->profiles, profile);
    }
}

void tio_settings_push_history(TioSettings *settings, const char *text)
{
    g_return_if_fail(settings != NULL);

    if (text == NULL || text[0] == '\0') {
        return;
    }
    for (guint index = 0; index < settings->history->len; ++index) {
        if (g_strcmp0(g_ptr_array_index(settings->history, index), text) == 0) {
            g_ptr_array_remove_index(settings->history, index);
            break;
        }
    }
    g_ptr_array_add(settings->history, g_strdup(text));
    while (settings->history->len > TIO_GUI_HISTORY_LIMIT) {
        g_ptr_array_remove_index(settings->history, 0);
    }
}

void tio_settings_clear_history(TioSettings *settings)
{
    g_return_if_fail(settings != NULL);

    g_ptr_array_set_size(settings->history, 0);
}

static void make_session_portable(TioSessionConfig *session)
{
    g_clear_pointer(&session->device, g_free);
    g_clear_pointer(&session->device_id, g_free);
    replace_string(&session->log_directory, "");
    replace_string(&session->log_file, "");
    replace_string(&session->exclude_devices, "");
    replace_string(&session->exclude_drivers, "");
    replace_string(&session->exclude_tids, "");
}

gboolean tio_settings_export_portable(const TioSettings *settings, const char *path, GError **error)
{
    TioSettings portable;
    tio_settings_init(&portable);
    tio_session_config_copy(&portable.defaults, &settings->defaults);
    make_session_portable(&portable.defaults);
    replace_string(&portable.language, settings->language);
    replace_string(&portable.theme, settings->theme);
    portable.log_warning_mb = settings->log_warning_mb;
    for (guint i = 0; i < settings->profiles->len; ++i) {
        TioProfile *profile = g_ptr_array_index(settings->profiles, i);
        tio_settings_store_profile(&portable, profile->name, &profile->session);
        make_session_portable(&tio_settings_find_profile(&portable, profile->name)->session);
    }
    for (guint i = 0; i < settings->sequences->len; ++i)
        g_ptr_array_add(portable.sequences, g_strdup(g_ptr_array_index(settings->sequences, i)));
    gboolean ok = tio_settings_save_to_file(&portable, path, error);
    tio_settings_clear(&portable);
    return ok;
}
