/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gtk/gtk.h>
#include "log_model.h"
GtkWidget *tio_analyzer_new(GtkWindow *parent, TioLogModel *model);

GtkWidget *tio_analyzer_open_replay(GtkWindow *parent);
