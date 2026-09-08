/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include <glib.h>

#define TIO_GUI_QUICK_BUTTON_COUNT 4
#define TIO_GUI_HISTORY_LIMIT 100

/* Everything a connection profile can capture. The live session uses the same
   structure, so saving a profile is a plain snapshot instead of a second list
   of fields that has to be kept in step by hand. */
typedef struct {
    gchar *device;
    /* Stable /dev/serial/by-id path for `device`, when the kernel provides
       one. Profiles prefer it so a saved entry survives ttyUSB renumbering. */
    gchar *device_id;
    gchar *baud;
    gchar *data_bits;
    gchar *stop_bits;
    gchar *parity;
    gchar *flow;
    guint dtr_default, rts_default; /* 0 unchanged, 1 low, 2 high */
    guint line_pulse_ms;
    gboolean rs485;
    gchar *rs485_config;
    gboolean local_echo;
    gboolean hex_output;
    gboolean timestamps;
    gchar *timestamp_format; /* 24hour, 24hour-start, 24hour-delta, iso8601 or epoch */
    gchar *line_ending; /* none, lf, cr or crlf */
    guint output_delay;
    guint output_line_delay;
    gboolean logging;
    gchar *log_directory;
    gchar *log_file; /* empty means tio-gui generates a dated name */
    gboolean log_append;
    gboolean log_strip;
    gchar *quick_labels[TIO_GUI_QUICK_BUTTON_COUNT];
    gchar *quick_payloads[TIO_GUI_QUICK_BUTTON_COUNT];
    guint quick_delays[TIO_GUI_QUICK_BUTTON_COUNT]; /* milliseconds before sending */
    guint quick_modes[TIO_GUI_QUICK_BUTTON_COUNT]; /* 0 text, 1 HEX */
    guint quick_endings[TIO_GUI_QUICK_BUTTON_COUNT]; /* none, LF, CR, CRLF */
    guint quick_crcs[TIO_GUI_QUICK_BUTTON_COUNT]; /* none, CRC8, MODBUS, CRC32 */
} TioSessionConfig;

typedef struct {
    gchar *name;
    TioSessionConfig session;
} TioProfile;

typedef struct {
    /* Starting values for a new session. Today a single session both seeds
       itself from this and writes back to it on connect; once sessions become
       per-tab, a tab keeps its own copy and only new tabs read this. */
    TioSessionConfig defaults;
    gchar *language;
    gchar *theme; /* system, light or dark */
    gchar *active_profile;
    gboolean show_all_ttys;
    gboolean advanced_expanded;
    guint log_warning_mb; /* 0 disables the log size warning */
    /* Reopen the sessions that were open at the last exit. Off by default:
       reconnecting serial ports without being asked is not always wanted. */
    gboolean restore_tabs;
    GPtrArray *profiles;    /* TioProfile *, in display order */
    GPtrArray *sequences;  /* serialized named sequences */
    GPtrArray *history;     /* gchar *, oldest first */
    GPtrArray *tab_configs; /* TioSessionConfig *, in tab order */
} TioSettings;

void tio_settings_init(TioSettings *settings);
void tio_settings_load(TioSettings *settings);
gboolean tio_settings_save(const TioSettings *settings, GError **error);
gboolean tio_settings_load_from_file(TioSettings *settings, const char *path, GError **error);
gboolean tio_settings_save_to_file(const TioSettings *settings, const char *path, GError **error);
void tio_settings_clear(TioSettings *settings);

void tio_session_config_init(TioSessionConfig *config);
void tio_session_config_copy(TioSessionConfig *destination, const TioSessionConfig *source);
void tio_session_config_clear(TioSessionConfig *config);

TioProfile *tio_settings_find_profile(const TioSettings *settings, const char *name);
void tio_settings_store_profile(TioSettings *settings,
                                const char *name,
                                const TioSessionConfig *session);
void tio_settings_remove_profile(TioSettings *settings, const char *name);

void tio_settings_clear_tabs(TioSettings *settings);
void tio_settings_add_tab(TioSettings *settings, const TioSessionConfig *session);

void tio_settings_push_history(TioSettings *settings, const char *text);
void tio_settings_clear_history(TioSettings *settings);
