/* SPDX-License-Identifier: GPL-3.0-only */
#include "quick_presets.h"
#include "payload.h"

gchar *tio_quick_presets_encode(const TioSessionConfig *config, gsize *length)
{
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_set_integer(file, "quick-buttons", "version", 1);
    for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
        g_autofree gchar *group = g_strdup_printf("button-%u", i + 1);
        g_key_file_set_string(file, group, "label", config->quick_labels[i]);
        g_key_file_set_string(file, group, "payload", config->quick_payloads[i]);
        g_key_file_set_integer(file, group, "mode", (gint)config->quick_modes[i]);
        g_key_file_set_integer(file, group, "ending", (gint)config->quick_endings[i]);
        g_key_file_set_integer(file, group, "crc", (gint)config->quick_crcs[i]);
        g_key_file_set_integer(file, group, "delay", (gint)config->quick_delays[i]);
    }
    return g_key_file_to_data(file, length, NULL);
}

gboolean tio_quick_presets_decode(TioSessionConfig *config, const char *data,
                                  gsize length, GError **error)
{
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (length > 1024 * 1024) {
        g_set_error_literal(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                            "Button group exceeds 1 MiB");
        return FALSE;
    }
    if (!g_key_file_load_from_data(file, data, length, G_KEY_FILE_NONE, error)) return FALSE;
    if (g_key_file_get_integer(file, "quick-buttons", "version", NULL) != 1) {
        g_set_error_literal(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                            "Unsupported button group version");
        return FALSE;
    }
    TioSessionConfig parsed;
    tio_session_config_init(&parsed);
    gboolean ok = FALSE;
    for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
        g_autofree gchar *group = g_strdup_printf("button-%u", i + 1);
        g_autofree gchar *payload = NULL;
        g_autoptr(GByteArray) bytes = NULL;
        g_autofree gchar *label = g_key_file_get_string(file, group, "label", error);
        if (!label) goto done;
        payload = g_key_file_get_string(file, group, "payload", error);
        if (!payload) goto done;
        const char *keys[] = {"mode", "ending", "crc", "delay"};
        guint *values[] = {&parsed.quick_modes[i], &parsed.quick_endings[i],
                          &parsed.quick_crcs[i], &parsed.quick_delays[i]};
        const gint limits[] = {1, 3, 3, 60000};
        for (guint k = 0; k < 4; ++k) {
            g_autoptr(GError) local = NULL;
            gint value = g_key_file_get_integer(file, group, keys[k], &local);
            if (local) { g_propagate_error(error, g_steal_pointer(&local)); goto done; }
            if (value < 0 || value > limits[k]) {
                g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                            "Invalid %s in %s", keys[k], group);
                goto done;
            }
            *values[k] = (guint)value;
        }
        bytes = tio_payload_build(payload, parsed.quick_modes[i] != 0,
            parsed.quick_endings[i], parsed.quick_crcs[i], error);
        if (!bytes) goto done;
        g_free(parsed.quick_labels[i]);
        g_free(parsed.quick_payloads[i]);
        parsed.quick_labels[i] = g_steal_pointer(&label);
        parsed.quick_payloads[i] = g_steal_pointer(&payload);
    }
    for (guint i = 0; i < TIO_GUI_QUICK_BUTTON_COUNT; ++i) {
        g_free(config->quick_labels[i]);
        g_free(config->quick_payloads[i]);
        config->quick_labels[i] = g_strdup(parsed.quick_labels[i]);
        config->quick_payloads[i] = g_strdup(parsed.quick_payloads[i]);
        config->quick_modes[i] = parsed.quick_modes[i];
        config->quick_endings[i] = parsed.quick_endings[i];
        config->quick_crcs[i] = parsed.quick_crcs[i];
        config->quick_delays[i] = parsed.quick_delays[i];
    }
    ok = TRUE;
done:
    tio_session_config_clear(&parsed);
    return ok;
}
