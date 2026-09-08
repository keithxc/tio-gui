/* SPDX-License-Identifier: GPL-3.0-only */
#include "can_ui.h"
#include "analyzer.h"
#include "can.h"
#include "dbc.h"
#include "log_model.h"
#include "payload.h"
#include <libintl.h>
#include <string.h>
#define _(s) gettext(s)
typedef struct {
  GtkWidget *window, *analyzer;
  GtkEntry *interface, *id, *payload;
  GtkCheckButton *extended, *fd, *brs, *rtr;
  GtkTextView *output;
  GtkLabel *status;
  TioCan *bus;
  TioDbc *dbc;
  TioLogModel *model;
  guint received;
} CanView;
static void append(CanView *view, const char *text) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->output);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_insert(buffer, &end, text, -1);
  gtk_text_buffer_insert(buffer, &end, "\n", 1);
  if (gtk_text_buffer_get_line_count(buffer) > 2000) {
    GtkTextIter begin, keep;
    gtk_text_buffer_get_start_iter(buffer, &begin);
    gtk_text_buffer_get_iter_at_line(buffer, &keep, 500);
    gtk_text_buffer_delete(buffer, &begin, &keep);
  }
}
static void event(const struct canfd_frame *frame, gboolean fd,
                  const char *error, gpointer data) {
  CanView *view = data;
  if (error) {
    gtk_label_set_text(view->status, error);
    return;
  }
  ++view->received;
  g_autoptr(GString) line = g_string_new(NULL);
  g_string_append_printf(line, "RX %08X %s%s%s ", frame->can_id,
                         fd ? "FD " : "",
                         frame->flags & CANFD_BRS ? "BRS " : "",
                         frame->can_id & CAN_RTR_FLAG ? "RTR " : "");
  for (guint i = 0; i < frame->len; ++i)
    g_string_append_printf(line, "%02X ", frame->data[i]);
  append(view, line->str);
  if (frame->can_id & CAN_ERR_FLAG) {
    gtk_label_set_text(
        view->status,
        _("CAN error frame received; inspect interface/bus state"));
    return;
  }
  if (frame->can_id & CAN_RTR_FLAG)
    return;
  g_autoptr(GError) decode_error = NULL;
  g_autofree gchar *json = tio_dbc_decode(
      view->dbc, frame->can_id & CAN_EFF_MASK, !!(frame->can_id & CAN_EFF_FLAG),
      frame->data, frame->len, &decode_error);
  if (decode_error)
    gtk_label_set_text(view->status, decode_error->message);
  if (json) {
    append(view, json);
    tio_log_model_feed(view->model, (const guint8 *)json, strlen(json),
                       g_get_real_time());
    tio_log_model_feed(view->model, (const guint8 *)"\n", 1, g_get_real_time());
  } else
    tio_log_model_command(view->model, line->str, g_get_real_time());
}
static void connect_clicked(GtkButton *button, gpointer data) {
  (void)button;
  CanView *view = data;
  g_clear_pointer(&view->bus, tio_can_free);
  g_autoptr(GError) error = NULL;
  view->bus = tio_can_open(gtk_editable_get_text(GTK_EDITABLE(view->interface)),
                           event, view, &error);
  gtk_label_set_text(view->status, view->bus ? _("SocketCAN interface opened")
                                             : error->message);
}
static void disconnect_clicked(GtkButton *button, gpointer data) {
  (void)button;
  CanView *view = data;
  g_clear_pointer(&view->bus, tio_can_free);
  gtk_label_set_text(view->status, _("Disconnected"));
}
static void send_clicked(GtkWidget *widget, gpointer data) {
  (void)widget;
  CanView *view = data;
  g_autoptr(GError) error = NULL;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(view->id));
  char *end;
  guint64 id = g_ascii_strtoull(text, &end, 16);
  if (!*text || *end || id > CAN_EFF_MASK) {
    gtk_label_set_text(view->status,
                       _("Enter a valid hexadecimal CAN identifier"));
    return;
  }
  g_autoptr(GByteArray) bytes = tio_payload_build(
      gtk_editable_get_text(GTK_EDITABLE(view->payload)), TRUE, 0, 0, &error);
  if (!bytes) {
    gtk_label_set_text(view->status, error->message);
    return;
  }
  if (!tio_can_send(view->bus, (guint32)id,
                    gtk_check_button_get_active(view->extended),
                    gtk_check_button_get_active(view->fd),
                    gtk_check_button_get_active(view->brs),
                    gtk_check_button_get_active(view->rtr), bytes->data,
                    bytes->len, &error))
    gtk_label_set_text(view->status, error->message);
  else {
    g_autofree gchar *preview = tio_payload_preview(bytes);
    g_autofree gchar *line =
        g_strdup_printf("TX %08X %s", (guint32)id, preview);
    append(view, line);
    gtk_label_set_text(view->status, _("Frame accepted by SocketCAN"));
  }
}
static void dbc_selected(GObject *source, GAsyncResult *result, gpointer data) {
  GWeakRef *weak = data;
  g_autoptr(GObject) window = g_weak_ref_get(weak);
  g_weak_ref_clear(weak);
  g_free(weak);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file =
      gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  if (!window || !gtk_widget_get_visible(GTK_WIDGET(window)) || !file)
    return;
  CanView *view = g_object_get_data(window, "can-view");
  g_autoptr(GFileInfo) info =
      g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                        G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
    gtk_label_set_text(view->status,
                       error ? error->message : _("Choose a regular DBC file"));
    return;
  }
  g_autoptr(GFileInputStream) input = g_file_read(file, NULL, &error);
  TioDbc *dbc = NULL;
  if (input) {
    g_autofree gchar *text = g_malloc(4194305);
    gsize length;
    if (g_input_stream_read_all(G_INPUT_STREAM(input), text, 4194305, &length,
                                NULL, &error))
      dbc = tio_dbc_parse(text, length, &error);
  }
  if (!dbc) {
    gtk_label_set_text(view->status,
                       error ? error->message : _("Could not load DBC"));
    return;
  }
  g_clear_pointer(&view->dbc, tio_dbc_free);
  view->dbc = dbc;
  gtk_label_set_text(
      view->status,
      _("DBC loaded; matching frames produce numeric fields in Analyze"));
}
static void load_dbc(GtkButton *button, gpointer data) {
  (void)button;
  CanView *view = data;
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Load DBC definitions"));
  GWeakRef *weak = g_new0(GWeakRef, 1);
  g_weak_ref_init(weak, view->window);
  gtk_file_dialog_open(dialog, GTK_WINDOW(view->window), NULL, dbc_selected,
                       weak);
}
static void analyze_clicked(GtkButton *button, gpointer data) {
  (void)button;
  CanView *view = data;
  if (view->analyzer) {
    gtk_window_present(GTK_WINDOW(view->analyzer));
    return;
  }
  view->analyzer = tio_analyzer_new(GTK_WINDOW(view->window), view->model);
  g_object_add_weak_pointer(G_OBJECT(view->analyzer),
                            (gpointer *)&view->analyzer);
}
static void free_view(gpointer data) {
  CanView *view = data;
  if (view->analyzer)
    gtk_window_destroy(GTK_WINDOW(view->analyzer));
  tio_can_free(view->bus);
  tio_dbc_free(view->dbc);
  tio_log_model_free(view->model);
  g_free(view);
}
GtkWidget *tio_can_window(GtkWindow *parent) {
  CanView *view = g_new0(CanView, 1);
  view->model = tio_log_model_new();
  view->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(view->window), _("CAN / DBC debugging"));
  gtk_window_set_transient_for(GTK_WINDOW(view->window), parent);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(view->window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(view->window), 950, 620);
  g_object_set_data_full(G_OBJECT(view->window), "can-view", view, free_view);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_window_set_child(GTK_WINDOW(view->window), box);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  GtkWidget *hint =
      gtk_label_new(_("Use a configured SocketCAN interface. DBC supports "
                      "integer signals, Intel/Motorola byte order, scaling and "
                      "simple multiplexing. Analyze plots decoded fields."));
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_box_append(GTK_BOX(box), hint);
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  view->interface = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(view->interface, _("Interface, e.g. can0"));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->interface));
  const char *labels[] = {_("Open"), _("Close"), _("Load DBC…"), _("Analyze…")};
  GCallback callbacks[] = {G_CALLBACK(connect_clicked),
                           G_CALLBACK(disconnect_clicked), G_CALLBACK(load_dbc),
                           G_CALLBACK(analyze_clicked)};
  for (guint i = 0; i < 4; ++i) {
    GtkWidget *button = gtk_button_new_with_label(labels[i]);
    g_signal_connect(button, "clicked", callbacks[i], view);
    gtk_box_append(GTK_BOX(row), button);
  }
  gtk_box_append(GTK_BOX(box), row);
  view->output = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(view->output, FALSE);
  gtk_text_view_set_monospace(view->output, TRUE);
  gtk_text_view_set_wrap_mode(view->output, GTK_WRAP_WORD_CHAR);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(view->output));
  gtk_box_append(GTK_BOX(box), scroll);
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  view->id = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(view->id, _("CAN ID (hex)"));
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->id));
  view->extended =
      GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("29-bit ID")));
  view->fd = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("CAN FD"));
  view->brs = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("BRS"));
  view->rtr = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("RTR"));
  GtkCheckButton *checks[] = {view->extended, view->fd, view->brs, view->rtr};
  for (guint i = 0; i < 4; ++i)
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(checks[i]));
  gtk_box_append(GTK_BOX(box), row);
  row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  view->payload = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(
      view->payload, _("HEX payload; RTR uses its length as requested DLC"));
  gtk_widget_set_hexpand(GTK_WIDGET(view->payload), TRUE);
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(view->payload));
  GtkWidget *send = gtk_button_new_with_label(_("Send frame"));
  gtk_box_append(GTK_BOX(row), send);
  gtk_box_append(GTK_BOX(box), row);
  g_signal_connect(send, "clicked", G_CALLBACK(send_clicked), view);
  view->status = GTK_LABEL(gtk_label_new(_("Ready")));
  gtk_label_set_wrap(view->status, TRUE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(view->status));
  gtk_window_present(GTK_WINDOW(view->window));
  return view->window;
}
