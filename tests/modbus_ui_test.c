/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/modbus_ui.c"
#include <stdlib.h>
#include <string.h>
int main(void)
{
    gtk_init();const char *endpoint=g_getenv("TIO_TEST_MODBUS_ENDPOINT"),*port=g_getenv("TIO_TEST_MODBUS_PORT");
    GtkWidget *parent=gtk_window_new(),*window=tio_modbus_window(GTK_WINDOW(parent),port?NULL:endpoint);
    View *view=g_object_get_data(G_OBJECT(window),"modbus-view");
    if(port){gtk_editable_set_text(GTK_EDITABLE(view->host),endpoint);gtk_spin_button_set_value(view->port,atoi(port));}
    run(NULL,view);gint64 until=g_get_monotonic_time()+5000000;
    while(view->cancel && g_get_monotonic_time()<until){g_main_context_iteration(NULL,FALSE);g_usleep(1000);}
    g_assert_null(view->cancel);
    GtkTextBuffer *buffer=gtk_text_view_get_buffer(view->output);GtkTextIter begin,end;gtk_text_buffer_get_bounds(buffer,&begin,&end);
    g_autofree gchar *text=gtk_text_buffer_get_text(buffer,&begin,&end,FALSE);
    g_assert_nonnull(strstr(text,"4660"));g_assert_nonnull(strstr(text,"43981"));
    gtk_window_destroy(GTK_WINDOW(window));gtk_window_destroy(GTK_WINDOW(parent));return 0;
}
