/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>
extern gboolean native_chinese;
const char *native_translate(const char *id) G_GNUC_FORMAT(1);
#undef _
#define _(id) native_translate(id)
