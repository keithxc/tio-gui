/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#include "can.h"
#include <linux/can/raw.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct {
  guint count;
  struct canfd_frame frame;
  gboolean fd;
} Sink;
static void event(const struct canfd_frame *frame, gboolean fd,
                  const char *error, gpointer data) {
  Sink *sink = data;
  g_assert_null(error);
  sink->frame = *frame;
  sink->fd = fd;
  ++sink->count;
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  Sink sink = {0};
  g_autoptr(GError) error = NULL;
  TioCan *bus = tio_can_open(argv[1], event, &sink, &error);
  g_assert_no_error(error);
  g_assert_nonnull(bus);
  int peer = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW),
      enabled = 1;
  g_assert_cmpint(peer, >=, 0);
  g_assert_cmpint(setsockopt(peer, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enabled,
                             sizeof enabled),
                  ==, 0);
  struct sockaddr_can address = {.can_family = AF_CAN,
                                 .can_ifindex = (int)if_nametoindex(argv[1])};
  g_assert_cmpint(bind(peer, (struct sockaddr *)&address, sizeof address), ==,
                  0);
  guint8 bytes[64];
  for (guint i = 0; i < 64; ++i)
    bytes[i] = (guint8)i;
  for (guint fd = 0; fd < 2; ++fd) {
    g_assert_true(tio_can_send(bus, fd ? 0x1abcdefu : 0x123u, fd, fd, fd, FALSE,
                               bytes, fd ? 64 : 8, NULL));
    struct canfd_frame got = {0};
    ssize_t count = -1;
    gint64 until = g_get_monotonic_time() + 2000000;
    while (count < 0 && g_get_monotonic_time() < until) {
      count = read(peer, &got, sizeof got);
      g_usleep(1000);
    }
    g_assert_cmpint(count, ==, fd ? CANFD_MTU : CAN_MTU);
    g_assert_cmpmem(got.data, got.len, bytes, fd ? 64 : 8);
    got.can_id = 0x321;
    g_assert_cmpint(write(peer, &got, (size_t)count), ==, count);
    guint before = sink.count;
    while (sink.count == before && g_get_monotonic_time() < until) {
      g_main_context_iteration(NULL, FALSE);
      g_usleep(1000);
    }
    g_assert_cmpuint(sink.count, ==, before + 1);
    g_assert_cmpuint(sink.frame.can_id, ==, 0x321);
    g_assert_cmpint(sink.fd, ==, (gboolean)fd);
    g_assert_cmpmem(sink.frame.data, sink.frame.len, bytes, fd ? 64 : 8);
  }
  g_assert_false(
      tio_can_send(bus, 0x800, FALSE, FALSE, FALSE, FALSE, bytes, 8, NULL));
  g_assert_false(
      tio_can_send(bus, 1, FALSE, TRUE, FALSE, FALSE, bytes, 9, NULL));
  tio_can_free(bus);
  close(peer);
  g_print("PASS kernel SocketCAN classic/FD/extended/BRS roundtrip and invalid "
          "frame rejection\n");
  return 0;
}
