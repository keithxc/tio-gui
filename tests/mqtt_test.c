/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/mqtt.c"
#include <stdlib.h>
typedef struct { gboolean subscribed, ack, message, retained; } Sink;
static void event(TioMqttEvent type, const char *topic, const guint8 *bytes, gsize length, guint qos, gboolean retained, gpointer data)
{
    Sink *sink = data;
    if (type == TIO_MQTT_STATUS) {
        if (g_str_has_prefix((const char *)bytes, "Subscribed")) sink->subscribed = TRUE;
        if (g_str_has_prefix((const char *)bytes, "Publish acknowledged")) sink->ack = TRUE;
        g_print("%.*s\n", (int)length, bytes);
    } else {
        g_assert_cmpstr(topic, ==, "tio/test/bytes"); g_assert_cmpuint(length, ==, 256);
        for (guint i = 0; i < 256; ++i) g_assert_cmpuint(bytes[i], ==, i);
        g_assert_cmpuint(qos, ==, 1); sink->message = TRUE; sink->retained |= retained;
    }
}
static void pump_until(gboolean *flag)
{
    gint64 until = g_get_monotonic_time() + 5000000;
    while (!*flag && g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    g_assert_true(*flag);
}
static void malformed(void)
{
    const guint8 samples[][6]={{0x30,0xff,0xff,0xff,0x7f,0},{0x20,2,1,0,0,0},{0x30,2,0,0,0,0}};
    for(guint i=0;i<G_N_ELEMENTS(samples);++i){
        TioMqtt *mqtt=g_new0(TioMqtt,1);mqtt->refs=1;mqtt->received=g_byte_array_new();mqtt->hello=g_byte_array_new();mqtt->pending=g_hash_table_new(g_direct_hash,g_direct_equal);
        g_byte_array_append(mqtt->received,samples[i],sizeof samples[i]);parse(mqtt);g_assert_true(mqtt->closed);tio_mqtt_free(mqtt);
    }
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    malformed();
    g_assert_true(topic_valid("board/+/log/#", TRUE));
    g_assert_false(topic_valid("board/#/bad", TRUE)); g_assert_false(topic_valid("board/+", FALSE));
    Sink sink = {0}; TioMqtt *mqtt = tio_mqtt_connect("127.0.0.1", (guint16)atoi(argv[1]), "", "", event, &sink, NULL);
    g_assert_nonnull(mqtt); pump_until(&mqtt->ready);
    g_assert_true(tio_mqtt_subscribe(mqtt, "tio/test/#", 1, FALSE, NULL)); pump_until(&sink.subscribed);
    guint8 values[256]; for (guint i = 0; i < 256; ++i) values[i] = (guint8)i;
    g_autoptr(GBytes) payload = g_bytes_new(values, sizeof values);
    g_assert_true(tio_mqtt_publish(mqtt, "tio/test/bytes", payload, 1, TRUE, NULL));
    pump_until(&sink.message); pump_until(&sink.ack);
    sink.subscribed = sink.message = FALSE;
    g_assert_true(tio_mqtt_subscribe(mqtt, "tio/test/#", 1, TRUE, NULL));
    g_assert_true(tio_mqtt_subscribe(mqtt, "tio/test/#", 1, FALSE, NULL));
    pump_until(&sink.subscribed); pump_until(&sink.retained);
    tio_mqtt_free(mqtt);
    mqtt = tio_mqtt_connect("localhost", (guint16)atoi(argv[1]), "", "", event, NULL, NULL);
    tio_mqtt_free(mqtt);
    gint64 until = g_get_monotonic_time() + 100000;
    while (g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    return 0;
}
