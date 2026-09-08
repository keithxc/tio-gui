/* SPDX-License-Identifier: GPL-3.0-only */
#include "transfer.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
struct _TioTransfer {
    guint refs, timer, protocol;
    gboolean receive, active, cancelled, success;
    gchar *path, *status, *program;
    GSubprocess *process;
    GCancellable *cancel;
    guint8 stderr_buffer[2048];
};
static TioTransfer *ref(TioTransfer *t) { ++t->refs; return t; }
static void unref(TioTransfer *t)
{
    if (--t->refs) return;
    g_clear_object(&t->process); g_clear_object(&t->cancel);
    g_free(t->path); g_free(t->status); g_free(t->program); g_free(t);
}
static void status(TioTransfer *t, const char *text) { g_free(t->status); t->status = g_strdup(text); }
static void finish(TioTransfer *t)
{
    t->active = FALSE;
    if (t->timer) { g_source_remove(t->timer); t->timer = 0; }
}
void tio_transfer_cancel(TioTransfer *t)
{
    if (!t || !t->active) return;
    t->cancelled = TRUE; status(t, "Transfer cancelled; check the peer before resuming commands");
    g_cancellable_cancel(t->cancel);
    if (t->process) g_subprocess_force_exit(t->process);
}
void tio_transfer_free(TioTransfer *t) { if (t) { tio_transfer_cancel(t); unref(t); } }
static gboolean timeout(gpointer data)
{
    TioTransfer *t = data; t->timer = 0;
    tio_transfer_cancel(t); status(t, "Transfer timed out; check the peer before resuming commands");
    return G_SOURCE_REMOVE;
}
static void exited(GObject *source, GAsyncResult *result, gpointer data)
{
    TioTransfer *t = data;
    g_autoptr(GError) error = NULL;
    gboolean ok = g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
    t->success = ok && !t->cancelled && g_subprocess_get_successful(t->process);
    if (!t->cancelled) {
        if (t->success) status(t, "Transfer completed");
        else if (error) status(t, error->message);
        else { g_autofree gchar *message = g_strdup_printf("Transfer failed: %s", t->status); status(t, message); }
    }
    finish(t); unref(t);
}
static void read_stderr(TioTransfer *t);
static void stderr_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    TioTransfer *t = data;
    gssize length = g_input_stream_read_finish(G_INPUT_STREAM(source), result, NULL);
    if (length > 0) {
        if (t->active && !t->cancelled) {
            g_autofree gchar *valid = g_utf8_make_valid((char *)t->stderr_buffer, length);
            status(t, valid);
        }
        read_stderr(t);
    }
    unref(t);
}
static void read_stderr(TioTransfer *t)
{
    g_input_stream_read_async(g_subprocess_get_stderr_pipe(t->process), t->stderr_buffer,
        sizeof t->stderr_buffer, G_PRIORITY_DEFAULT, NULL, stderr_ready, ref(t));
}
static void connected(GObject *source, GAsyncResult *result, gpointer data)
{
    TioTransfer *t = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketConnection) connection = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), result, &error);
    if (!connection || t->cancelled) {
        if (!t->cancelled) status(t, error->message);
        finish(t); unref(t); return;
    }
    int fd = g_socket_get_fd(g_socket_connection_get_socket(connection));
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        status(t, "Could not prepare transfer stream"); finish(t); unref(t); return;
    }
    int input = dup(fd), output = dup(fd);
    if (input < 0 || output < 0) {
        if (input >= 0) close(input);
        if (output >= 0) close(output);
        status(t, "Could not duplicate transfer stream"); finish(t); unref(t); return;
    }
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_take_stdin_fd(launcher, input);
    g_subprocess_launcher_take_stdout_fd(launcher, output);
    g_autofree gchar *directory = t->receive ? g_strdup(t->path) : g_path_get_dirname(t->path);
    g_autofree gchar *basename = t->receive ? g_strdup("received.bin") : g_path_get_basename(t->path);
    g_subprocess_launcher_set_cwd(launcher, directory);
    const char *protocol[] = {"--xmodem", "--ymodem", "--zmodem"};
    const char *argv[] = {t->program, protocol[t->protocol], "--binary", "--restricted", "--protect",
        t->receive && t->protocol == 0 ? "--with-crc" : "--verbose", "--", basename, NULL};
    if (t->receive && t->protocol != 0) argv[7] = NULL;
    t->process = g_subprocess_launcher_spawnv(launcher, argv, &error);
    if (!t->process) { status(t, error->message); finish(t); }
    else {
        status(t, "Transfer running; start the matching protocol on the peer");
        read_stderr(t);
        g_subprocess_wait_async(t->process, NULL, exited, ref(t));
    }
    unref(t);
}
TioTransfer *tio_transfer_start(const char *socket_path, const char *path,
                              guint protocol, gboolean receive, guint timeout_seconds,
                              GError **error)
{
    if (protocol > 2 || !path || !g_path_is_absolute(path) || !timeout_seconds || timeout_seconds > 86400) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid transfer options"); return NULL;
    }
    GStatBuf info;
    if (g_stat(path, &info) != 0 || (receive ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a regular send file or receive directory"); return NULL;
    }
    if (receive) {
        g_autoptr(GDir) dir = g_dir_open(path, 0, error);
        if (!dir) return NULL;
        if (g_dir_read_name(dir)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Choose an empty receive directory to protect existing files"); return NULL;
        }
    }
    g_autofree gchar *program = g_find_program_in_path(receive ? "rz" : "sz");
    if (!program) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Install lrzsz to use file transfers"); return NULL; }
    TioTransfer *t = g_new0(TioTransfer, 1);
    t->refs = 1; t->active = TRUE; t->protocol = protocol; t->receive = receive;
    t->path = g_strdup(path); t->program = g_steal_pointer(&program);
    t->cancel = g_cancellable_new(); status(t, "Connecting transfer stream…");
    t->timer = g_timeout_add_seconds(timeout_seconds, timeout, t);
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_socket_client_set_timeout(client, 5);
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
    g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(address), t->cancel, connected, ref(t));
    return t;
}
gboolean tio_transfer_active(const TioTransfer *t) { return t && t->active; }
const char *tio_transfer_status(const TioTransfer *t) { return t ? t->status : ""; }
gboolean tio_transfer_success(const TioTransfer *t) { return t && t->success; }
