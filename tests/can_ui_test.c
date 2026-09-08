/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#include "../src/can_ui.c"
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  gtk_init();
  GtkWidget *parent = gtk_window_new(),
            *window = tio_can_window(GTK_WINDOW(parent));
  CanView *view = g_object_get_data(G_OBJECT(window), "can-view");
  gtk_editable_set_text(GTK_EDITABLE(view->interface), argv[1]);
  connect_clicked(NULL, view);
  g_assert_nonnull(view->bus);
  const char *dbc =
      "BO_ 291 Board: 8 ECU\n SG_ value : 0|16@1+ (1,0) [0|65535] \"\" ECU\n";
  view->dbc = tio_dbc_parse(dbc, strlen(dbc), NULL);
  g_assert_nonnull(view->dbc);
  int peer = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
  struct sockaddr_can address = {.can_family = AF_CAN,
                                 .can_ifindex = (int)if_nametoindex(argv[1])};
  g_assert_cmpint(bind(peer, (struct sockaddr *)&address, sizeof address), ==,
                  0);
  gtk_editable_set_text(GTK_EDITABLE(view->id), "123");
  gtk_editable_set_text(GTK_EDITABLE(view->payload), "34 12 00 00 00 00 00 00");
  send_clicked(NULL, view);
  struct can_frame frame = {0};
  ssize_t count = -1;
  gint64 until = g_get_monotonic_time() + 3000000;
  while (count < 0 && g_get_monotonic_time() < until) {
    count = read(peer, &frame, sizeof frame);
    g_usleep(1000);
  }
  g_assert_cmpint(count, ==, CAN_MTU);
  g_assert_cmpuint(frame.can_id, ==, 0x123);
  g_assert_cmpuint(frame.data[0], ==, 0x34);
  g_assert_cmpint(write(peer, &frame, sizeof frame), ==, sizeof frame);
  while (!view->received && g_get_monotonic_time() < until) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
  g_assert_cmpuint(view->received, ==, 1);
  const GQueue *entries = tio_log_model_entries(view->model);
  g_assert_cmpuint(entries->length, ==, 1);
  TioLogEntry *entry = g_queue_peek_head((GQueue *)entries);
  double value = 0;
  g_assert_true(tio_log_entry_number(entry, "value", &value));
  g_assert_cmpfloat(value, ==, 4660);
  analyze_clicked(NULL, view);
  g_assert_nonnull(view->analyzer);
  disconnect_clicked(NULL, view);
  g_assert_null(view->bus);
  close(peer);
  gtk_window_destroy(GTK_WINDOW(window));
  gtk_window_destroy(GTK_WINDOW(parent));
  return 0;
}
