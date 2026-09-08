/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>
/* Linux: observe the exact child without opening or modifying its serial port. */
gchar *tio_connection_device(GPid pid, const char *preferred_device);
