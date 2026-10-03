/* SPDX-License-Identifier: GPL-3.0-only */
/* Private stdin/stdout protocol used by the authenticated remote gateway.
 * This process does not listen on a network socket or execute commands. */
#include "native_serial.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#ifdef G_OS_WIN32
#include <io.h>
#include <fcntl.h>
#endif

#define REQUEST_LIMIT (128 * 1024)
#define DATA_LIMIT (64 * 1024)
typedef struct { GString *line; gboolean oversized, eof; } Request;
static GAsyncQueue *requests;
static TioNativeSerial *serial;

static gpointer read_requests(gpointer unused)
{
    (void)unused;
    for (;;) {
        Request *r = g_new0(Request, 1);
        r->line = g_string_sized_new(256);
        int c;
        while ((c = getchar()) != EOF && c != '\n') {
            if (r->line->len < REQUEST_LIMIT) g_string_append_c(r->line, (char)c);
            else r->oversized = TRUE;
        }
        r->eof = c == EOF;
        gboolean eof = r->eof;
        while (g_async_queue_length(requests) >= 16) g_usleep(2000);
        g_async_queue_push(requests, r);
        if (eof) return NULL;
    }
}

static void output(JsonObject *object)
{
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, object);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, root);
    g_autofree char *text = json_generator_to_data(generator, NULL);
    if (puts(text) == EOF || fflush(stdout) == EOF) exit(1);
}

static const char *string_member(JsonObject *o, const char *name)
{
    JsonNode *n = json_object_get_member(o, name);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
        ? json_node_get_string(n) : NULL;
}

static gboolean uint_member(JsonObject *o, const char *name, guint fallback,
                            guint minimum, guint maximum, guint *result)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n) { *result = fallback; return TRUE; }
    if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return FALSE;
    gint64 v = json_node_get_int(n);
    if (v < minimum || v > maximum) return FALSE;
    *result = (guint)v;
    return TRUE;
}

static gboolean bool_member(JsonObject *o, const char *name, gboolean fallback, gboolean *result)
{
    JsonNode *n = json_object_get_member(o, name);
    if (!n) { *result = fallback; return TRUE; }
    if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) return FALSE;
    *result = json_node_get_boolean(n);
    return TRUE;
}

static gboolean drain_events(guint limit)
{
    for (guint i = 0; serial && i < limit; ++i) {
        TioNativeEvent *e = tio_native_poll(serial);
        if (!e) return FALSE;
        JsonObject *o = json_object_new();
        const char *name = e->kind == TIO_NATIVE_STATUS ? "status" :
            e->kind == TIO_NATIVE_RX ? "rx" : e->kind == TIO_NATIVE_TX ? "tx" : "done";
        json_object_set_string_member(o, "event", name);
        if (e->kind == TIO_NATIVE_STATUS) {
            json_object_set_boolean_member(o, "connected", e->connected);
            json_object_set_string_member(o, "message", e->message ? e->message : "");
            json_object_set_string_member(o, "device", e->device ? e->device : "");
        }
        if (e->bytes) {
            gsize length;
            const guint8 *bytes = g_bytes_get_data(e->bytes, &length);
            g_autofree char *encoded = g_base64_encode(bytes, length);
            json_object_set_string_member(o, "data", encoded);
        }
        gboolean done = e->kind == TIO_NATIVE_DONE;
        tio_native_event_free(e);
        output(o);
        if (done) { tio_native_stop(serial); serial = NULL; return FALSE; }
    }
    return serial != NULL;
}

static void close_serial(void)
{
    if (!serial) return;
    tio_native_finish(serial);
    while (drain_events(64)) {}
    if (serial) { tio_native_stop(serial); serial = NULL; }
}

/* JSON-GLib exposes strings as NUL-terminated C strings. Reject escaped NULs
 * instead of accepting a truncated operation, request id, or device path. */
static gboolean contains_nul(const GString *line)
{
    if (memchr(line->str, '\0', line->len)) return TRUE;
    for (gsize i = 0; i < line->len; ++i) {
        if (line->str[i] != '\\') continue;
        ++i;
        if (i + 4 < line->len && line->str[i] == 'u' &&
            memcmp(line->str + i + 1, "0000", 4) == 0) return TRUE;
    }
    return FALSE;
}

static void process(Request *r)
{
    JsonObject *reply = json_object_new();
    json_object_set_null_member(reply, "id");
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    const char *failure = NULL;
    if (r->oversized) { failure = "Request exceeds 128 KiB"; goto respond; }
    if (contains_nul(r->line) ||
        !json_parser_load_from_data(parser, r->line->str, (gssize)r->line->len, NULL) ||
        !json_parser_get_root(parser) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        failure = "Expected a JSON object"; goto respond;
    }
    JsonObject *request = json_node_get_object(json_parser_get_root(parser));
    const char *id = string_member(request, "id");
    const char *op = string_member(request, "op");
    if (!id || !*id || strlen(id) > 128 || !op) {
        failure = "Expected a string id (1-128 bytes) and op"; goto respond;
    }
    json_object_set_string_member(reply, "id", id);
    if (strcmp(op, "devices") == 0) {
        g_auto(GStrv) devices = tio_native_devices();
        JsonArray *list = json_array_new();
        for (guint i = 0; devices[i]; ++i) json_array_add_string_element(list, devices[i]);
        json_object_set_array_member(reply, "devices", list);
    } else if (strcmp(op, "open") == 0) {
        if (serial) { failure = "Close the existing session first"; goto respond; }
        TioNativeConfig c = {0};
        c.device = string_member(request, "device");
        if (!c.device || !*c.device || strlen(c.device) > 4096 ||
            !uint_member(request, "baud", 115200, 1, 12000000, &c.baud) ||
            !uint_member(request, "bits", 8, 5, 8, &c.bits) ||
            !uint_member(request, "stops", 1, 1, 2, &c.stops) ||
            !uint_member(request, "parity", 0, 0, 2, &c.parity) ||
            !uint_member(request, "flow", 0, 0, 2, &c.flow) ||
            !bool_member(request, "reconnect", FALSE, &c.reconnect)) {
            failure = "Invalid serial configuration"; goto respond;
        }
        serial = tio_native_start(&c, &error);
    } else if (strcmp(op, "send") == 0) {
        const char *encoded = string_member(request, "data");
        if (!serial) { failure = "No serial session"; goto respond; }
        if (!encoded || !*encoded || strlen(encoded) > ((DATA_LIMIT + 2) / 3) * 4) {
            failure = "Expected base64 data (1-65536 decoded bytes)"; goto respond;
        }
        gsize length;
        g_autofree guint8 *bytes = g_base64_decode(encoded, &length);
        g_autofree char *canonical = g_base64_encode(bytes, length);
        if (length == 0 || length > DATA_LIMIT || strcmp(encoded, canonical) != 0) {
            failure = "Invalid canonical base64 data"; goto respond;
        }
        g_autoptr(GBytes) data = g_bytes_new(bytes, length);
        tio_native_send(serial, data, &error);
    } else if (strcmp(op, "line") == 0) {
        guint line;
        gboolean high;
        if (!serial) { failure = "No serial session"; goto respond; }
        if (!uint_member(request, "line", 3, 0, 2, &line) || line > 2 ||
            !bool_member(request, "high", FALSE, &high)) {
            failure = "Invalid serial line control"; goto respond;
        }
        tio_native_line(serial, line, high, &error);
    } else if (strcmp(op, "close") == 0) {
        close_serial();
    } else {
        failure = "Unknown operation";
    }
respond:
    if (error) failure = error->message;
    json_object_set_boolean_member(reply, "ok", failure == NULL);
    if (failure) json_object_set_string_member(reply, "error", failure);
    output(reply);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("tio-serial-agent %s\n", TIO_GUI_VERSION); return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: tio-serial-agent [--version]\nJSON lines on stdin/stdout; no network listener.\n");
        return 2;
    }
#ifdef G_OS_WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    requests = g_async_queue_new();
    GThread *reader = g_thread_new("agent-input", read_requests, NULL);
    gboolean eof = FALSE;
    while (!eof) {
        for (guint i = 0; i < 8; ++i) {
            Request *r = g_async_queue_try_pop(requests);
            if (!r) break;
            eof = r->eof;
            if (r->line->len || r->oversized) process(r);
            g_string_free(r->line, TRUE); g_free(r);
            if (eof) break;
        }
        drain_events(64);
        if (!eof) g_usleep(2000);
    }
    close_serial();
    g_thread_join(reader);
    g_async_queue_unref(requests);
    return 0;
}
