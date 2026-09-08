/* SPDX-License-Identifier: GPL-3.0-only */
#include "log_model.h"
#include <string.h>
#include "text_line_cases.h"

static void progress(void)
{
    TioLogModel *model = tio_log_model_new();
    for (guint i = 0; i < G_N_ELEMENTS(text_line_cases); ++i) {
        const char *input = text_line_cases[i].input;
        for (guint chunk = 1; chunk <= strlen(input); ++chunk) {
            tio_log_model_clear(model);
            for (gsize offset = 0; offset < strlen(input); offset += chunk)
                tio_log_model_feed(model, (const guint8 *)input + offset,
                                   MIN(chunk, strlen(input) - offset), 123);
            g_autoptr(GString) actual = g_string_new(NULL);
            for (GList *it = tio_log_model_entries(model)->head; it; it = it->next) {
                TioLogEntry *entry = it->data;
                g_string_append(actual, entry->text);
                g_string_append_c(actual, '\n');
            }
            g_assert_cmpstr(actual->str, ==, text_line_cases[i].expected);
        }
    }
    g_assert_cmpuint(tio_log_model_count(model, TIO_LOG_ERROR), ==, 0);
    g_assert_cmpuint(tio_log_model_count(model, TIO_LOG_INFO), ==, 1);
    tio_log_model_clear(model);
    const char *pending = "ERROR 10%\r";
    tio_log_model_feed(model, (const guint8 *)pending, strlen(pending), 123);
    g_assert_cmpuint(tio_log_model_entries(model)->length, ==, 0);
    tio_log_model_command(model, "status", 124);
    const char *done = "INFO 100%\x1b[K\n";
    tio_log_model_feed(model, (const guint8 *)done, strlen(done), 125);
    g_assert_cmpuint(tio_log_model_entries(model)->length, ==, 2);
    g_assert_cmpuint(tio_log_model_count(model, TIO_LOG_ERROR), ==, 0);
    g_assert_cmpuint(tio_log_model_count(model, TIO_LOG_COMMAND), ==, 1);
    tio_log_model_free(model);
}
static void parsing(void)
{
    TioLogModel *model = tio_log_model_new();
    const char *a = "\x1b[31mERROR temp=23.5 voltage: 3.3\x1b[0m\r";
    tio_log_model_feed(model, (const guint8 *)a, strlen(a), 123);
    const char *b = "\n{\"sensor\":{\"temp\":24.5},\"level\":\"INFO\"}\n1,2.5,3\n";
    tio_log_model_feed(model, (const guint8 *)b, strlen(b), 456);
    const GQueue *entries = tio_log_model_entries(model);
    g_assert_cmpuint(entries->length, ==, 3);
    g_assert_cmpuint(tio_log_model_count(model, TIO_LOG_ERROR), ==, 1);
    g_assert_cmpuint(tio_log_model_count(model, TIO_LOG_INFO), ==, 1);
    TioLogEntry *first = entries->head->data;
    double value;
    g_assert_true(tio_log_entry_number(first, "temp", &value));
    g_assert_cmpfloat(value, ==, 23.5);
    g_assert_true(tio_log_entry_number(entries->head->next->data, "sensor.temp", &value));
    g_assert_cmpfloat(value, ==, 24.5);
    g_assert_true(tio_log_entry_number(entries->tail->data, "col2", &value));
    g_assert_cmpfloat(value, ==, 2.5);
    TioLogEntry quoted = {.text = "\"1\",\"2.5\",\"a,b\""};
    g_assert_true(tio_log_entry_number(&quoted, "col2", &value));
    g_assert_cmpfloat(value, ==, 2.5);
    TioLogEntry array = {.text = "{\"values\":[4,5]}"};
    g_assert_true(tio_log_entry_number(&array, "values.1", &value));
    g_assert_cmpfloat(value, ==, 5);
    TioLogFilter *filter = tio_log_filter_new("ERROR", FALSE, FALSE, 127, NULL);
    g_assert_true(tio_log_filter_matches(filter, first));
    g_assert_false(tio_log_filter_matches(filter, entries->tail->data));
    tio_log_filter_free(filter);
    g_autofree gchar *csv = tio_log_model_csv(model, NULL);
    g_assert_nonnull(strstr(csv, "\"temp\""));
    g_assert_nonnull(strstr(csv, "\"voltage\""));
    g_assert_nonnull(strstr(csv, "\"col2\""));
    tio_log_model_free(model);
}
static void limits(void)
{
    TioLogModel *model = tio_log_model_new();
    for (guint i = 0; i < 10050; ++i) tio_log_model_feed(model, (const guint8 *)"INFO x\n", 7, i);
    g_assert_cmpuint(tio_log_model_entries(model)->length, ==, 10000);
    g_assert_cmpuint(tio_log_model_count(model, TIO_LOG_INFO), ==, 10050);
    TioLogFilter *filter = tio_log_filter_new("^(a+)+$", TRUE, TRUE, 127, NULL);
    g_assert_nonnull(filter);
    TioLogEntry entry = {.text = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", .level = TIO_LOG_DATA};
    gint64 started = g_get_monotonic_time();
    g_assert_false(tio_log_filter_matches(filter, &entry));
    g_assert_cmpint(g_get_monotonic_time() - started, <, G_TIME_SPAN_SECOND);
    g_assert_cmpuint(tio_log_filter_limited(filter), >, 0);
    tio_log_filter_free(filter);
    tio_log_model_free(model);
}
static void csv(void)
{
    TioLogEntry entry = {.time_us = 42, .text = "  =HYPERLINK(\"x\")", .level = TIO_LOG_DATA};
    g_autofree gchar *row = tio_log_csv_row(&entry, NULL);
    g_assert_cmpstr(row, ==, "42,DATA,\"'  =HYPERLINK(\"\"x\"\")\"\n");
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/log-model/parsing", parsing);
    g_test_add_func("/log-model/retention-regex-limits", limits);
    g_test_add_func("/log-model/csv-formula", csv);
    g_test_add_func("/log-model/progress-line-editing", progress);
    return g_test_run();
}
