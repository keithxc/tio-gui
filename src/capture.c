/* SPDX-License-Identifier: GPL-3.0-only */
#include "capture.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#define QUEUE_LIMIT (8 * 1024 * 1024)
#define REPLAY_LIMIT (128 * 1024 * 1024)

typedef struct { gchar *path; guint64 size; } Part;
struct _TioCapture {
    guint refs;
    gchar *base, *header, *error;
    GOutputStream *output;
    GQueue pending, parts;
    guint64 queued, part_bytes, disk_limit, total_bytes;
    guint part_seconds, keep_files, serial;
    gint64 opened, last_stamp;
    gboolean busy, stopped;
};
static void part_free(gpointer data) { Part *part = data; g_free(part->path); g_free(part); }
TioCapture *tio_capture_ref(TioCapture *capture) { ++capture->refs; return capture; }
void tio_capture_unref(TioCapture *capture)
{
    if (!capture || --capture->refs) return;
    g_clear_object(&capture->output);
    g_queue_clear_full(&capture->pending, (GDestroyNotify)g_bytes_unref);
    g_queue_clear_full(&capture->parts, part_free);
    g_free(capture->base); g_free(capture->header); g_free(capture->error); g_free(capture);
}
static void fail(TioCapture *capture, const char *message)
{
    if (!capture->error) capture->error = g_strdup(message);
    capture->stopped = TRUE;
}
static gboolean open_part(TioCapture *capture, GError **error)
{
    g_autofree gchar *path = capture->serial == 0 ? g_strdup(capture->base)
        : g_strdup_printf("%s.part%04u", capture->base, capture->serial);
    g_autoptr(GFile) file = g_file_new_for_path(path);
    GFileOutputStream *output = g_file_create(file, G_FILE_CREATE_PRIVATE, NULL, error);
    if (!output) return FALSE;
    capture->output = G_OUTPUT_STREAM(output);
    Part *part = g_new0(Part, 1);
    part->path = g_strdup(path);
    g_queue_push_tail(&capture->parts, part);
    capture->opened = g_get_monotonic_time();
    ++capture->serial;
    g_queue_push_head(&capture->pending, g_bytes_new(capture->header, strlen(capture->header)));
    capture->queued += strlen(capture->header);
    return TRUE;
}
static void pump(TioCapture *capture);
static void written(GObject *source, GAsyncResult *result, gpointer data)
{
    TioCapture *capture = data;
    g_autoptr(GError) error = NULL;
    gsize count;
    gboolean ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, &count, &error);
    GBytes *bytes = g_queue_pop_head(&capture->pending);
    capture->queued -= g_bytes_get_size(bytes);
    g_bytes_unref(bytes);
    Part *part = g_queue_peek_tail(&capture->parts);
    part->size += count; capture->total_bytes += count;
    capture->busy = FALSE;
    if (!ok) fail(capture, error->message);
    /* Only delete files this capture created, after rotating away from them. */
    while (capture->parts.length > 1 &&
           (capture->parts.length > capture->keep_files || capture->total_bytes > capture->disk_limit)) {
        Part *old = g_queue_peek_head(&capture->parts);
        if (g_unlink(old->path) != 0) { fail(capture, "Could not remove an expired capture part"); break; }
        capture->total_bytes -= old->size;
        part_free(g_queue_pop_head(&capture->parts));
    }
    pump(capture);
    tio_capture_unref(capture);
}
static void pump(TioCapture *capture)
{
    if (capture->busy) return;
    if (capture->error) {
        g_queue_clear_full(&capture->pending, (GDestroyNotify)g_bytes_unref);
        capture->queued = 0;
    }
    if (g_queue_is_empty(&capture->pending)) {
        if (capture->stopped && capture->output) {
            g_autoptr(GError) error = NULL;
            if (!g_output_stream_close(capture->output, NULL, &error)) fail(capture, error->message);
            g_clear_object(&capture->output);
        }
        return;
    }
    GBytes *bytes = g_queue_peek_head(&capture->pending);
    Part *part = g_queue_peek_tail(&capture->parts);
    gsize length = g_bytes_get_size(bytes);
    gboolean time_limit = capture->part_seconds &&
        g_get_monotonic_time() - capture->opened >= (gint64)capture->part_seconds * G_TIME_SPAN_SECOND;
    if (part->size > strlen(capture->header) &&
        (part->size + length > capture->part_bytes || time_limit)) {
        g_autoptr(GError) error = NULL;
        if (!g_output_stream_close(capture->output, NULL, &error)) {
            fail(capture, error->message); pump(capture); return;
        }
        g_clear_object(&capture->output);
        if (!open_part(capture, &error)) { fail(capture, error->message); pump(capture); return; }
        bytes = g_queue_peek_head(&capture->pending);
        length = g_bytes_get_size(bytes);
    }
    capture->busy = TRUE;
    g_output_stream_write_all_async(capture->output, g_bytes_get_data(bytes, NULL), length,
        G_PRIORITY_DEFAULT, NULL, written, tio_capture_ref(capture));
}
TioCapture *tio_capture_new(const char *path, guint64 part_bytes, guint part_seconds,
                            guint keep_files, guint64 disk_limit, const char *metadata, GError **error)
{
    if (part_bytes < 1024 || part_bytes > REPLAY_LIMIT || keep_files == 0 || keep_files > 1000 || disk_limit < part_bytes) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid capture rotation limits"); return NULL;
    }
    TioCapture *capture = g_new0(TioCapture, 1);
    capture->refs = 1; capture->base = g_strdup(path); capture->part_bytes = part_bytes;
    capture->part_seconds = part_seconds; capture->keep_files = keep_files; capture->disk_limit = disk_limit;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "format"); json_builder_add_string_value(builder, "tio-gui-capture");
    json_builder_set_member_name(builder, "version"); json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "metadata"); json_builder_add_string_value(builder, metadata ? metadata : "");
    json_builder_end_object(builder);
    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    g_autoptr(JsonGenerator) generator = json_generator_new(); json_generator_set_root(generator, root);
    g_autofree gchar *header = json_generator_to_data(generator, NULL);
    capture->header = g_strconcat(header, "\n", NULL);
    if (strlen(capture->header) + 104 >= part_bytes || !open_part(capture, error)) {
        if (error && !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Capture metadata exceeds part size");
        tio_capture_unref(capture); return NULL;
    }
    pump(capture);
    return capture;
}
gboolean tio_capture_record(TioCapture *capture, TioCaptureKind kind, const guint8 *data, gsize length, gint64 time_us)
{
    if (!capture || capture->stopped || capture->error) return FALSE;
    if (kind > TIO_CAPTURE_INPUT || (length && !data) || time_us < 0) { fail(capture, "Invalid capture event"); pump(capture); return FALSE; }
    /* Preserve large writes as adjacent chunks with identical timestamps. */
    gsize chunk = MIN((gsize)65536, ((capture->part_bytes - strlen(capture->header) - 100) / 4) * 3);
    if (length > chunk) {
        for (gsize offset = 0; offset < length;) {
            gsize count = MIN(chunk, length - offset);
            if (!tio_capture_record(capture, kind, data + offset, count, time_us)) return FALSE;
            offset += count;
        }
        return TRUE;
    }
    time_us = MAX(time_us, capture->last_stamp);
    capture->last_stamp = time_us;
    g_autofree gchar *base64 = g_base64_encode(data, length);
    g_autofree gchar *line = g_strdup_printf("{\"t\":%" G_GINT64_FORMAT ",\"kind\":%u,\"data\":\"%s\"}\n", time_us, kind, base64);
    gsize size = strlen(line);
    if (size + strlen(capture->header) > capture->part_bytes || capture->queued + size > QUEUE_LIMIT) {
        fail(capture, "Capture queue or part limit exceeded; recording stopped without blocking serial I/O");
        pump(capture); return FALSE;
    }
    g_queue_push_tail(&capture->pending, g_bytes_new_take(g_steal_pointer(&line), size));
    capture->queued += size;
    pump(capture);
    return TRUE;
}
void tio_capture_stop(TioCapture *capture) { if (capture) { capture->stopped = TRUE; pump(capture); } }
gboolean tio_capture_finished(const TioCapture *capture) { return !capture || (capture->stopped && !capture->busy && !capture->output); }
const char *tio_capture_error(const TioCapture *capture) { return capture ? capture->error : NULL; }
const char *tio_capture_path(const TioCapture *capture)
{
    Part *part = capture ? g_queue_peek_tail((GQueue *)&capture->parts) : NULL;
    return part ? part->path : NULL;
}

/* Replay has no transport callback and therefore cannot send to a device. */
typedef struct { gint64 time; TioCaptureKind kind; GBytes *bytes; } Event;
struct _TioReplay {
    GPtrArray *events;
    TioReplayEvent event;
    gpointer data;
    guint index, timer;
    double speed, virtual_us;
    gint64 anchored;
    gboolean paused;
};
static void event_free(gpointer data) { Event *event = data; g_bytes_unref(event->bytes); g_free(event); }
static double replay_time(TioReplay *replay)
{
    return replay->virtual_us + (replay->paused ? 0 : (double)(g_get_monotonic_time() - replay->anchored) * replay->speed);
}
static gboolean replay_tick(gpointer data)
{
    TioReplay *replay = data;
    if (replay->paused) return G_SOURCE_CONTINUE;
    double time = replay_time(replay);
    guint budget = 256;
    while (replay->index < replay->events->len && budget--) {
        Event *event = g_ptr_array_index(replay->events, replay->index);
        if ((double)event->time > time) break;
        gsize length; const guint8 *bytes = g_bytes_get_data(event->bytes, &length);
        replay->event(event->kind, bytes, length, event->time, replay->data);
        ++replay->index;
    }
    if (replay->index == replay->events->len) { replay->timer = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static gboolean shallow_json(const char *text)
{
    guint depth = 0; gboolean quoted = FALSE, escaped = FALSE;
    for (const char *p = text; *p; ++p) {
        if (quoted) {
            if (escaped) escaped = FALSE; else if (*p == '\\') escaped = TRUE; else if (*p == '"') quoted = FALSE;
        } else if (*p == '"') quoted = TRUE;
        else if (*p == '{' || *p == '[') { if (++depth > 8) return FALSE; }
        else if (*p == '}' || *p == ']') { if (depth) --depth; }
    }
    return TRUE;
}
TioReplay *tio_replay_load(const char *path, TioReplayEvent callback, gpointer data, GError **error)
{
    GStatBuf info;
    if (g_stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size > REPLAY_LIMIT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a regular capture file up to 128 MiB"); return NULL;
    }
    g_autofree gchar *contents = NULL; gsize length;
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInputStream) input = g_file_read(file, NULL, error);
    if (!input) return NULL;
    g_autoptr(GByteArray) loaded = g_byte_array_new();
    guint8 block[65536];
    while (TRUE) {
        gssize count = g_input_stream_read(G_INPUT_STREAM(input), block, sizeof block, NULL, error);
        if (count < 0) return NULL;
        if (!count) break;
        if ((gsize)count + loaded->len > REPLAY_LIMIT) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Capture exceeds 128 MiB"); return NULL;
        }
        g_byte_array_append(loaded, block, (guint)count);
    }
    length = loaded->len;
    g_byte_array_append(loaded, (const guint8 *)"", 1);
    contents = (gchar *)g_byte_array_free(g_steal_pointer(&loaded), FALSE);
    if (length > REPLAY_LIMIT || memchr(contents, 0, length)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid capture file size or NUL byte"); return NULL;
    }
    TioReplay *replay = g_new0(TioReplay, 1);
    replay->events = g_ptr_array_new_with_free_func(event_free); replay->event = callback; replay->data = data; replay->speed = 1;
    char *cursor = contents; guint line_number = 0; gint64 previous = 0;
    while (*cursor) {
        g_autoptr(JsonParser) parser = NULL;
        char *newline = strchr(cursor, '\n');
        if (!newline || newline - cursor > 100000) goto invalid;
        *newline = 0;
        if (!shallow_json(cursor)) goto invalid;
        parser = json_parser_new();
        if (!json_parser_load_from_data(parser, cursor, -1, NULL)) goto invalid;
        JsonNode *root = json_parser_get_root(parser);
        if (!JSON_NODE_HOLDS_OBJECT(root)) goto invalid;
        JsonObject *object = json_node_get_object(root);
        if (line_number++ == 0) {
            JsonNode *format = json_object_get_member(object, "format");
            JsonNode *version = json_object_get_member(object, "version");
            if (!format || !JSON_NODE_HOLDS_VALUE(format) || json_node_get_value_type(format) != G_TYPE_STRING ||
                !g_str_equal(json_node_get_string(format), "tio-gui-capture") || !version ||
                !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_INT64 || json_node_get_int(version) != 1) goto invalid;
        } else {
            JsonNode *time = json_object_get_member(object, "t"), *kind = json_object_get_member(object, "kind"), *payload = json_object_get_member(object, "data");
            if (!time || !kind || !payload || !JSON_NODE_HOLDS_VALUE(time) || !JSON_NODE_HOLDS_VALUE(kind) || !JSON_NODE_HOLDS_VALUE(payload) ||
                json_node_get_value_type(time) != G_TYPE_INT64 || json_node_get_value_type(kind) != G_TYPE_INT64 ||
                json_node_get_value_type(payload) != G_TYPE_STRING) goto invalid;
            gint64 stamp = json_node_get_int(time), type = json_node_get_int(kind);
            const char *base64 = json_node_get_string(payload); gsize size = strlen(base64);
            if (stamp < previous || type < 0 || type > TIO_CAPTURE_INPUT || size % 4 || size > 87384 || replay->events->len >= 1000000) goto invalid;
            for (gsize i = 0; i < size; ++i) {
                if (g_ascii_isalnum(base64[i]) || base64[i] == '+' || base64[i] == '/') continue;
                if (base64[i] != '=' || i < size - 2 || (i + 1 < size && base64[i + 1] != '=')) goto invalid;
            }
            gsize decoded; guint8 *bytes = g_base64_decode(base64, &decoded);
            if (decoded > 65536) { g_free(bytes); goto invalid; }
            Event *event = g_new0(Event, 1);
            *event = (Event){stamp, (TioCaptureKind)type, g_bytes_new_take(bytes, decoded)};
            g_ptr_array_add(replay->events, event); previous = stamp;
        }
        cursor = newline + 1;
    }
    if (!line_number) goto invalid;
    replay->virtual_us = replay->events->len ? (double)((Event *)g_ptr_array_index(replay->events, 0))->time : 0;
    replay->anchored = g_get_monotonic_time();
    replay->paused = TRUE;
    return replay;
invalid:
    tio_replay_free(replay);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid or truncated capture near line %u", line_number + 1);
    return NULL;
}
TioReplay *tio_replay_new(const char *path, TioReplayEvent callback, gpointer data, GError **error)
{
    TioReplay *replay = tio_replay_load(path, callback, data, error);
    if (replay) tio_replay_pause(replay, FALSE);
    return replay;
}
void tio_replay_free(TioReplay *replay)
{
    if (!replay) return;
    if (replay->timer) g_source_remove(replay->timer);
    g_ptr_array_unref(replay->events); g_free(replay);
}
void tio_replay_pause(TioReplay *replay, gboolean paused)
{
    if (!replay || replay->paused == paused) return;
    replay->virtual_us = replay_time(replay); replay->anchored = g_get_monotonic_time(); replay->paused = paused;
    if (!paused && !replay->timer && replay->index < replay->events->len)
        replay->timer = g_timeout_add(10, replay_tick, replay);
}
void tio_replay_speed(TioReplay *replay, double speed)
{
    if (!replay || !isfinite(speed) || speed < .1 || speed > 32) return;
    replay->virtual_us = replay_time(replay); replay->anchored = g_get_monotonic_time(); replay->speed = speed;
}
gboolean tio_replay_finished(const TioReplay *replay) { return !replay || replay->index >= replay->events->len; }
guint tio_replay_position(const TioReplay *replay) { return replay ? replay->index : 0; }
guint tio_replay_count(const TioReplay *replay) { return replay ? replay->events->len : 0; }
