/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/network_ui.c"
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc != 3) return 2; gtk_init();
    GtkWidget *parent = gtk_window_new(); GtkWidget *window = tio_network_window(GTK_WINDOW(parent));
    NetworkView *view = g_object_get_data(G_OBJECT(window), "network-view");
    gtk_editable_set_text(GTK_EDITABLE(view->host), "127.0.0.1");
    gtk_spin_button_set_value(view->port, atoi(argv[1])); gtk_drop_down_set_selected(view->protocol, (guint)atoi(argv[2]));
    connect_clicked(NULL, view);
    gint64 until = g_get_monotonic_time() + 5000000;
    while (!tio_network_ready(view->network) && g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    g_assert_true(tio_network_ready(view->network));
    g_autoptr(GString) hex = g_string_new(NULL);
    for (guint i = 0; i < 256; ++i) g_string_append_printf(hex, "%02X ", i);
    gtk_drop_down_set_selected(view->mode, 1); gtk_editable_set_text(GTK_EDITABLE(view->payload), hex->str);
    g_assert_nonnull(strstr(gtk_label_get_text(view->preview), "256 bytes"));
    send_clicked(NULL, view);
    while ((view->rx < 256 || view->tx < 256) && g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    g_assert_cmpuint(view->rx, ==, 256); g_assert_cmpuint(view->tx, ==, 256);
    analyze_clicked(NULL, view); g_assert_nonnull(view->analyzer);
    disconnect_clicked(NULL, view); g_assert_null(view->network);
    gtk_window_destroy(GTK_WINDOW(window)); gtk_window_destroy(GTK_WINDOW(parent));
    return 0;
}
