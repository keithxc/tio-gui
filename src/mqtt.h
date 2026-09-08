/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "network.h"
typedef struct _TioMqtt TioMqtt;
typedef enum { TIO_MQTT_STATUS, TIO_MQTT_MESSAGE } TioMqttEvent;
typedef void (*TioMqttCallback)(TioMqttEvent event, const char *topic, const guint8 *payload,
                              gsize length, guint qos, gboolean retained, gpointer data);
TioMqtt *tio_mqtt_connect(const char *host, guint16 port, const char *username, const char *password,
                         TioMqttCallback callback, gpointer data, GError **error);
gboolean tio_mqtt_subscribe(TioMqtt *mqtt, const char *topic, guint qos, gboolean unsubscribe, GError **error);
gboolean tio_mqtt_publish(TioMqtt *mqtt, const char *topic, GBytes *payload, guint qos, gboolean retain, GError **error);
gboolean tio_mqtt_ready(const TioMqtt *mqtt);
void tio_mqtt_free(TioMqtt *mqtt);
