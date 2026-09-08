/* SPDX-License-Identifier: GPL-3.0-only */
#include "modbus_ui.h"
#include "modbus.h"
#include "payload.h"
#include <libintl.h>
#define _(s) gettext(s)
typedef struct {
    GtkWidget *window, *run;
    GtkEntry *host;
    GtkSpinButton *port, *unit, *address, *quantity, *value;
    GtkDropDown *function;
    GtkTextView *output;
    GtkLabel *preview;
    gchar *socket;
    GCancellable *cancel;
    guint16 transaction;
} View;
static void free_view(gpointer data)
{
    View *view = data; if (view->cancel) g_cancellable_cancel(view->cancel);
    g_clear_object(&view->cancel); g_free(view->socket); g_free(view);
}
static TioModbusRequest request(View *view)
{
    return (TioModbusRequest){.tcp = !view->socket,
        .endpoint = view->socket ? view->socket : gtk_editable_get_text(GTK_EDITABLE(view->host)),
        .port = (guint16)gtk_spin_button_get_value_as_int(view->port), .unit = (guint8)gtk_spin_button_get_value_as_int(view->unit),
        .address = (guint16)gtk_spin_button_get_value_as_int(view->address), .quantity = (guint16)gtk_spin_button_get_value_as_int(view->quantity),
        .value = (guint16)gtk_spin_button_get_value_as_int(view->value), .function = (guint8)(gtk_drop_down_get_selected(view->function) + 1),
        .transaction = view->transaction};
}
static void preview(View *view)
{
    TioModbusRequest r = request(view); g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) frame = tio_modbus_frame(&r, &error);
    if (!frame) { gtk_label_set_text(view->preview, error->message); return; }
    gsize length; const guint8 *bytes = g_bytes_get_data(frame, &length);
    g_autoptr(GByteArray) array = g_byte_array_new(); g_byte_array_append(array, bytes, (guint)length);
    g_autofree gchar *text = tio_payload_preview(array); gtk_label_set_text(view->preview, text);
}
static void changed(GtkWidget *widget, gpointer data) { (void)widget; preview(data); }
static void selected(GObject *object, GParamSpec *spec, gpointer data) { (void)object; (void)spec; preview(data); }
static void completed(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; GWeakRef *weak = data; g_autoptr(GObject) window = g_weak_ref_get(weak); g_weak_ref_clear(weak); g_free(weak);
    g_autoptr(GError) error = NULL; g_autofree gchar *text = tio_modbus_request_finish(result, &error);
    if (!window || !gtk_widget_get_visible(GTK_WIDGET(window))) return;
    View *view = g_object_get_data(window, "modbus-view");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view->output), text ? text : error->message, -1);
    g_clear_object(&view->cancel); gtk_widget_set_sensitive(view->run, TRUE);
}
static void run(GtkButton *button, gpointer data)
{
    (void)button; View *view = data; if (view->cancel) return;
    ++view->transaction; preview(view);
    TioModbusRequest r = request(view); view->cancel = g_cancellable_new();
    gtk_widget_set_sensitive(view->run, FALSE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view->output), _("Waiting for Modbus response…"), -1);
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, view->window);
    tio_modbus_request_async(&r, view->cancel, completed, weak);
}
static GtkSpinButton *spin(GtkWidget *row, const char *label, guint maximum, guint initial)
{
    gtk_box_append(GTK_BOX(row), gtk_label_new(label));
    GtkSpinButton *widget = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, maximum, 1));
    gtk_spin_button_set_value(widget, initial); gtk_box_append(GTK_BOX(row), GTK_WIDGET(widget)); return widget;
}
GtkWidget *tio_modbus_window(GtkWindow *parent, const char *socket_path)
{
    View *view = g_new0(View, 1); view->socket = g_strdup(socket_path);
    view->window = gtk_window_new(); gtk_window_set_title(GTK_WINDOW(view->window), socket_path ? _("Modbus RTU — current serial session") : _("Modbus TCP"));
    gtk_window_set_transient_for(GTK_WINDOW(view->window), parent); gtk_window_set_destroy_with_parent(GTK_WINDOW(view->window), TRUE);
    gtk_window_set_modal(GTK_WINDOW(view->window), socket_path != NULL); gtk_window_set_default_size(GTK_WINDOW(view->window), 850, 560);
    g_object_set_data_full(G_OBJECT(view->window), "modbus-view", view, free_view);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_window_set_child(GTK_WINDOW(view->window), box);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12); gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *hint = gtk_label_new(_("Addresses are zero-based protocol addresses. Functions 1–4 read; 5–6 write one coil/register. Coil write values are 0 or 1. No automatic retries."));
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE); gtk_box_append(GTK_BOX(box), hint);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    view->host = GTK_ENTRY(gtk_entry_new()); gtk_entry_set_placeholder_text(view->host, _("TCP hostname or IP"));
    gtk_widget_set_hexpand(GTK_WIDGET(view->host), TRUE); gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->host));
    view->port = spin(row, _("Port"), 65535, 502); view->unit = spin(row, _("Unit"), socket_path ? 247 : 255, 1);
    gtk_widget_set_sensitive(GTK_WIDGET(view->host), !socket_path); gtk_widget_set_sensitive(GTK_WIDGET(view->port), !socket_path);
    gtk_box_append(GTK_BOX(box), row);
    const char *functions[] = {_("01 Read coils"), _("02 Read discrete inputs"), _("03 Read holding registers"), _("04 Read input registers"), _("05 Write single coil"), _("06 Write single register"), NULL};
    view->function = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(functions)); gtk_drop_down_set_selected(view->function, 2);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(view->function));
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    view->address = spin(row, _("Address"), 65535, 0); view->quantity = spin(row, _("Read count"), 2000, 2); view->value = spin(row, _("Write value"), 65535, 0);
    gtk_box_append(GTK_BOX(box), row);
    view->preview = GTK_LABEL(gtk_label_new("")); gtk_label_set_selectable(view->preview, TRUE); gtk_label_set_wrap(view->preview, TRUE); gtk_box_append(GTK_BOX(box), GTK_WIDGET(view->preview));
    view->run = gtk_button_new_with_label(_("Execute request")); g_signal_connect(view->run, "clicked", G_CALLBACK(run), view); gtk_box_append(GTK_BOX(box), view->run);
    view->output = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_editable(view->output, FALSE); gtk_text_view_set_monospace(view->output, TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(view->output)); gtk_box_append(GTK_BOX(box), scroll);
    GtkSpinButton *spins[] = {view->unit, view->address, view->quantity, view->value};
    for (guint i = 0; i < G_N_ELEMENTS(spins); ++i) g_signal_connect(spins[i], "value-changed", G_CALLBACK(changed), view);
    g_signal_connect(view->function, "notify::selected", G_CALLBACK(selected), view);
    preview(view); gtk_window_present(GTK_WINDOW(view->window)); return view->window;
}
