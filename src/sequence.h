/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <glib.h>

typedef struct {
    gchar *payload;
    guint mode, ending, crc, delay_ms;
} TioSequenceStep;

typedef struct {
    gchar *name;
    GPtrArray *steps;
} TioSequence;

typedef struct _TioSequenceRunner TioSequenceRunner;
/* WAIT leaves the current step pending. ACCEPT means the transport owns a copy.
 * The transport must report WAIT for the next step until its prior write ends. */
typedef enum { TIO_SEQUENCE_WAIT, TIO_SEQUENCE_ACCEPT, TIO_SEQUENCE_ERROR } TioSequenceSendResult;
typedef TioSequenceSendResult (*TioSequenceSend)(const GByteArray *, gpointer);

TioSequence *tio_sequence_new(const char *name);
void tio_sequence_free(TioSequence *sequence);
void tio_sequence_add(TioSequence *sequence, const char *payload, guint mode,
                      guint ending, guint crc, guint delay_ms);
gboolean tio_sequence_validate(const TioSequence *sequence, GError **error);
gchar *tio_sequence_encode(const TioSequence *sequence, gsize *length);
TioSequence *tio_sequence_decode(const char *text, gsize length, GError **error);

TioSequenceRunner *tio_sequence_runner_new(const TioSequence *sequence, gboolean loop,
                                         TioSequenceSend send, gpointer data, GError **error);
void tio_sequence_runner_free(TioSequenceRunner *runner);
void tio_sequence_runner_pause(TioSequenceRunner *runner, gboolean paused);
gboolean tio_sequence_runner_active(const TioSequenceRunner *runner);
gboolean tio_sequence_runner_failed(const TioSequenceRunner *runner);
guint tio_sequence_runner_step(const TioSequenceRunner *runner);
