/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include "native_serial.h"
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#ifndef G_OS_WIN32
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#endif
static void invalid(void)
{
    TioNativeConfig config = {"", 115200, 8, 1, 0, 0, FALSE};
    g_autoptr(GError) error = NULL;
    g_assert_null(tio_native_start(&config, &error)); g_assert_nonnull(error);
    g_clear_error(&error); config.device = "COM1"; config.baud = 0;
    g_assert_null(tio_native_start(&config, &error)); g_assert_nonnull(error);
}
static void absent(void)
{
#ifdef G_OS_WIN32
    const char *device = "COM9999";
#else
    const char *device = "/nonexistent-tio-test-port";
#endif
    TioNativeConfig config = {device, 115200, 8, 1, 0, 0, FALSE};
    TioNativeSerial *s = tio_native_start(&config, NULL); g_assert_nonnull(s);
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    TioNativeEvent *e = NULL;
    while (!(e = tio_native_poll(s)) && g_get_monotonic_time() < deadline) g_usleep(1000);
    g_assert_nonnull(e); g_assert_cmpint(e->kind, ==, TIO_NATIVE_STATUS); g_assert_false(e->connected);
    tio_native_event_free(e); tio_native_stop(s);
}
#ifndef G_OS_WIN32
static TioNativeEvent *wait_event(TioNativeSerial *s, TioNativeEventKind kind)
{
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        TioNativeEvent *e = tio_native_poll(s);
        if (e) { if (e->kind == kind) return e; tio_native_event_free(e); }
        g_usleep(1000);
    }
    g_error("Timed out waiting for serial event %d", kind); return NULL;
}
static int make_pty(gchar **name)
{
    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    g_assert_cmpint(fd, >=, 0); g_assert_cmpint(grantpt(fd), ==, 0); g_assert_cmpint(unlockpt(fd), ==, 0);
    *name = g_strdup(ptsname(fd)); g_assert_nonnull(*name); return fd;
}
static void inherited_settings(void)
{
    g_autofree gchar *name = NULL; int master = make_pty(&name);
    int slave = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    g_assert_cmpint(slave, >=, 0);
    struct termios t;
    g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
    t.c_iflag |= IXON | IXOFF | IXANY | INPCK | IGNPAR;
    g_assert_cmpint(tcsetattr(slave, TCSANOW, &t), ==, 0);
    TioNativeConfig config = {name, 115200, 8, 1, 0, 0, FALSE};
    TioNativeSerial *s = tio_native_start(&config, NULL);
    TioNativeEvent *e = wait_event(s, TIO_NATIVE_STATUS);
    g_assert_true(e->connected); tio_native_event_free(e);
    g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
    g_assert_cmpuint(t.c_iflag & (IXON | IXOFF | IXANY | INPCK | IGNPAR), ==, 0);
    tio_native_stop(s); close(slave); close(master);
}
static void transport(void)
{
    g_autofree gchar *name = NULL; int master = make_pty(&name);
    TioNativeConfig config = {name, 115200, 8, 1, 0, 0, FALSE};
    TioNativeSerial *s = tio_native_start(&config, NULL);
    TioNativeEvent *e = wait_event(s, TIO_NATIVE_STATUS);
    if (!e->connected) g_error("Open PTY: %s", e->message);
    tio_native_event_free(e);
    /* All control bytes, including NUL, XON, XOFF and Ctrl-T, stay intact. */
    guint8 bytes[256]; for (guint i = 0; i < 256; i++) bytes[i] = (guint8)i;
    g_assert_cmpint(write(master, bytes, sizeof bytes), ==, sizeof bytes);
    GByteArray *received = g_byte_array_new();
    while (received->len < sizeof bytes) {
        e = wait_event(s, TIO_NATIVE_RX); gsize length; const guint8 *data = g_bytes_get_data(e->bytes, &length);
        g_byte_array_append(received, data, (guint)length); tio_native_event_free(e);
    }
    g_assert_cmpmem(received->data, received->len, bytes, sizeof bytes); g_byte_array_unref(received);
    g_autoptr(GBytes) payload = g_bytes_new(bytes, sizeof bytes);
    g_assert_true(tio_native_send(s, payload, NULL));
    guint8 reply[256]; gsize total = 0; gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (total < sizeof reply && g_get_monotonic_time() < deadline) {
        ssize_t count = read(master, reply + total, sizeof reply - total);
        if (count > 0) total += (gsize)count; else g_usleep(1000);
    }
    g_assert_cmpmem(reply, total, bytes, sizeof bytes);
    e = wait_event(s, TIO_NATIVE_TX); tio_native_event_free(e);
    close(master); e = wait_event(s, TIO_NATIVE_STATUS); g_assert_false(e->connected); tio_native_event_free(e);
    g_autoptr(GError) error = NULL; g_assert_false(tio_native_send(s, payload, &error));
    tio_native_stop(s);
}
static void reconnect(void)
{
    g_autofree gchar *name = NULL; int master = make_pty(&name);
    g_autofree gchar *dir = g_dir_make_tmp("tio-native-XXXXXX", NULL);
    g_autofree gchar *link = g_build_filename(dir, "port", NULL);
    g_assert_cmpint(symlink(name, link), ==, 0);
    TioNativeConfig config = {link, 115200, 8, 1, 0, 0, TRUE};
    TioNativeSerial *s = tio_native_start(&config, NULL);
    TioNativeEvent *e = wait_event(s, TIO_NATIVE_STATUS); g_assert_true(e->connected); tio_native_event_free(e);
    close(master); e = wait_event(s, TIO_NATIVE_STATUS); g_assert_false(e->connected); tio_native_event_free(e);
    unlink(link); g_clear_pointer(&name, g_free); master = make_pty(&name); g_assert_cmpint(symlink(name, link), ==, 0);
    gint64 deadline = g_get_monotonic_time() + 4 * G_TIME_SPAN_SECOND; gboolean connected = FALSE;
    while (!connected && g_get_monotonic_time() < deadline) { e = wait_event(s, TIO_NATIVE_STATUS); connected = e->connected; tio_native_event_free(e); }
    g_assert_true(connected);
    g_assert_cmpint(write(master, "reconnected", 11), ==, 11); e = wait_event(s, TIO_NATIVE_RX);
    gsize length; const char *bytes = g_bytes_get_data(e->bytes, &length); g_assert_cmpmem(bytes, length, "reconnected", 11); tio_native_event_free(e);
    tio_native_finish(s); tio_native_stop(s); close(master); unlink(link); rmdir(dir);
}
#endif
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); g_test_add_func("/native/invalid", invalid); g_test_add_func("/native/absent", absent);
#ifndef G_OS_WIN32
    g_test_add_func("/native/binary-transport-disconnect", transport); g_test_add_func("/native/reconnect", reconnect);
    g_test_add_func("/native/inherited-settings", inherited_settings);
#endif
    return g_test_run();
}
