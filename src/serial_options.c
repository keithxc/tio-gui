/* SPDX-License-Identifier: GPL-3.0-only */
#include "serial_options.h"
#include <string.h>

gboolean tio_serial_options_validate(const TioSessionConfig *config, GError **error)
{
    g_auto(GStrv) parts = NULL;
    if (config->dtr_default > 2 || config->rts_default > 2 || config->line_pulse_ms > 10000) goto invalid;
    if (!config->rs485 || !*config->rs485_config) return TRUE;
    parts = g_strsplit(config->rs485_config, ",", -1);
    const char *flags[] = {"RX_DURING_TX", NULL};
    for (guint i = 0; parts[i]; ++i) {
        char *part = g_strstrip(parts[i]);
        if (g_strv_contains(flags, part)) continue;
        const char *keys[] = {"RTS_ON_SEND", "RTS_AFTER_SEND", "RTS_DELAY_BEFORE_SEND", "RTS_DELAY_AFTER_SEND", NULL};
        gboolean found = FALSE;
        for (guint k = 0; keys[k]; ++k) {
            gsize n = strlen(keys[k]);
            if (g_str_has_prefix(part, keys[k]) && part[n] == '=') {
                const char *value = part + n + 1;
                if (!*value) goto invalid;
                for (const char *p = value; *p; ++p) if (!g_ascii_isdigit(*p)) goto invalid;
                guint64 number = g_ascii_strtoull(value, NULL, 10);
                if (number > (k < 2 ? 1u : 10000u)) goto invalid;
                found = TRUE;
                break;
            }
        }
        if (!found) goto invalid;
    }
    return TRUE;
invalid:
    g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
        "Invalid serial line options. RS-485 accepts RTS_ON_SEND=0|1, RTS_AFTER_SEND=0|1, "
        "RTS_DELAY_BEFORE_SEND=ms, RTS_DELAY_AFTER_SEND=ms and RX_DURING_TX (comma-separated).");
    return FALSE;
}

gchar *tio_line_script(guint line, gboolean high)
{
    g_return_val_if_fail(line < 2, NULL);
    return g_strdup_printf("local api = tio or _G; api.set{%s=%u}\n", line == 0 ? "DTR" : "RTS", high ? 1u : 0u);
}

void tio_serial_options_append(GPtrArray *arguments, const TioSessionConfig *config)
{
    if (config->dtr_default || config->rts_default) {
        GString *script = g_string_new("local api = tio or _G; api.set{");
        if (config->dtr_default) g_string_append_printf(script, "DTR=%u,", config->dtr_default - 1);
        if (config->rts_default) g_string_append_printf(script, "RTS=%u,", config->rts_default - 1);
        g_string_append(script, "}");
        g_ptr_array_add(arguments, g_strdup("--script"));
        g_ptr_array_add(arguments, g_string_free(script, FALSE));
    }
    g_ptr_array_add(arguments, g_strdup("--line-pulse-duration"));
    g_ptr_array_add(arguments, g_strdup_printf("DTR=%u,RTS=%u", config->line_pulse_ms, config->line_pulse_ms));
    if (config->rs485) {
        g_ptr_array_add(arguments, g_strdup("--rs-485"));
        if (*config->rs485_config) {
            g_ptr_array_add(arguments, g_strdup("--rs-485-config"));
            g_auto(GStrv) parts = g_strsplit(config->rs485_config, ",", -1);
            for (guint i = 0; parts[i]; ++i) g_strstrip(parts[i]);
            g_ptr_array_add(arguments, g_strjoinv(",", parts));
        }
    }
}
