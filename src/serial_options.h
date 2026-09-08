/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "settings.h"
gboolean tio_serial_options_validate(const TioSessionConfig *config, GError **error);
void tio_serial_options_append(GPtrArray *arguments, const TioSessionConfig *config);
gchar *tio_line_script(guint line, gboolean high);
