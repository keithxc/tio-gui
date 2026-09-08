/* SPDX-License-Identifier: GPL-3.0-only */
#include "sequence.h"
#include "payload.h"

static void step_free(gpointer data)
{
    TioSequenceStep *step = data;
    g_free(step->payload);
    g_free(step);
}

TioSequence *tio_sequence_new(const char *name)
{
    TioSequence *sequence = g_new0(TioSequence, 1);
    sequence->name = g_strdup(name);
    sequence->steps = g_ptr_array_new_with_free_func(step_free);
    return sequence;
}

void tio_sequence_free(TioSequence *sequence)
{
    if (!sequence) return;
    g_free(sequence->name);
    g_ptr_array_unref(sequence->steps);
    g_free(sequence);
}

void tio_sequence_add(TioSequence *sequence, const char *payload, guint mode,
                      guint ending, guint crc, guint delay_ms)
{
    TioSequenceStep *step = g_new0(TioSequenceStep, 1);
    *step = (TioSequenceStep){g_strdup(payload), mode, ending, crc, delay_ms};
    g_ptr_array_add(sequence->steps, step);
}

gboolean tio_sequence_validate(const TioSequence *sequence, GError **error)
{
    if (!sequence || !sequence->name || !*sequence->name ||
        !g_utf8_validate(sequence->name, -1, NULL) ||
        sequence->steps->len == 0 || sequence->steps->len > 256) {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                            "A sequence needs a name and 1–256 steps");
        return FALSE;
    }
    for (guint i = 0; i < sequence->steps->len; ++i) {
        TioSequenceStep *step = g_ptr_array_index(sequence->steps, i);
        if (step->mode > 1 || step->delay_ms > 60000) {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                        "Invalid mode or delay at step %u", i + 1);
            return FALSE;
        }
        g_autoptr(GByteArray) bytes = tio_payload_build(step->payload, step->mode != 0,
                                                       step->ending, step->crc, error);
        if (!bytes) { g_prefix_error(error, "Step %u: ", i + 1); return FALSE; }
    }
    return TRUE;
}

gchar *tio_sequence_encode(const TioSequence *sequence, gsize *length)
{
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_set_integer(file, "sequence", "version", 1);
    g_key_file_set_string(file, "sequence", "name", sequence->name);
    g_key_file_set_integer(file, "sequence", "count", (gint)sequence->steps->len);
    for (guint i = 0; i < sequence->steps->len; ++i) {
        TioSequenceStep *step = g_ptr_array_index(sequence->steps, i);
        g_autofree gchar *group = g_strdup_printf("step-%u", i + 1);
        g_key_file_set_string(file, group, "payload", step->payload);
        g_key_file_set_integer(file, group, "mode", (gint)step->mode);
        g_key_file_set_integer(file, group, "ending", (gint)step->ending);
        g_key_file_set_integer(file, group, "crc", (gint)step->crc);
        g_key_file_set_integer(file, group, "delay", (gint)step->delay_ms);
    }
    return g_key_file_to_data(file, length, NULL);
}

TioSequence *tio_sequence_decode(const char *text, gsize length, GError **error)
{
    if (length > 20 * 1024 * 1024) {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Sequence exceeds 20 MiB");
        return NULL;
    }
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_data(file, text, length, G_KEY_FILE_NONE, error)) return NULL;
    if (g_key_file_get_integer(file, "sequence", "version", NULL) != 1) {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Unsupported sequence version");
        return NULL;
    }
    g_autofree gchar *name = g_key_file_get_string(file, "sequence", "name", error);
    if (!name) return NULL;
    gint count = g_key_file_get_integer(file, "sequence", "count", NULL);
    if (count < 1 || count > 256) {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Sequence needs 1–256 steps");
        return NULL;
    }
    TioSequence *sequence = tio_sequence_new(name);
    for (gint i = 0; i < count; ++i) {
        g_autofree gchar *group = g_strdup_printf("step-%d", i + 1);
        g_autofree gchar *payload = g_key_file_get_string(file, group, "payload", error);
        if (!payload) goto invalid;
        const char *keys[] = {"mode", "ending", "crc", "delay"};
        guint values[4];
        for (guint k = 0; k < 4; ++k) {
            g_autoptr(GError) local = NULL;
            gint value = g_key_file_get_integer(file, group, keys[k], &local);
            if (local) { g_propagate_error(error, g_steal_pointer(&local)); goto invalid; }
            if (value < 0) {
                g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Negative %s", keys[k]);
                goto invalid;
            }
            values[k] = (guint)value;
        }
        tio_sequence_add(sequence, payload, values[0], values[1], values[2], values[3]);
    }
    if (!tio_sequence_validate(sequence, error)) goto invalid;
    return sequence;
invalid:
    tio_sequence_free(sequence);
    return NULL;
}

struct _TioSequenceRunner {
    GPtrArray *payloads;
    GArray *delays;
    TioSequenceSend send;
    gpointer data;
    guint timer, step, last_delay;
    gint64 due, remaining;
    gboolean paused, loop, failed, finishing, awaiting;
};

static gboolean tick(gpointer data)
{
    TioSequenceRunner *runner = data;
    if (runner->paused || g_get_monotonic_time() < runner->due) return G_SOURCE_CONTINUE;
    /* An empty transport probe waits for completion of the last accepted write. */
    GByteArray probe = {0};
    TioSequenceSendResult ready = runner->send(&probe, runner->data);
    if (ready == TIO_SEQUENCE_WAIT) return G_SOURCE_CONTINUE;
    if (ready == TIO_SEQUENCE_ERROR) goto failed;
    if (runner->awaiting) {
        runner->awaiting = FALSE;
        runner->due = g_get_monotonic_time() + (gint64)MAX(runner->last_delay, 10u) * 1000;
        return G_SOURCE_CONTINUE;
    }
    if (runner->finishing) {
        if (!runner->loop) { runner->timer = 0; return G_SOURCE_REMOVE; }
        runner->step = 0;
        runner->finishing = FALSE;
    }
    GByteArray *payload = g_ptr_array_index(runner->payloads, runner->step);
    TioSequenceSendResult result = runner->send(payload, runner->data);
    if (result == TIO_SEQUENCE_WAIT) return G_SOURCE_CONTINUE;
    if (result == TIO_SEQUENCE_ERROR) goto failed;
    runner->last_delay = g_array_index(runner->delays, guint, runner->step);
    runner->awaiting = TRUE;
    ++runner->step;
    runner->finishing = runner->step == runner->payloads->len;
    return G_SOURCE_CONTINUE;
failed:
    runner->failed = TRUE;
    runner->timer = 0;
    return G_SOURCE_REMOVE;
}

TioSequenceRunner *tio_sequence_runner_new(const TioSequence *sequence, gboolean loop,
                                         TioSequenceSend send, gpointer data, GError **error)
{
    if (!tio_sequence_validate(sequence, error)) return NULL;
    TioSequenceRunner *runner = g_new0(TioSequenceRunner, 1);
    runner->payloads = g_ptr_array_new_with_free_func((GDestroyNotify)g_byte_array_unref);
    runner->delays = g_array_new(FALSE, FALSE, sizeof(guint));
    runner->send = send;
    runner->data = data;
    runner->loop = loop;
    for (guint i = 0; i < sequence->steps->len; ++i) {
        TioSequenceStep *step = g_ptr_array_index(sequence->steps, i);
        g_ptr_array_add(runner->payloads, tio_payload_build(step->payload, step->mode != 0,
                                                         step->ending, step->crc, NULL));
        g_array_append_val(runner->delays, step->delay_ms);
    }
    runner->timer = g_timeout_add(10, tick, runner);
    return runner;
}

void tio_sequence_runner_free(TioSequenceRunner *runner)
{
    if (!runner) return;
    if (runner->timer) g_source_remove(runner->timer);
    g_ptr_array_unref(runner->payloads);
    g_array_unref(runner->delays);
    g_free(runner);
}

void tio_sequence_runner_pause(TioSequenceRunner *runner, gboolean paused)
{
    if (!runner || !runner->timer || runner->paused == paused) return;
    if (paused) runner->remaining = MAX(0, runner->due - g_get_monotonic_time());
    else runner->due = g_get_monotonic_time() + runner->remaining;
    runner->paused = paused;
}

gboolean tio_sequence_runner_active(const TioSequenceRunner *runner) { return runner && runner->timer; }
gboolean tio_sequence_runner_failed(const TioSequenceRunner *runner) { return runner && runner->failed; }
guint tio_sequence_runner_step(const TioSequenceRunner *runner) { return runner ? runner->step : 0; }
