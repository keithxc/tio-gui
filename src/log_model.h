/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

typedef enum { TIO_LOG_CRITICAL, TIO_LOG_ERROR, TIO_LOG_WARNING, TIO_LOG_INFO,
               TIO_LOG_DEBUG, TIO_LOG_COMMAND, TIO_LOG_DATA, TIO_LOG_LEVELS } TioLogLevel;
typedef struct {
    guint64 id;
    gint64 time_us;
    gchar *text;
    TioLogLevel level;
} TioLogEntry;
typedef struct _TioLogModel TioLogModel;
typedef struct _TioLogFilter TioLogFilter;

TioLogModel *tio_log_model_new(void);
void tio_log_model_free(TioLogModel *model);
void tio_log_model_clear(TioLogModel *model);
void tio_log_model_feed(TioLogModel *model, const guint8 *bytes, gsize length, gint64 time_us);
void tio_log_model_command(TioLogModel *model, const char *text, gint64 time_us);
const GQueue *tio_log_model_entries(const TioLogModel *model);
guint64 tio_log_model_count(const TioLogModel *model, TioLogLevel level);
guint64 tio_log_model_revision(const TioLogModel *model);
const char *tio_log_level_name(TioLogLevel level);
/* Caller owns parsed JSON. Non-JSON lines expose key/value or CSV col1..colN fields. */
JsonNode *tio_log_entry_fields(const TioLogEntry *entry);
gboolean tio_log_entry_number(const TioLogEntry *entry, const char *field, double *value);

TioLogFilter *tio_log_filter_new(const char *pattern, gboolean regex, gboolean case_sensitive,
                                guint level_mask, GError **error);
void tio_log_filter_free(TioLogFilter *filter);
gboolean tio_log_filter_matches(TioLogFilter *filter, const TioLogEntry *entry);
guint64 tio_log_filter_limited(const TioLogFilter *filter);
/* CSV values are quoted and spreadsheet formula prefixes escaped. */
gchar *tio_log_csv_row(const TioLogEntry *entry, const char *field);
gboolean tio_log_filter_number(TioLogFilter *filter, const TioLogEntry *entry,
                               const char *capture, double *value);
gchar *tio_log_model_csv(const TioLogModel *model, TioLogFilter *filter);
