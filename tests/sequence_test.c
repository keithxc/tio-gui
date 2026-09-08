/* SPDX-License-Identifier: GPL-3.0-only */
#include "sequence.h"
#include <string.h>
typedef struct { GByteArray *bytes; guint writes; gboolean busy, fail; gint64 last; } Sink;
static TioSequenceSendResult send_step(const GByteArray *bytes, gpointer data)
{
    Sink *sink = data;
    if (sink->fail) return TIO_SEQUENCE_ERROR;
    if (sink->busy) return TIO_SEQUENCE_WAIT;
    if (bytes->len) {
        g_byte_array_append(sink->bytes, bytes->data, bytes->len);
        sink->writes++;
        sink->last = g_get_monotonic_time();
    }
    return TIO_SEQUENCE_ACCEPT;
}
static void pump(guint ms)
{
    gint64 until = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < until) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
}
static void lifecycle(void)
{
    TioSequence *sequence = tio_sequence_new("Boot check");
    tio_sequence_add(sequence, "00 14 FF", 1, 0, 0, 80);
    tio_sequence_add(sequence, "AT", 0, 3, 0, 0);
    Sink sink = {.bytes = g_byte_array_new(), .busy = TRUE};
    TioSequenceRunner *runner = tio_sequence_runner_new(sequence, FALSE, send_step, &sink, NULL);
    g_assert_nonnull(runner);
    pump(40);
    g_assert_cmpuint(sink.writes, ==, 0);
    sink.busy = FALSE;
    pump(30);
    g_assert_cmpuint(sink.writes, ==, 1);
    tio_sequence_runner_pause(runner, TRUE);
    pump(130);
    g_assert_cmpuint(sink.writes, ==, 1);
    tio_sequence_runner_pause(runner, FALSE);
    pump(160);
    g_assert_false(tio_sequence_runner_active(runner));
    g_assert_false(tio_sequence_runner_failed(runner));
    const guint8 expected[] = {0, 0x14, 0xff, 'A', 'T', '\r', '\n'};
    g_assert_cmpmem(sink.bytes->data, sink.bytes->len, expected, sizeof expected);
    tio_sequence_runner_free(runner);
    sink.writes = 0;
    runner = tio_sequence_runner_new(sequence, TRUE, send_step, &sink, NULL);
    pump(260);
    g_assert_cmpuint(sink.writes, >=, 3);
    tio_sequence_runner_free(runner);
    guint writes = sink.writes;
    pump(100);
    g_assert_cmpuint(sink.writes, ==, writes);
    sink.fail = TRUE;
    runner = tio_sequence_runner_new(sequence, FALSE, send_step, &sink, NULL);
    pump(30);
    g_assert_false(tio_sequence_runner_active(runner));
    g_assert_true(tio_sequence_runner_failed(runner));
    tio_sequence_runner_free(runner);
    tio_sequence_free(sequence);
    g_byte_array_unref(sink.bytes);
}
static void serialization(void)
{
    TioSequence *sequence = tio_sequence_new("Reset and query");
    tio_sequence_add(sequence, "\\x03", 0, 0, 0, 100);
    tio_sequence_add(sequence, "01 03 00 00 00 0A", 1, 0, 2, 200);
    gsize length;
    g_autofree gchar *text = tio_sequence_encode(sequence, &length);
    TioSequence *restored = tio_sequence_decode(text, length, NULL);
    g_assert_nonnull(restored);
    g_assert_cmpstr(restored->name, ==, sequence->name);
    g_assert_cmpuint(restored->steps->len, ==, 2);
    TioSequenceStep *step = g_ptr_array_index(restored->steps, 1);
    g_assert_cmpuint(step->crc, ==, 2);
    g_assert_cmpuint(step->delay_ms, ==, 200);
    const char *invalid = "[sequence]\nversion=1\nname=X\ncount=1\n[step-1]\npayload=\\\\x\nmode=0\nending=0\ncrc=0\ndelay=0\n";
    g_assert_null(tio_sequence_decode(invalid, strlen(invalid), NULL));
    tio_sequence_free(sequence);
    tio_sequence_free(restored);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sequence/lifecycle", lifecycle);
    g_test_add_func("/sequence/serialization", serialization);
    return g_test_run();
}
