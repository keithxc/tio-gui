/* SPDX-License-Identifier: GPL-3.0-only */
#include "modbus.h"
#include <string.h>
static guint16 crc16(const guint8 *bytes, gsize length)
{
    guint16 crc = 0xffff;
    for (gsize i = 0; i < length; ++i) { crc ^= bytes[i]; for (guint j = 0; j < 8; ++j) crc = (guint16)((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0)); }
    return crc;
}
static guint16 word(const guint8 *p) { return (guint16)((p[0] << 8) | p[1]); }
static void put(guint8 *p, guint16 value) { p[0] = (guint8)(value >> 8); p[1] = (guint8)value; }
GBytes *tio_modbus_frame(const TioModbusRequest *r, GError **error)
{
    if (!r || !r->unit || r->unit > (r->tcp ? 255 : 247) || r->function < 1 || r->function > 6 ||
        (r->function <= 4 && (!r->quantity || r->quantity > (r->function <= 2 ? 2000 : 125) || (guint)r->address + r->quantity > 65536)) ||
        (r->function == 5 && r->value > 1)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid Modbus unit/function/address/count/value"); return NULL;
    }
    guint8 frame[12] = {0}; guint offset = r->tcp ? 6 : 0;
    if (r->tcp) { put(frame, r->transaction); put(frame + 4, 6); }
    frame[offset] = r->unit; frame[offset + 1] = r->function;
    put(frame + offset + 2, r->address);
    put(frame + offset + 4, r->function <= 4 ? r->quantity : r->function == 5 ? (r->value ? 0xff00 : 0) : r->value);
    if (r->tcp) return g_bytes_new(frame, 12);
    guint16 crc = crc16(frame, 6); frame[6] = (guint8)crc; frame[7] = (guint8)(crc >> 8);
    return g_bytes_new(frame, 8);
}
gchar *tio_modbus_decode(const TioModbusRequest *r, GBytes *response, GError **error)
{
    g_autoptr(GBytes) request = tio_modbus_frame(r, error); if (!request) return NULL;
    gsize length; const guint8 *frame = g_bytes_get_data(response, &length);
    guint offset = r->tcp ? 6 : 0; gsize end = length;
    if (r->tcp) {
        if (length < 9 || length > 260 || word(frame) != r->transaction || word(frame + 2) || word(frame + 4) != length - 6) goto invalid;
    } else {
        if (length < 5 || length > 256 || crc16(frame, length) != 0) goto invalid;
        end -= 2;
    }
    if (frame[offset] != r->unit) goto invalid;
    if (frame[offset + 1] == (r->function | 0x80)) {
        if (end != offset + 3) goto invalid;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Modbus exception 0x%02X (function 0x%02X)", frame[offset + 2], r->function); return NULL;
    }
    if (frame[offset + 1] != r->function) goto invalid;
    if (r->function >= 5) {
        const guint8 *sent = g_bytes_get_data(request, NULL);
        if (end != offset + 6 || memcmp(sent + offset, frame + offset, 6)) goto invalid;
        return g_strdup_printf("Write acknowledged: address %u = %u (0x%04X)", r->address, r->value, r->value);
    }
    guint count = r->function <= 2 ? (r->quantity + 7u) / 8u : r->quantity * 2u;
    if (end != offset + 3 + count || frame[offset + 2] != count) goto invalid;
    GString *text = g_string_new(NULL);
    for (guint i = 0; i < r->quantity; ++i) {
        guint16 value = r->function <= 2 ? (guint16)((frame[offset + 3 + i / 8] >> (i % 8)) & 1) : word(frame + offset + 3 + i * 2);
        g_string_append_printf(text, "%u: %u (0x%04X)", r->address + i, value, value);
        if (r->function >= 3) g_string_append_printf(text, " signed=%d", (gint16)value);
        g_string_append_c(text, '\n');
    }
    return g_string_free(text, FALSE);
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Modbus response failed CRC, transaction, unit, function, length or echo validation"); return NULL;
}
typedef struct { TioModbusRequest request; gchar *endpoint; } Job;
static void job_free(gpointer data) { Job *job = data; g_free(job->endpoint); g_free(job); }
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; Job *job = data; TioModbusRequest *r = &job->request;
    g_autoptr(GError) error = NULL; g_autoptr(GBytes) frame = tio_modbus_frame(r, &error);
    if (!frame) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    g_autoptr(GSocketClient) client = g_socket_client_new(); g_socket_client_set_enable_proxy(client, FALSE); g_socket_client_set_timeout(client, 2);
    g_autoptr(GSocketConnectable) address = r->tcp ? g_network_address_new(r->endpoint, r->port)
        : G_SOCKET_CONNECTABLE(g_unix_socket_address_new(r->endpoint));
    g_autoptr(GSocketConnection) connection = g_socket_client_connect(client, address, cancel, &error);
    if (!connection) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    gsize length; const guint8 *bytes = g_bytes_get_data(frame, &length);
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    if (!g_output_stream_write_all(output, bytes, length, NULL, cancel, &error)) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    guint8 response[260]; gsize received = 0, needed = r->tcp ? 6 : 3;
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (received < needed) {
        if (g_get_monotonic_time() > deadline) { g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Modbus response deadline exceeded"); break; }
        gssize count = g_input_stream_read(input, response + received, needed - received, cancel, &error);
        if (count <= 0) { if (!error) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "Modbus peer closed before a full response"); break; }
        received += (gsize)count;
        if (received == needed) {
            if (r->tcp && needed == 6) {
                guint rest = word(response + 4);
                if (rest < 3 || rest > 254) { g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid Modbus TCP length"); break; }
                needed = 6 + rest;
            } else if (!r->tcp && needed == 3) {
                needed = response[1] & 0x80 ? 5 : r->function <= 4 ? (gsize)response[2] + 5 : 8;
                if (needed > 256) { g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid Modbus RTU length"); break; }
            }
        }
    }
    if (error) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    g_autoptr(GBytes) reply = g_bytes_new(response, received);
    gchar *result = tio_modbus_decode(r, reply, &error);
    if (!result) g_task_return_error(task, g_steal_pointer(&error)); else g_task_return_pointer(task, result, g_free);
}
void tio_modbus_request_async(const TioModbusRequest *request, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
    g_autoptr(GTask) task = g_task_new(NULL, cancel, callback, data);
    if (!request || !request->endpoint || !*request->endpoint || (request->tcp && !request->port)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Select a connected RTU session or a TCP host and port"); return;
    }
    Job *job = g_new0(Job, 1); job->request = *request; job->endpoint = g_strdup(request->endpoint); job->request.endpoint = job->endpoint;
    g_task_set_task_data(task, job, job_free); g_task_run_in_thread(task, worker);
}
gchar *tio_modbus_request_finish(GAsyncResult *result, GError **error) { return g_task_propagate_pointer(G_TASK(result), error); }
