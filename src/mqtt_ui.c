/* SPDX-License-Identifier: GPL-3.0-only */
#include "mqtt_ui.h"
#include "mqtt.h"
#include "payload.h"
#include "log_model.h"
#include "analyzer.h"
#include <libintl.h>
#include <string.h>
#define _(s) gettext(s)
typedef struct {
    GtkWidget *window, *analyzer;
    GtkEntry *host, *username, *subscription, *topic, *payload;
    GtkEditable *password;
    GtkSpinButton *port;
    GtkDropDown *qos, *mode;
    GtkCheckButton *retain;
    GtkTextView *output;
    GtkLabel *status, *preview;
    TioMqtt *mqtt;
    TioLogModel *model;
    guint messages;
} MqttView;
static void append(MqttView *view, const char *text)
{
    GtkTextBuffer *buffer=gtk_text_view_get_buffer(view->output);GtkTextIter end;gtk_text_buffer_get_end_iter(buffer,&end);
    gtk_text_buffer_insert(buffer,&end,text,-1);gtk_text_buffer_insert(buffer,&end,"\n",1);
    if(gtk_text_buffer_get_line_count(buffer)>1000){GtkTextIter begin,keep;gtk_text_buffer_get_start_iter(buffer,&begin);gtk_text_buffer_get_iter_at_line(buffer,&keep,250);gtk_text_buffer_delete(buffer,&begin,&keep);}
}
static void event(TioMqttEvent type,const char *topic,const guint8 *bytes,gsize length,guint qos,gboolean retained,gpointer data)
{
    MqttView *view=data;
    if(type==TIO_MQTT_STATUS){g_autofree gchar *text=g_strndup((const char *)bytes,length);gtk_label_set_text(view->status,text);append(view,text);return;}
    ++view->messages;
    g_autoptr(GByteArray) preview=g_byte_array_new();g_byte_array_append(preview,bytes,(guint)MIN(length,256u));
    g_autofree gchar *hex=tio_payload_preview(preview);
    g_autofree gchar *line=g_strdup_printf("%s · QoS %u · retained=%s · %zu bytes\n%s",topic,qos,retained?"true":"false",length,hex);
    append(view,line);
    tio_log_model_feed(view->model,bytes,length,g_get_real_time());tio_log_model_feed(view->model,(const guint8 *)"\n",1,g_get_real_time());
}
static void connect_clicked(GtkButton *button,gpointer data)
{
    (void)button;MqttView *view=data;g_clear_pointer(&view->mqtt,tio_mqtt_free);g_autoptr(GError) error=NULL;
    view->mqtt=tio_mqtt_connect(gtk_editable_get_text(GTK_EDITABLE(view->host)),(guint16)gtk_spin_button_get_value_as_int(view->port),
        gtk_editable_get_text(GTK_EDITABLE(view->username)),gtk_editable_get_text(view->password),event,view,&error);
    gtk_label_set_text(view->status,view->mqtt?_("Connecting…"):error->message);
}
static void disconnect_clicked(GtkButton *button,gpointer data)
{
    (void)button;MqttView *view=data;g_clear_pointer(&view->mqtt,tio_mqtt_free);gtk_label_set_text(view->status,_("Disconnected"));
}
static void subscribe_clicked(GtkButton *button,gpointer data)
{
    MqttView *view=data;g_autoptr(GError) error=NULL;
    if(!tio_mqtt_subscribe(view->mqtt,gtk_editable_get_text(GTK_EDITABLE(view->subscription)),gtk_drop_down_get_selected(view->qos),
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),"unsubscribe")),&error))gtk_label_set_text(view->status,error->message);
}
static void publish_clicked(GtkWidget *widget,gpointer data)
{
    (void)widget;MqttView *view=data;g_autoptr(GError) error=NULL;
    g_autoptr(GByteArray) bytes=tio_payload_build(gtk_editable_get_text(GTK_EDITABLE(view->payload)),gtk_drop_down_get_selected(view->mode)==1,0,0,&error);
    if(!bytes){gtk_label_set_text(view->status,error->message);return;}
    g_autoptr(GBytes) payload=g_bytes_new(bytes->data,bytes->len);
    if(!tio_mqtt_publish(view->mqtt,gtk_editable_get_text(GTK_EDITABLE(view->topic)),payload,gtk_drop_down_get_selected(view->qos),gtk_check_button_get_active(view->retain),&error))gtk_label_set_text(view->status,error->message);
    else gtk_label_set_text(view->status,_("Publish queued; QoS 1 waits for broker acknowledgement"));
}
static void preview_changed(GtkWidget *widget,gpointer data)
{
    (void)widget;MqttView *view=data;g_autoptr(GError) error=NULL;
    g_autoptr(GByteArray) bytes=tio_payload_build(gtk_editable_get_text(GTK_EDITABLE(view->payload)),gtk_drop_down_get_selected(view->mode)==1,0,0,&error);
    g_autofree gchar *text=bytes?tio_payload_preview(bytes):g_strdup(error->message);gtk_label_set_text(view->preview,text);
}
static void preview_notify(GObject *object,GParamSpec *spec,gpointer data){(void)object;(void)spec;preview_changed(NULL,data);}
static void analyze_clicked(GtkButton *button,gpointer data)
{
    (void)button;MqttView *view=data;
    if(view->analyzer){gtk_window_present(GTK_WINDOW(view->analyzer));return;}
    view->analyzer=tio_analyzer_new(GTK_WINDOW(view->window),view->model);g_object_add_weak_pointer(G_OBJECT(view->analyzer),(gpointer *)&view->analyzer);
}
static void free_view(gpointer data)
{
    MqttView *view=data;if(view->analyzer)gtk_window_destroy(GTK_WINDOW(view->analyzer));tio_mqtt_free(view->mqtt);tio_log_model_free(view->model);g_free(view);
}
static GtkEntry *entry(GtkWidget *row,const char *placeholder)
{
    GtkEntry *field=GTK_ENTRY(gtk_entry_new());gtk_entry_set_placeholder_text(field,placeholder);gtk_widget_set_hexpand(GTK_WIDGET(field),TRUE);gtk_box_append(GTK_BOX(row),GTK_WIDGET(field));return field;
}
GtkWidget *tio_mqtt_window(GtkWindow *parent)
{
    MqttView *view=g_new0(MqttView,1);view->model=tio_log_model_new();view->window=gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(view->window),_("MQTT 3.1.1 / TCP debugging"));gtk_window_set_transient_for(GTK_WINDOW(view->window),parent);gtk_window_set_destroy_with_parent(GTK_WINDOW(view->window),TRUE);gtk_window_set_default_size(GTK_WINDOW(view->window),1000,680);
    g_object_set_data_full(G_OBJECT(view->window),"mqtt-view",view,free_view);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_window_set_child(GTK_WINDOW(view->window),box);
    gtk_widget_set_margin_start(box,12);gtk_widget_set_margin_end(box,12);gtk_widget_set_margin_top(box,12);gtk_widget_set_margin_bottom(box,12);
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);view->host=entry(row,_("Broker hostname or IP"));
    view->port=GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1,65535,1));gtk_spin_button_set_value(view->port,1883);gtk_box_append(GTK_BOX(row),GTK_WIDGET(view->port));
    GtkWidget *connect=gtk_button_new_with_label(_("Connect")),*disconnect=gtk_button_new_with_label(_("Disconnect")),*analyze=gtk_button_new_with_label(_("Analyze payloads…"));
    g_signal_connect(connect,"clicked",G_CALLBACK(connect_clicked),view);g_signal_connect(disconnect,"clicked",G_CALLBACK(disconnect_clicked),view);g_signal_connect(analyze,"clicked",G_CALLBACK(analyze_clicked),view);
    gtk_box_append(GTK_BOX(row),connect);gtk_box_append(GTK_BOX(row),disconnect);gtk_box_append(GTK_BOX(row),analyze);gtk_box_append(GTK_BOX(box),row);
    row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);view->username=entry(row,_("Optional username"));
    GtkWidget *password=gtk_password_entry_new();view->password=GTK_EDITABLE(password);gtk_widget_set_hexpand(password,TRUE);gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(password),TRUE);gtk_box_append(GTK_BOX(row),gtk_label_new(_("Password")));gtk_box_append(GTK_BOX(row),password);gtk_box_append(GTK_BOX(box),row);
    row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);view->subscription=entry(row,_("Subscription filter, e.g. board/+/log"));
    const char *qos[]={"QoS 0","QoS 1",NULL};view->qos=GTK_DROP_DOWN(gtk_drop_down_new_from_strings(qos));gtk_drop_down_set_selected(view->qos,1);gtk_box_append(GTK_BOX(row),GTK_WIDGET(view->qos));
    for(guint i=0;i<2;++i){GtkWidget *button=gtk_button_new_with_label(i?_("Unsubscribe"):_("Subscribe"));g_object_set_data(G_OBJECT(button),"unsubscribe",GINT_TO_POINTER(i));g_signal_connect(button,"clicked",G_CALLBACK(subscribe_clicked),view);gtk_box_append(GTK_BOX(row),button);}gtk_box_append(GTK_BOX(box),row);
    view->output=GTK_TEXT_VIEW(gtk_text_view_new());gtk_text_view_set_editable(view->output,FALSE);gtk_text_view_set_monospace(view->output,TRUE);gtk_text_view_set_wrap_mode(view->output,GTK_WRAP_WORD_CHAR);
    GtkWidget *scroll=gtk_scrolled_window_new();gtk_widget_set_vexpand(scroll,TRUE);gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),GTK_WIDGET(view->output));gtk_box_append(GTK_BOX(box),scroll);
    row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);view->topic=entry(row,_("Publish topic (no wildcards)"));view->retain=GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Retain")));gtk_box_append(GTK_BOX(row),GTK_WIDGET(view->retain));gtk_box_append(GTK_BOX(box),row);
    row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);view->payload=entry(row,_("Payload"));const char *modes[]={_("Text"),"HEX",NULL};view->mode=GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes));gtk_box_append(GTK_BOX(row),GTK_WIDGET(view->mode));
    GtkWidget *publish=gtk_button_new_with_label(_("Publish"));gtk_box_append(GTK_BOX(row),publish);gtk_box_append(GTK_BOX(box),row);g_signal_connect(publish,"clicked",G_CALLBACK(publish_clicked),view);g_signal_connect(view->payload,"activate",G_CALLBACK(publish_clicked),view);
    view->preview=GTK_LABEL(gtk_label_new(""));gtk_label_set_wrap(view->preview,TRUE);gtk_label_set_selectable(view->preview,TRUE);gtk_box_append(GTK_BOX(box),GTK_WIDGET(view->preview));
    view->status=GTK_LABEL(gtk_label_new(_("Ready — TCP transport, clean session, QoS 0/1")));gtk_label_set_wrap(view->status,TRUE);gtk_box_append(GTK_BOX(box),GTK_WIDGET(view->status));
    g_signal_connect(view->payload,"changed",G_CALLBACK(preview_changed),view);g_signal_connect(view->mode,"notify::selected",G_CALLBACK(preview_notify),view);
    gtk_window_present(GTK_WINDOW(view->window));return view->window;
}
