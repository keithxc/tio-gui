/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gio/gio.h>
typedef struct _TioCapture TioCapture;
typedef enum { TIO_CAPTURE_RX, TIO_CAPTURE_TX, TIO_CAPTURE_CONNECT,
               TIO_CAPTURE_DISCONNECT, TIO_CAPTURE_PARAMETERS, TIO_CAPTURE_INPUT } TioCaptureKind;
TioCapture *tio_capture_new(const char *path, guint64 part_bytes, guint part_seconds,
                            guint keep_files, guint64 disk_limit, const char *metadata, GError **error);
TioCapture *tio_capture_ref(TioCapture *capture);
void tio_capture_unref(TioCapture *capture);
gboolean tio_capture_record(TioCapture *capture, TioCaptureKind kind, const guint8 *data,
                            gsize length, gint64 time_us);
void tio_capture_stop(TioCapture *capture);
gboolean tio_capture_finished(const TioCapture *capture);
const char *tio_capture_error(const TioCapture *capture);
const char *tio_capture_path(const TioCapture *capture);

typedef struct _TioReplay TioReplay;
typedef void (*TioReplayEvent)(TioCaptureKind kind, const guint8 *data, gsize length, gint64 time_us, gpointer user_data);
/* Load without starting; safe to call from a worker thread. Resume starts playback. */
TioReplay *tio_replay_load(const char *path, TioReplayEvent event, gpointer data, GError **error);
TioReplay *tio_replay_new(const char *path, TioReplayEvent event, gpointer data, GError **error);
void tio_replay_free(TioReplay *replay);
void tio_replay_pause(TioReplay *replay, gboolean paused);
void tio_replay_speed(TioReplay *replay, double speed);
gboolean tio_replay_finished(const TioReplay *replay);
guint tio_replay_position(const TioReplay *replay);
guint tio_replay_count(const TioReplay *replay);
