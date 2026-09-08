/* SPDX-License-Identifier: GPL-3.0-only */
#include "mqtt.h"
#include <string.h>
struct _TioMqtt {
    guint refs, timer;
    gboolean ready, closed, connect_sent;
    TioNetwork *network;
    GByteArray *received, *hello;
    GHashTable *pending;
    guint16 packet_id;
    gint64 started, last_rx, last_tx;
    TioMqttCallback callback;
    gpointer data;
};
static TioMqtt *ref(TioMqtt *mqtt) { ++mqtt->refs; return mqtt; }
static void unref(gpointer data)
{
    TioMqtt *mqtt = data; if (--mqtt->refs) return;
    g_byte_array_unref(mqtt->received); g_byte_array_unref(mqtt->hello); g_hash_table_unref(mqtt->pending); g_free(mqtt);
}
static void status(TioMqtt *mqtt, const char *message)
{
    if (mqtt->callback) mqtt->callback(TIO_MQTT_STATUS, "", (const guint8 *)message, strlen(message), 0, FALSE, mqtt->data);
}
static void close_mqtt(TioMqtt *mqtt, const char *reason)
{
    if (mqtt->closed) return;
    mqtt->closed = TRUE; mqtt->ready = FALSE;
    g_clear_pointer(&mqtt->network, tio_network_free);
    if (mqtt->timer) { g_source_remove(mqtt->timer); mqtt->timer = 0; }
    status(mqtt, reason);
}
void tio_mqtt_free(TioMqtt *mqtt)
{
    if (!mqtt) return;
    mqtt->callback = NULL; close_mqtt(mqtt, "Disconnected"); unref(mqtt);
}
static void byte(GByteArray *array, guint value) { guint8 b = (guint8)value; g_byte_array_append(array, &b, 1); }
static void word(GByteArray *array, guint value) { byte(array, value >> 8); byte(array, value); }
static void string(GByteArray *array, const char *text) { gsize length = strlen(text); word(array, (guint)length); g_byte_array_append(array, (const guint8 *)text, (guint)length); }
static gboolean utf8(const char *text, gsize length)
{
    if (memchr(text, 0, length) || !g_utf8_validate(text, (gssize)length, NULL)) return FALSE;
    for (const char *p = text; p < text + length; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if ((c >= 0xfdd0 && c <= 0xfdef) || (c & 0xffff) >= 0xfffe) return FALSE;
    }
    return TRUE;
}
static gboolean topic_valid(const char *topic, gboolean filter)
{
    if (!topic || !*topic || strlen(topic) > 1024 || !utf8(topic, strlen(topic))) return FALSE;
    for (const char *p = topic; *p; ++p) {
        if (*p == '#') { if (!filter || p[1] || (p != topic && p[-1] != '/')) return FALSE; }
        if (*p == '+') { if (!filter || (p[1] && p[1] != '/') || (p != topic && p[-1] != '/')) return FALSE; }
    }
    return TRUE;
}
static gboolean send_packet(TioMqtt *mqtt, guint header, const guint8 *body, gsize length, GError **error)
{
    g_autoptr(GByteArray) packet = g_byte_array_new(); byte(packet, header);
    gsize remaining = length;
    do { guint digit = (guint)(remaining % 128); remaining /= 128; byte(packet, digit | (remaining ? 128u : 0u)); } while (remaining);
    if (length) g_byte_array_append(packet, body, (guint)length);
    g_autoptr(GBytes) bytes = g_bytes_new(packet->data, packet->len);
    if (!tio_network_send(mqtt->network, bytes, error)) return FALSE;
    mqtt->last_tx = g_get_monotonic_time(); return TRUE;
}
static guint16 next_id(TioMqtt *mqtt)
{
    do { ++mqtt->packet_id; } while (!mqtt->packet_id || g_hash_table_contains(mqtt->pending, GUINT_TO_POINTER(mqtt->packet_id)));
    return mqtt->packet_id;
}
static gboolean packet(TioMqtt *mqtt, guint8 header, const guint8 *body, gsize length)
{
    guint type = header >> 4;
    if (type != 3 && (header & 15)) return FALSE;
    if (type == 2) {
        if (mqtt->ready || length != 2 || body[0] != 0 || body[1] > 5) return FALSE;
        if (body[1]) { g_autofree gchar *message = g_strdup_printf("MQTT connection refused (code %u)", body[1]); close_mqtt(mqtt, message); return TRUE; }
        mqtt->ready = TRUE; status(mqtt, "MQTT connected (clean session)"); return TRUE;
    }
    if (!mqtt->ready) return FALSE;
    if (type == 13) return length == 0;
    if (type == 4 || type == 9 || type == 11) {
        if (length != (type == 9 ? 3u : 2u)) return FALSE;
        guint id = (guint)(body[0] << 8) | body[1];
        guint expected = GPOINTER_TO_UINT(g_hash_table_lookup(mqtt->pending, GUINT_TO_POINTER(id)));
        if (!id || expected != type) return FALSE;
        g_hash_table_remove(mqtt->pending, GUINT_TO_POINTER(id));
        if (type == 9 && body[2] > 1 && body[2] != 128) return FALSE;
        status(mqtt, type == 4 ? "Publish acknowledged (QoS 1)" : type == 11 ? "Unsubscribed" : body[2] == 128 ? "Subscription refused" : "Subscribed");
        return TRUE;
    }
    if (type != 3 || length < 2) return FALSE;
    guint qos = (header >> 1) & 3; if (qos > 1 || (!qos && (header & 8))) return FALSE;
    guint topic_length = (guint)(body[0] << 8) | body[1];
    gsize offset = 2u + topic_length;
    if (!topic_length || topic_length > 1024 || offset + (qos ? 2u : 0u) > length || !utf8((const char *)body + 2, topic_length)) return FALSE;
    g_autofree gchar *topic = g_strndup((const char *)body + 2, topic_length); if (!topic_valid(topic, FALSE)) return FALSE;
    if (qos) {
        if (!body[offset] && !body[offset + 1]) return FALSE;
        if (!send_packet(mqtt, 0x40, body + offset, 2, NULL)) { close_mqtt(mqtt, "MQTT acknowledgement queue full"); return TRUE; }
        offset += 2;
    }
    if (mqtt->callback) mqtt->callback(TIO_MQTT_MESSAGE, topic, body + offset, length - offset, qos, header & 1, mqtt->data);
    return TRUE;
}
static void parse(TioMqtt *mqtt)
{
    guint budget = 64;
    while (!mqtt->closed && mqtt->received->len >= 2 && budget--) {
        guint8 *bytes = mqtt->received->data; guint offset = 1, multiplier = 1, length = 0; gboolean complete = FALSE;
        while (offset < mqtt->received->len && offset <= 4) {
            guint digit = bytes[offset++]; length += (digit & 127) * multiplier;
            if (!(digit & 128)) { complete = TRUE; break; } multiplier *= 128;
        }
        if (length > 65530 || (!complete && offset > 4)) { close_mqtt(mqtt, "MQTT packet exceeds 64 KiB or has invalid length"); return; }
        if (!complete || mqtt->received->len < offset + length) return;
        if (!packet(mqtt, bytes[0], bytes + offset, length)) { close_mqtt(mqtt, "Malformed or unsupported MQTT response"); return; }
        g_byte_array_remove_range(mqtt->received, 0, offset + length);
    }
}
static void network_event(TioNetworkEvent event, const guint8 *bytes, gsize length, gpointer data)
{
    TioMqtt *mqtt = ref(data);
    if (event == TIO_NET_STATUS) {
        if (tio_network_ready(mqtt->network) && !mqtt->connect_sent) {
            mqtt->connect_sent = TRUE;
            if (!send_packet(mqtt, 0x10, mqtt->hello->data, mqtt->hello->len, NULL)) close_mqtt(mqtt, "Could not send MQTT CONNECT");
            /* Credentials are no longer needed after CONNECT is queued. */
            memset(mqtt->hello->data, 0, mqtt->hello->len); g_byte_array_set_size(mqtt->hello, 0);
        } else if (!tio_network_ready(mqtt->network)) {
            g_autofree gchar *message = g_strndup((const char *)bytes, length); close_mqtt(mqtt, message);
        }
    } else if (event == TIO_NET_RX) {
        mqtt->last_rx = g_get_monotonic_time();
        if (mqtt->received->len + length > 131072) close_mqtt(mqtt, "MQTT receive queue exceeded 128 KiB");
        else { g_byte_array_append(mqtt->received, bytes, (guint)length); parse(mqtt); }
    }
    unref(mqtt);
}
static gboolean tick(gpointer data)
{
    TioMqtt *mqtt = ref(data); gint64 now = g_get_monotonic_time();
    if ((!mqtt->ready && now - mqtt->started > 5 * G_TIME_SPAN_SECOND) || (mqtt->ready && now - mqtt->last_rx > 45 * G_TIME_SPAN_SECOND))
        close_mqtt(mqtt, "MQTT handshake or keepalive timed out");
    else if (mqtt->ready && now - mqtt->last_tx > 15 * G_TIME_SPAN_SECOND)
        if (!send_packet(mqtt, 0xc0, NULL, 0, NULL)) close_mqtt(mqtt, "Could not send MQTT keepalive");
    if (!mqtt->closed) parse(mqtt);
    gboolean keep = !mqtt->closed; unref(mqtt); return keep;
}
TioMqtt *tio_mqtt_connect(const char *host, guint16 port, const char *username, const char *password,
                         TioMqttCallback callback, gpointer data, GError **error)
{
    if (!username) username = "";
    if (!password) password = "";
    if (strlen(username) > 1024 || strlen(password) > 1024 || !utf8(username, strlen(username)) || (*password && !*username)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "MQTT username/password invalid or exceed 1024 bytes"); return NULL;
    }
    TioMqtt *mqtt = g_new0(TioMqtt, 1); mqtt->refs = 1; mqtt->callback = callback; mqtt->data = data;
    mqtt->received = g_byte_array_new(); mqtt->hello = g_byte_array_new(); mqtt->pending = g_hash_table_new(g_direct_hash, g_direct_equal);
    mqtt->started = mqtt->last_rx = mqtt->last_tx = g_get_monotonic_time();
    string(mqtt->hello, "MQTT"); byte(mqtt->hello, 4); byte(mqtt->hello, 2 | (*username ? 128 : 0) | (*password ? 64 : 0)); word(mqtt->hello, 30);
    g_autofree gchar *uuid = g_uuid_string_random(); g_autofree gchar *id = g_strdup_printf("tiogui-%.16s", uuid);
    string(mqtt->hello, id); if (*username) string(mqtt->hello, username); if (*password) string(mqtt->hello, password);
    mqtt->network = tio_network_connect(host, port, FALSE, network_event, mqtt, error);
    if (!mqtt->network) { tio_mqtt_free(mqtt); return NULL; }
    mqtt->timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, tick, ref(mqtt), unref); return mqtt;
}
gboolean tio_mqtt_ready(const TioMqtt *mqtt) { return mqtt && mqtt->ready && !mqtt->closed; }
static gboolean check_ready(TioMqtt *mqtt, const char *topic, guint qos, gboolean filter, GError **error)
{
    if (!tio_mqtt_ready(mqtt) || qos > 1 || !topic_valid(topic, filter) || g_hash_table_size(mqtt->pending) >= 64) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "MQTT not ready, invalid topic/QoS or 64 acknowledgements pending"); return FALSE;
    }
    return TRUE;
}
gboolean tio_mqtt_subscribe(TioMqtt *mqtt, const char *topic, guint qos, gboolean unsubscribe, GError **error)
{
    if (!check_ready(mqtt, topic, qos, TRUE, error)) return FALSE;
    guint16 id = next_id(mqtt); g_autoptr(GByteArray) body = g_byte_array_new(); word(body, id); string(body, topic); if (!unsubscribe) byte(body, qos);
    if (!send_packet(mqtt, unsubscribe ? 0xa2 : 0x82, body->data, body->len, error)) return FALSE;
    g_hash_table_insert(mqtt->pending, GUINT_TO_POINTER(id), GUINT_TO_POINTER(unsubscribe ? 11 : 9)); return TRUE;
}
gboolean tio_mqtt_publish(TioMqtt *mqtt, const char *topic, GBytes *payload, guint qos, gboolean retain, GError **error)
{
    if (!check_ready(mqtt, topic, qos, FALSE, error)) return FALSE;
    gsize length; const guint8 *bytes = g_bytes_get_data(payload, &length);
    if (length + strlen(topic) + 8 > 65530) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "MQTT packet exceeds 64 KiB"); return FALSE; }
    g_autoptr(GByteArray) body = g_byte_array_new(); string(body, topic); guint16 id = qos ? next_id(mqtt) : 0;
    if (qos) word(body, id);
    if (length) g_byte_array_append(body, bytes, (guint)length);
    if (!send_packet(mqtt, 0x30 | (qos << 1) | (retain ? 1 : 0), body->data, body->len, error)) return FALSE;
    if (qos) g_hash_table_insert(mqtt->pending, GUINT_TO_POINTER(id), GUINT_TO_POINTER(4));
    return TRUE;
}
