/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "settings.h"
gchar *tio_quick_presets_encode(const TioSessionConfig *config, gsize *length);
gboolean tio_quick_presets_decode(TioSessionConfig *config, const char *data,
                                  gsize length, GError **error);
