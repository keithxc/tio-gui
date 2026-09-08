/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/mqtt_ui.c"
#include <stdlib.h>
int main(int argc,char **argv)
{
    if(argc!=2)return 2;gtk_init();GtkWidget *parent=gtk_window_new(),*window=tio_mqtt_window(GTK_WINDOW(parent));
    MqttView *view=g_object_get_data(G_OBJECT(window),"mqtt-view");gtk_editable_set_text(GTK_EDITABLE(view->host),"127.0.0.1");gtk_spin_button_set_value(view->port,atoi(argv[1]));connect_clicked(NULL,view);
    gint64 until=g_get_monotonic_time()+5000000;
    while(!tio_mqtt_ready(view->mqtt)&&g_get_monotonic_time()<until){g_main_context_iteration(NULL,FALSE);g_usleep(1000);}g_assert_true(tio_mqtt_ready(view->mqtt));
    gtk_editable_set_text(GTK_EDITABLE(view->subscription),"tio/test/#");GtkWidget *button=g_object_ref_sink(gtk_button_new());subscribe_clicked(GTK_BUTTON(button),view);
    while(!g_str_has_prefix(gtk_label_get_text(view->status),"Subscribed")&&g_get_monotonic_time()<until){g_main_context_iteration(NULL,FALSE);g_usleep(1000);}
    g_assert_true(g_str_has_prefix(gtk_label_get_text(view->status),"Subscribed"));
    gtk_editable_set_text(GTK_EDITABLE(view->topic),"tio/test/bytes");gtk_drop_down_set_selected(view->mode,1);gtk_editable_set_text(GTK_EDITABLE(view->payload),"00 14 FF");publish_clicked(NULL,view);
    while(!view->messages&&g_get_monotonic_time()<until){g_main_context_iteration(NULL,FALSE);g_usleep(1000);}g_assert_cmpuint(view->messages,==,1);
    g_assert_nonnull(strstr(gtk_label_get_text(view->preview),"00 14 FF"));analyze_clicked(NULL,view);g_assert_nonnull(view->analyzer);
    disconnect_clicked(NULL,view);g_assert_null(view->mqtt);g_object_unref(button);gtk_window_destroy(GTK_WINDOW(window));gtk_window_destroy(GTK_WINDOW(parent));return 0;
}
