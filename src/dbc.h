/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>
typedef struct _TioDbc TioDbc;
TioDbc *tio_dbc_parse(const char *text,gsize length,GError **error);
void tio_dbc_free(TioDbc *dbc);
/* JSON numeric signal fields, or NULL if no matching message / malformed data. */
gchar *tio_dbc_decode(TioDbc *dbc,guint32 id,gboolean extended,const guint8 *bytes,gsize length,GError **error);
