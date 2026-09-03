/* SPDX-License-Identifier: GPL-3.0-only */

#pragma once

#include <glib.h>

#define TIO_GUI_QUICK_BUTTON_COUNT 4

typedef struct {
    gchar *device;
    gchar *baud;
    gchar *data_bits;
    gchar *stop_bits;
    gchar *parity;
    gchar *flow;
    gchar *log_directory;
    gboolean local_echo;
    gboolean show_all_ttys;
    gboolean timestamps;
    gboolean logging;
    gboolean hex_output;
    gchar *quick_labels[TIO_GUI_QUICK_BUTTON_COUNT];
    gchar *quick_payloads[TIO_GUI_QUICK_BUTTON_COUNT];
} TioSettings;

void tio_settings_init(TioSettings *settings);
void tio_settings_load(TioSettings *settings);
gboolean tio_settings_save(const TioSettings *settings, GError **error);
void tio_settings_clear(TioSettings *settings);
