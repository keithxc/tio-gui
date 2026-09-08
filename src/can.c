/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#include "can.h"
#include <errno.h>
#include <glib-unix.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
struct _TioCan {
  guint refs, source;
  int fd;
  gboolean closed;
  TioCanCallback callback;
  gpointer data;
};
static TioCan *ref(TioCan *bus) {
  ++bus->refs;
  return bus;
}
static void unref(gpointer data) {
  TioCan *bus = data;
  if (!--bus->refs)
    g_free(bus);
}
void tio_can_free(TioCan *bus) {
  if (!bus)
    return;
  bus->closed = TRUE;
  bus->callback = NULL;
  if (bus->source) {
    g_source_remove(bus->source);
    bus->source = 0;
  }
  if (bus->fd >= 0) {
    close(bus->fd);
    bus->fd = -1;
  }
  unref(bus);
}
static gboolean readable(gint fd, GIOCondition condition, gpointer data) {
  (void)condition;
  TioCan *bus = ref(data);
  guint budget = 64;
  gboolean keep = TRUE;
  while (!bus->closed && budget--) {
    struct canfd_frame frame = {0};
    ssize_t length = read(fd, &frame, sizeof frame);
    if (length < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      if (bus->callback)
        bus->callback(NULL, FALSE, g_strerror(errno), bus->data);
      keep = FALSE;
      break;
    }
    if (length != CAN_MTU && length != CANFD_MTU) {
      if (bus->callback)
        bus->callback(NULL, FALSE, "Invalid SocketCAN frame size", bus->data);
      continue;
    }
    if (frame.len > (length == CAN_MTU ? 8 : 64)) {
      if (bus->callback)
        bus->callback(NULL, FALSE, "Invalid CAN payload length", bus->data);
      continue;
    }
    if (bus->callback)
      bus->callback(&frame, length == CANFD_MTU, NULL, bus->data);
  }
  if (!keep && !bus->closed) {
    bus->source = 0;
    bus->closed = TRUE;
    close(bus->fd);
    bus->fd = -1;
  }
  keep &= !bus->closed;
  unref(bus);
  return keep;
}
TioCan *tio_can_open(const char *interface, TioCanCallback callback,
                     gpointer data, GError **error) {
  guint index = interface ? if_nametoindex(interface) : 0;
  if (!index) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
        "CAN interface not found; configure a SocketCAN interface first");
    return NULL;
  }
  int fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
  if (fd < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "SocketCAN: %s", g_strerror(errno));
    return NULL;
  }
  int enable = 1;
  can_err_mask_t mask = CAN_ERR_MASK;
  struct sockaddr_can address = {.can_family = AF_CAN,
                                 .can_ifindex = (int)index};
  if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof enable) ||
      setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &mask, sizeof mask) ||
      bind(fd, (struct sockaddr *)&address, sizeof address)) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "SocketCAN: %s", g_strerror(errno));
    close(fd);
    return NULL;
  }
  TioCan *bus = g_new0(TioCan, 1);
  bus->refs = 1;
  bus->fd = fd;
  bus->callback = callback;
  bus->data = data;
  bus->source =
      g_unix_fd_add_full(G_PRIORITY_DEFAULT, fd, G_IO_IN | G_IO_ERR | G_IO_HUP,
                         readable, ref(bus), unref);
  return bus;
}
gboolean tio_can_send(TioCan *bus, guint32 id, gboolean extended, gboolean fd,
                      gboolean brs, gboolean rtr, const guint8 *bytes,
                      gsize length, GError **error) {
  if (!bus || bus->closed || id > (extended ? CAN_EFF_MASK : CAN_SFF_MASK) ||
      length > (fd ? 64u : 8u) || (length && !bytes) || (fd && rtr) ||
      (!fd && brs)) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "Invalid CAN ID/frame flags/payload or interface disconnected");
    return FALSE;
  }
  /* CAN FD DLC sizes above 8 are discrete. Refuse implicit padding. */
  if (fd && length > 8 && length != 12 && length != 16 && length != 20 &&
      length != 24 && length != 32 && length != 48 && length != 64) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "CAN FD length must be 0–8, 12, 16, 20, 24, 32, 48 or 64 bytes");
    return FALSE;
  }
  struct canfd_frame frame = {.can_id = id | (extended ? CAN_EFF_FLAG : 0) |
                                        (rtr ? CAN_RTR_FLAG : 0),
                              .len = (guint8)length,
                              .flags = brs ? CANFD_BRS : 0};
  if (length && !rtr)
    memcpy(frame.data, bytes, length);
  ssize_t size = fd ? CANFD_MTU : CAN_MTU;
  if (write(bus->fd, &frame, (size_t)size) != size) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "CAN send: %s",
                g_strerror(errno));
    return FALSE;
  }
  return TRUE;
}
