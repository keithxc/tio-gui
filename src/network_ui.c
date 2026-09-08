/* SPDX-License-Identifier: GPL-3.0-only */
#include "network_ui.h"
#include "network.h"
#include "payload.h"
#include "log_model.h"
#include "analyzer.h"
#include <libintl.h>
#include <string.h>
#define _(s) gettext(s)
typedef struct {
    GtkWidget *window, *analyzer;
    GtkEntry *host, *payload;
    GtkSpinButton *port;
    GtkDropDown *protocol, *mode, *ending, *crc;
    GtkCheckButton *hex, *follow;
    GtkLabel *status, *preview;
    GtkTextView *output;
    TioNetwork *network;
    TioLogModel *model;
    guint64 rx, tx, packets;
} NetworkView;
static void append(NetworkView *view, const char *text)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->output);
    GtkTextIter end; gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, text, -1); gtk_text_buffer_insert(buffer, &end, "\n", 1);
    if (gtk_text_buffer_get_line_count(buffer) > 2000 || gtk_text_buffer_get_char_count(buffer) > 262144) {
        GtkTextIter begin, keep; gtk_text_buffer_get_start_iter(buffer, &begin);
        gtk_text_buffer_get_iter_at_line(buffer, &keep, 500); gtk_text_buffer_delete(buffer, &begin, &keep);
    }
    if (gtk_check_button_get_active(view->follow)) {
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_view_scroll_to_iter(view->output, &end, 0, FALSE, 0, 0);
    }
}
static void event(TioNetworkEvent kind, const guint8 *bytes, gsize length, gpointer data)
{
    NetworkView *view = data;
    if (kind == TIO_NET_STATUS) {
        g_autofree gchar *message = g_strndup((const char *)bytes, length);
        gtk_label_set_text(view->status, message); append(view, message);
        tio_log_model_command(view->model, message, g_get_real_time()); return;
    }
    if (kind == TIO_NET_RX) { view->rx += length; ++view->packets; tio_log_model_feed(view->model, bytes, length, g_get_real_time()); }
    else view->tx += length;
    g_autoptr(GString) line = g_string_new(kind == TIO_NET_RX ? "RX " : "TX ");
    g_string_append_printf(line, "(%zu bytes) ", length);
    if (gtk_check_button_get_active(view->hex)) {
        for (gsize i = 0; i < MIN(length, 512u); ++i) g_string_append_printf(line, "%02X ", bytes[i]);
    } else {
        /* Escape terminal controls, keeping the text preview unambiguous. */
        for (gsize i = 0; i < MIN(length, 512u); ++i) {
            if (bytes[i] >= 32 && bytes[i] < 127) g_string_append_c(line, (char)bytes[i]);
            else g_string_append_printf(line, "\\x%02X", bytes[i]);
        }
    }
    if (length > 512) g_string_append(line, "…");
    append(view, line->str);
    if (kind == TIO_NET_TX) tio_log_model_command(view->model, line->str, g_get_real_time());
    g_autofree gchar *status = g_strdup_printf(_("RX %" G_GUINT64_FORMAT " bytes / %" G_GUINT64_FORMAT " chunks · TX %" G_GUINT64_FORMAT " bytes"), view->rx, view->packets, view->tx);
    gtk_label_set_text(view->status, status);
}
static void connect_clicked(GtkButton *button, gpointer data)
{
    (void)button; NetworkView *view = data;
    g_clear_pointer(&view->network, tio_network_free);
    g_autoptr(GError) error = NULL;
    view->network = tio_network_connect(gtk_editable_get_text(GTK_EDITABLE(view->host)),
        (guint16)gtk_spin_button_get_value_as_int(view->port), gtk_drop_down_get_selected(view->protocol) == 1, event, view, &error);
    gtk_label_set_text(view->status, view->network ? _("Connecting…") : error->message);
}
static void disconnect_clicked(GtkButton *button, gpointer data)
{
    (void)button; NetworkView *view = data; g_clear_pointer(&view->network, tio_network_free);
    gtk_label_set_text(view->status, _("Disconnected"));
}
static GByteArray *payload(NetworkView *view, GError **error)
{
    return tio_payload_build(gtk_editable_get_text(GTK_EDITABLE(view->payload)), gtk_drop_down_get_selected(view->mode) == 1,
        gtk_drop_down_get_selected(view->ending), gtk_drop_down_get_selected(view->crc), error);
}
static void preview_changed(GtkWidget *widget, gpointer data)
{
    (void)widget; NetworkView *view = data;
    g_autoptr(GError) error = NULL; g_autoptr(GByteArray) bytes = payload(view, &error);
    g_autofree gchar *text = bytes ? tio_payload_preview(bytes) : g_strdup(error->message);
    gtk_label_set_text(view->preview, text);
}
static void preview_notify(GObject *object, GParamSpec *spec, gpointer data) { (void)spec; preview_changed(GTK_WIDGET(object), data); }
static void send_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget; NetworkView *view = data;
    g_autoptr(GError) error = NULL; g_autoptr(GByteArray) bytes = payload(view, &error);
    if (!bytes) { gtk_label_set_text(view->status, error->message); return; }
    g_autoptr(GBytes) message = g_bytes_new(bytes->data, bytes->len);
    if (!tio_network_send(view->network, message, &error)) gtk_label_set_text(view->status, error->message);
}
static void analyze_clicked(GtkButton *button, gpointer data)
{
    (void)button; NetworkView *view = data;
    if (view->analyzer) { gtk_window_present(GTK_WINDOW(view->analyzer)); return; }
    view->analyzer = tio_analyzer_new(GTK_WINDOW(view->window), view->model);
    g_object_add_weak_pointer(G_OBJECT(view->analyzer), (gpointer *)&view->analyzer);
}
static void free_view(gpointer data)
{
    NetworkView *view = data;
    if (view->analyzer) gtk_window_destroy(GTK_WINDOW(view->analyzer));
    tio_network_free(view->network); tio_log_model_free(view->model); g_free(view);
}
GtkWidget *tio_network_window(GtkWindow *parent)
{
    NetworkView *view = g_new0(NetworkView, 1); view->model = tio_log_model_new();
    view->window = gtk_window_new(); gtk_window_set_title(GTK_WINDOW(view->window), _("TCP / UDP debugging"));
    gtk_window_set_transient_for(GTK_WINDOW(view->window), parent); gtk_window_set_destroy_with_parent(GTK_WINDOW(view->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(view->window), 1000, 650);
    g_object_set_data_full(G_OBJECT(view->window), "network-view", view, free_view);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12); gtk_window_set_child(GTK_WINDOW(view->window), box);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *protocols[] = {"TCP client", "UDP peer", NULL};
    view->protocol = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(protocols)); gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->protocol));
    view->host = GTK_ENTRY(gtk_entry_new()); gtk_entry_set_placeholder_text(view->host, _("Hostname or IP address"));
    gtk_widget_set_hexpand(GTK_WIDGET(view->host), TRUE); gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->host));
    view->port = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1)); gtk_spin_button_set_value(view->port, 5000);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->port));
    GtkWidget *connect = gtk_button_new_with_label(_("Connect")), *disconnect = gtk_button_new_with_label(_("Disconnect")), *analyze = gtk_button_new_with_label(_("Analyze…"));
    g_signal_connect(connect, "clicked", G_CALLBACK(connect_clicked), view);
    g_signal_connect(disconnect, "clicked", G_CALLBACK(disconnect_clicked), view); g_signal_connect(analyze, "clicked", G_CALLBACK(analyze_clicked), view);
    gtk_box_append(GTK_BOX(row), connect); gtk_box_append(GTK_BOX(row), disconnect); gtk_box_append(GTK_BOX(row), analyze); gtk_box_append(GTK_BOX(box), row);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    view->hex = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("HEX display")));
    view->follow = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Follow"))); gtk_check_button_set_active(view->follow, TRUE);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->hex)); gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->follow)); gtk_box_append(GTK_BOX(box), row);
    view->output = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_editable(view->output, FALSE); gtk_text_view_set_monospace(view->output, TRUE);
    gtk_text_view_set_wrap_mode(view->output, GTK_WRAP_WORD_CHAR);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(view->output)); gtk_box_append(GTK_BOX(box), scroll);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    view->payload = GTK_ENTRY(gtk_entry_new()); gtk_widget_set_hexpand(GTK_WIDGET(view->payload), TRUE); gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->payload));
    const char *modes[] = {_("Text"), "HEX", NULL}, *endings[] = {_("No ending"), "LF", "CR", "CRLF", NULL}, *crcs[] = {_("No CRC"), "CRC8", "CRC16/MODBUS", "CRC32", NULL};
    view->mode = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes)); view->ending = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(endings)); view->crc = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(crcs));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->mode)); gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->ending)); gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->crc));
    GtkWidget *send = gtk_button_new_with_label(_("Send")); gtk_box_append(GTK_BOX(row), send); gtk_box_append(GTK_BOX(box), row);
    view->preview = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(view->preview, TRUE); gtk_label_set_selectable(view->preview, TRUE); gtk_box_append(GTK_BOX(box), GTK_WIDGET(view->preview));
    view->status = GTK_LABEL(gtk_label_new(_("Ready"))); gtk_label_set_wrap(view->status, TRUE); gtk_box_append(GTK_BOX(box), GTK_WIDGET(view->status));
    g_signal_connect(send, "clicked", G_CALLBACK(send_clicked), view); g_signal_connect(view->payload, "activate", G_CALLBACK(send_clicked), view);
    g_signal_connect(view->payload, "changed", G_CALLBACK(preview_changed), view);
    g_signal_connect(view->mode, "notify::selected", G_CALLBACK(preview_notify), view); g_signal_connect(view->ending, "notify::selected", G_CALLBACK(preview_notify), view); g_signal_connect(view->crc, "notify::selected", G_CALLBACK(preview_notify), view);
    gtk_window_present(GTK_WINDOW(view->window)); return view->window;
}
