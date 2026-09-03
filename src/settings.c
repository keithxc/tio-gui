/* SPDX-License-Identifier: GPL-3.0-only */

#include "settings.h"

#include <errno.h>

#include <glib/gstdio.h>

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

void tio_settings_init(TioSettings *settings)
{
    g_return_if_fail(settings != NULL);

    const char *documents = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    g_autofree gchar *fallback_documents = NULL;
    if (documents == NULL || documents[0] == '\0') {
        fallback_documents = g_build_filename(g_get_home_dir(), "Documents", NULL);
        documents = fallback_documents;
    }

    *settings = (TioSettings){
        .baud = g_strdup("115200"),
        .data_bits = g_strdup("8"),
        .stop_bits = g_strdup("1"),
        .parity = g_strdup("none"),
        .flow = g_strdup("none"),
        .log_directory = g_build_filename(documents, "tio-gui", NULL),
    };

    static const char *const default_labels[TIO_GUI_QUICK_BUTTON_COUNT] = {
        "Ctrl-C", "Enter", "Tab", "Escape",
    };
    static const char *const default_payloads[TIO_GUI_QUICK_BUTTON_COUNT] = {
        "\\x03", "\\r", "\\t", "\\x1b",
    };
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        settings->quick_labels[index] = g_strdup(default_labels[index]);
        settings->quick_payloads[index] = g_strdup(default_payloads[index]);
    }
}

void tio_settings_load(TioSettings *settings)
{
    g_return_if_fail(settings != NULL);

    g_autofree gchar *path = settings_path();
    g_autoptr(GKeyFile) key_file = g_key_file_new();
    if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL)) {
        return;
    }

    replace_string_from_key(key_file, "serial", "device", &settings->device);
    replace_string_from_key(key_file, "serial", "baud", &settings->baud);
    replace_string_from_key(key_file, "serial", "data-bits", &settings->data_bits);
    replace_string_from_key(key_file, "serial", "stop-bits", &settings->stop_bits);
    replace_string_from_key(key_file, "serial", "parity", &settings->parity);
    replace_string_from_key(key_file, "serial", "flow", &settings->flow);
    replace_boolean_from_key(key_file, "serial", "local-echo", &settings->local_echo);
    replace_boolean_from_key(key_file, "serial", "show-all-ttys", &settings->show_all_ttys);
    replace_boolean_from_key(key_file, "display", "timestamps", &settings->timestamps);
    replace_boolean_from_key(key_file, "display", "hex-output", &settings->hex_output);
    replace_boolean_from_key(key_file, "logging", "enabled", &settings->logging);
    replace_string_from_key(key_file, "logging", "directory", &settings->log_directory);
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_autofree gchar *label_key = g_strdup_printf("label-%u", index + 1);
        g_autofree gchar *payload_key = g_strdup_printf("payload-%u", index + 1);
        replace_string_from_key(key_file,
                                "quick-buttons",
                                label_key,
                                &settings->quick_labels[index]);
        replace_string_from_key(key_file,
                                "quick-buttons",
                                payload_key,
                                &settings->quick_payloads[index]);
    }
}

gboolean tio_settings_save(const TioSettings *settings, GError **error)
{
    g_return_val_if_fail(settings != NULL, FALSE);

    g_autoptr(GKeyFile) key_file = g_key_file_new();
    if (settings->device != NULL) {
        g_key_file_set_string(key_file, "serial", "device", settings->device);
    }
    g_key_file_set_string(key_file, "serial", "baud", settings->baud);
    g_key_file_set_string(key_file, "serial", "data-bits", settings->data_bits);
    g_key_file_set_string(key_file, "serial", "stop-bits", settings->stop_bits);
    g_key_file_set_string(key_file, "serial", "parity", settings->parity);
    g_key_file_set_string(key_file, "serial", "flow", settings->flow);
    g_key_file_set_boolean(key_file, "serial", "local-echo", settings->local_echo);
    g_key_file_set_boolean(key_file, "serial", "show-all-ttys", settings->show_all_ttys);
    g_key_file_set_boolean(key_file, "display", "timestamps", settings->timestamps);
    g_key_file_set_boolean(key_file, "display", "hex-output", settings->hex_output);
    g_key_file_set_boolean(key_file, "logging", "enabled", settings->logging);
    g_key_file_set_string(key_file, "logging", "directory", settings->log_directory);
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_autofree gchar *label_key = g_strdup_printf("label-%u", index + 1);
        g_autofree gchar *payload_key = g_strdup_printf("payload-%u", index + 1);
        g_key_file_set_string(key_file,
                              "quick-buttons",
                              label_key,
                              settings->quick_labels[index]);
        g_key_file_set_string(key_file,
                              "quick-buttons",
                              payload_key,
                              settings->quick_payloads[index]);
    }

    gsize length = 0;
    g_autofree gchar *contents = g_key_file_to_data(key_file, &length, error);
    if (contents == NULL) {
        return FALSE;
    }

    g_autofree gchar *path = settings_path();
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

void tio_settings_clear(TioSettings *settings)
{
    if (settings == NULL) {
        return;
    }

    g_clear_pointer(&settings->device, g_free);
    g_clear_pointer(&settings->baud, g_free);
    g_clear_pointer(&settings->data_bits, g_free);
    g_clear_pointer(&settings->stop_bits, g_free);
    g_clear_pointer(&settings->parity, g_free);
    g_clear_pointer(&settings->flow, g_free);
    g_clear_pointer(&settings->log_directory, g_free);
    for (guint index = 0; index < TIO_GUI_QUICK_BUTTON_COUNT; ++index) {
        g_clear_pointer(&settings->quick_labels[index], g_free);
        g_clear_pointer(&settings->quick_payloads[index], g_free);
    }
}
