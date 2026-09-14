/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#include "native_serial.h"
#include <gio/gio.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CFNumber.h>
#include <IOKit/serial/IOSerialKeys.h>
#endif
#endif

#define QUEUE_LIMIT (8 * 1024 * 1024)
#define SEND_LIMIT (1024 * 1024)
typedef struct { GBytes *bytes; guint line; gboolean high; guint pulse_ms; gint64 pulse_due; } Command;
struct _TioNativeSerial {
    TioNativeConfig config;
    gchar *device, *device_id;
    GRegex *exclude;
    GStrv initial_devices;
    GThread *thread;
    GAsyncQueue *events, *commands;
    GMutex mutex;
    gboolean stop, connected;
    gsize queued, sending;
#ifdef G_OS_WIN32
    HANDLE port;
#else
    int port;
    gboolean exclusive;
#endif
};
static void command_free(Command *c) { if (c) { g_clear_pointer(&c->bytes, g_bytes_unref); g_free(c); } }
void tio_native_event_free(TioNativeEvent *e) { if (e) { g_clear_pointer(&e->bytes, g_bytes_unref); g_free(e->message); g_free(e->device); g_free(e); } }
static gchar *os_error(void)
{
#ifdef G_OS_WIN32
    return g_win32_error_message(GetLastError());
#else
    return g_strdup(g_strerror(errno));
#endif
}
static gboolean stopped(TioNativeSerial *s) { g_mutex_lock(&s->mutex); gboolean stop = s->stop; g_mutex_unlock(&s->mutex); return stop; }
static void status(TioNativeSerial *s, gboolean connected, const char *message)
{
    g_mutex_lock(&s->mutex); s->connected = connected; g_mutex_unlock(&s->mutex);
    TioNativeEvent *e = g_new0(TioNativeEvent, 1);
    e->kind = TIO_NATIVE_STATUS; e->connected = connected; e->message = g_strdup(message); e->device = g_strdup(s->device);
    g_async_queue_push(s->events, e);
}
static gboolean emit_bytes(TioNativeSerial *s, TioNativeEventKind kind, const guint8 *data, gsize length)
{
    g_mutex_lock(&s->mutex);
    gboolean room = s->queued + length <= QUEUE_LIMIT;
    if (room) s->queued += length;
    g_mutex_unlock(&s->mutex);
    if (!room) return FALSE;
    TioNativeEvent *e = g_new0(TioNativeEvent, 1);
    e->kind = kind; e->bytes = g_bytes_new(data, length);
    g_async_queue_push(s->events, e); return TRUE;
}
static void close_port(TioNativeSerial *s)
{
#ifdef G_OS_WIN32
    if (s->port != INVALID_HANDLE_VALUE) CloseHandle(s->port);
    s->port = INVALID_HANDLE_VALUE;
#else
    if (s->port >= 0) { if (s->exclusive) ioctl(s->port, TIOCNXCL); close(s->port); }
    s->exclusive = FALSE;
    s->port = -1;
#endif
}
#ifndef G_OS_WIN32
static speed_t standard_speed(guint baud)
{
    switch (baud) {
    case 1200: return B1200; case 2400: return B2400; case 4800: return B4800;
    case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
    case 57600: return B57600; case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
    default: return 0;
    }
}
#endif
static gboolean open_port(TioNativeSerial *s)
{
    const TioNativeConfig *c = &s->config;
#ifdef G_OS_WIN32
    g_autofree gchar *path = g_strdup_printf("\\\\.\\%s", s->device);
    s->port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (s->port == INVALID_HANDLE_VALUE) return FALSE;
    DCB dcb = {0}; dcb.DCBlength = sizeof dcb;
    if (!GetCommState(s->port, &dcb)) goto failed;
    dcb.BaudRate = c->baud; dcb.ByteSize = (BYTE)c->bits;
    dcb.Parity = c->parity == 1 ? ODDPARITY : c->parity == 2 ? EVENPARITY : NOPARITY;
    dcb.StopBits = c->stops == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fParity = c->parity != 0;
    dcb.fOutxCtsFlow = c->flow == 1; dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fDsrSensitivity = FALSE;
    dcb.fRtsControl = c->flow == 1 ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fOutX = dcb.fInX = c->flow == 2;
    dcb.fTXContinueOnXoff = FALSE; dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE; dcb.fAbortOnError = FALSE;
    dcb.XonChar = 0x11; dcb.XoffChar = 0x13; dcb.XonLim = 128; dcb.XoffLim = 128;
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.WriteTotalTimeoutConstant = 100;
    if (!SetCommState(s->port, &dcb) || !SetCommTimeouts(s->port, &timeouts)) goto failed;
#else
    s->port = open(s->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (s->port < 0) return FALSE;
    fcntl(s->port, F_SETFD, FD_CLOEXEC);
    if (flock(s->port, LOCK_EX | LOCK_NB) < 0 || ioctl(s->port, TIOCEXCL) < 0) goto failed;
    s->exclusive = TRUE;
    struct termios t;
    if (tcgetattr(s->port, &t) < 0) goto failed;
    cfmakeraw(&t);
    /* cfmakeraw does not clear every inherited flow/parity flag on Unix. */
    t.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY | INPCK | IGNPAR);
    t.c_cflag = (t.c_cflag & ~(tcflag_t)(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS)) | CLOCAL | CREAD;
    t.c_cflag |= c->bits == 5 ? CS5 : c->bits == 6 ? CS6 : c->bits == 7 ? CS7 : CS8;
    if (c->stops == 2) t.c_cflag |= CSTOPB;
    if (c->parity) { t.c_cflag |= PARENB; if (c->parity == 1) t.c_cflag |= PARODD; t.c_iflag |= INPCK; }
    if (c->flow == 1) t.c_cflag |= CRTSCTS;
    if (c->flow == 2) { t.c_iflag |= IXON | IXOFF; t.c_cc[VSTART] = 0x11; t.c_cc[VSTOP] = 0x13; }
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    speed_t speed = standard_speed(c->baud);
#ifdef __APPLE__
    if (!speed) speed = B9600;
#else
    if (!speed) { errno = EINVAL; goto failed; }
#endif
    if (cfsetispeed(&t, speed) < 0 || cfsetospeed(&t, speed) < 0 || tcsetattr(s->port, TCSANOW, &t) < 0) goto failed;
#ifdef __APPLE__
    speed_t actual = c->baud;
    if (ioctl(s->port, IOSSIOSPEED, &actual) < 0 && !standard_speed(c->baud)) goto failed;
#endif
#endif
    return TRUE;
failed: {
#ifdef G_OS_WIN32
    DWORD saved = GetLastError(); close_port(s); SetLastError(saved);
#else
    int saved = errno; close_port(s); errno = saved;
#endif
    return FALSE;
    }
}
static gssize read_port(TioNativeSerial *s, guint8 *data, gsize capacity)
{
#ifdef G_OS_WIN32
    DWORD errors, count; COMSTAT state;
    if (!ClearCommError(s->port, &errors, &state)) return -1;
    if (!ReadFile(s->port, data, (DWORD)capacity, &count, NULL)) return -1;
    return count;
#else
    struct pollfd fd = {s->port, POLLIN, 0};
    int ready = poll(&fd, 1, 0);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; return -1; }
    if (!(fd.revents & POLLIN)) return 0;
    ssize_t count = read(s->port, data, capacity);
    return count < 0 && (errno == EAGAIN || errno == EINTR) ? 0 : count;
#endif
}
static gssize write_port(TioNativeSerial *s, const guint8 *data, gsize length)
{
#ifdef G_OS_WIN32
    DWORD written = 0;
    if (!WriteFile(s->port, data, (DWORD)length, &written, NULL)) return -1;
    return written;
#else
    ssize_t count = write(s->port, data, length);
    return count < 0 && (errno == EAGAIN || errno == EINTR) ? 0 : count;
#endif
}
static gboolean set_line(TioNativeSerial *s, guint line, gboolean high)
{
    if (line == 2) {
#ifdef G_OS_WIN32
        if (!SetCommBreak(s->port)) return FALSE;
#else
        if (ioctl(s->port, TIOCSBRK) < 0) return FALSE;
#endif
        for (guint i = 0; i < 25 && !stopped(s); i++) g_usleep(10000);
#ifdef G_OS_WIN32
        return ClearCommBreak(s->port);
#else
        return ioctl(s->port, TIOCCBRK) == 0;
#endif
    }
#ifdef G_OS_WIN32
    return EscapeCommFunction(s->port, line == 0 ? (high ? SETDTR : CLRDTR) : (high ? SETRTS : CLRRTS));
#else
    int bit = line == 0 ? TIOCM_DTR : TIOCM_RTS;
    return ioctl(s->port, high ? TIOCMBIS : TIOCMBIC, &bit) == 0;
#endif
}
static void discard_commands(TioNativeSerial *s)
{
    Command *c;
    while ((c = g_async_queue_try_pop(s->commands))) command_free(c);
    g_mutex_lock(&s->mutex); s->sending = 0; g_mutex_unlock(&s->mutex);
}
static gboolean select_device(TioNativeSerial *s)
{
    if (!s->config.auto_connect && !s->device_id) return TRUE;
    g_auto(GStrv) devices = tio_native_devices();
    gchar *selected = NULL; gint64 latest = G_MININT64; guint matches = 0;
    for (guint i = 0; devices[i]; i++) {
        if (s->exclude && g_regex_match(s->exclude, devices[i], 0, NULL)) continue;
        if (s->device_id) {
            g_autofree gchar *identity = tio_native_device_identity(devices[i]);
            if (g_strcmp0(identity, s->device_id)) continue;
            ++matches; g_free(selected); selected = g_strdup(devices[i]); continue;
        }
        if (s->config.auto_connect == 1 && g_strv_contains((const gchar *const *)s->initial_devices, devices[i])) continue;
        GFile *file = g_file_new_for_path(devices[i]);
        GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, 0, NULL, NULL);
        gint64 modified = info ? (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_TIME_SPAN_SECOND
            + g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) : 0;
        g_clear_object(&info); g_object_unref(file);
        if (modified >= latest) { latest = modified; g_free(selected); selected = g_strdup(devices[i]); }
    }
    if (!selected || (s->device_id && matches != 1)) { g_free(selected); return FALSE; }
    g_free(s->device); s->device = selected; s->config.device = selected; s->config.auto_connect = 0;
    if (!s->device_id) s->device_id = tio_native_device_identity(selected);
    return TRUE;
}
static gpointer worker(gpointer data)
{
    TioNativeSerial *s = data;
    gint64 last_retry = 0;
    while (!stopped(s)) {
        if (g_get_monotonic_time() - last_retry < G_TIME_SPAN_SECOND) { g_usleep(10000); continue; }
        last_retry = g_get_monotonic_time();
        if (!select_device(s)) {
            status(s, FALSE, "Waiting for matching serial device");
            if (!s->config.reconnect && !s->config.auto_connect) break;
            continue;
        }
        if (!open_port(s)) {
            g_autofree gchar *error = os_error();
            status(s, FALSE, error);
            if (!s->config.reconnect) break;
            continue;
        }
        if ((s->config.dtr_default && !set_line(s, 0, s->config.dtr_default == 2)) ||
            (s->config.rts_default && s->config.flow != 1 && !set_line(s, 1, s->config.rts_default == 2))) {
            g_autofree gchar *error = os_error(); status(s, FALSE, error); close_port(s);
            if (!s->config.reconnect) break;
            continue;
        }
        status(s, TRUE, "Connected");
        Command *pending = NULL; gsize offset = 0;
        gint64 write_deadline = 0, write_due = 0;
        gboolean overflow = FALSE;
        while (!stopped(s)) {
            guint8 buffer[16384];
            gssize count = read_port(s, buffer, sizeof buffer);
            if (count < 0) break;
            if (count && !emit_bytes(s, TIO_NATIVE_RX, buffer, (gsize)count)) { overflow = TRUE; break; }
            if (!pending) { pending = g_async_queue_try_pop(s->commands); offset = 0; write_deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND; }
            if (pending) {
                if (!pending->bytes) {
                    if (pending->pulse_ms) {
                        if (!pending->pulse_due) {
                            if (!set_line(s, pending->line, TRUE)) {
                                g_autofree gchar *error = os_error(); status(s, TRUE, error);
                                pending->pulse_due = 1;
                            } else pending->pulse_due = g_get_monotonic_time() + (gint64)pending->pulse_ms * 1000;
                        }
                        if (g_get_monotonic_time() < pending->pulse_due) { g_usleep(1000); continue; }
                        if (!set_line(s, pending->line, FALSE)) { g_autofree gchar *error = os_error(); status(s, TRUE, error); }
                    } else if (!set_line(s, pending->line, pending->high)) {
                        g_autofree gchar *error = os_error(); status(s, TRUE, error);
                    }
                    command_free(pending); pending = NULL;
                    g_mutex_lock(&s->mutex); s->sending--; g_mutex_unlock(&s->mutex);
                } else {
                    gsize length; const guint8 *bytes = g_bytes_get_data(pending->bytes, &length);
                    if (g_get_monotonic_time() < write_due) { g_usleep(1000); continue; }
                    count = write_port(s, bytes + offset, MIN(length - offset,
                        s->config.output_delay || s->config.output_line_delay ? 1 : 256));
                    if (count < 0) break;
                    if (count) {
                        if (!emit_bytes(s, TIO_NATIVE_TX, bytes + offset, (gsize)count)) { overflow = TRUE; break; }
                        offset += (gsize)count;
                        guint delay = s->config.output_delay;
                        if (bytes[offset - 1] == '\n') delay += s->config.output_line_delay;
                        write_due = g_get_monotonic_time() + (gint64)delay * 1000;
                        write_deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
                    } else if (g_get_monotonic_time() > write_deadline) {
#ifdef G_OS_WIN32
                        SetLastError(ERROR_TIMEOUT);
#else
                        errno = ETIMEDOUT;
#endif
                        break;
                    }
                    if (offset == length) {
                        g_mutex_lock(&s->mutex); s->sending -= length; g_mutex_unlock(&s->mutex);
                        command_free(pending); pending = NULL;
                    }
                }
            }
            if (!count) g_usleep(10000);
        }
        g_autofree gchar *error = overflow ? g_strdup("Receive queue full; disconnected to avoid silently losing data") : os_error();
        status(s, FALSE, stopped(s) ? "Disconnected" : error);
        if (pending && pending->pulse_due) set_line(s, pending->line, FALSE);
        close_port(s); command_free(pending); discard_commands(s);
        if (!s->config.reconnect || overflow) break;
    }
    TioNativeEvent *done = g_new0(TioNativeEvent, 1); done->kind = TIO_NATIVE_DONE; g_async_queue_push(s->events, done);
    return NULL;
}
TioNativeSerial *tio_native_start(const TioNativeConfig *c, GError **error)
{
    if (!c || !c->device || (!*c->device && !c->auto_connect) || !c->baud || c->baud > 12000000 || c->bits < 5 || c->bits > 8 || c->stops < 1 || c->stops > 2 || c->parity > 2 || c->flow > 2 || c->dtr_default > 2 || c->rts_default > 2 || c->output_delay > 60000 || c->output_line_delay > 60000 || c->auto_connect > 2) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid serial configuration"); return NULL;
    }
#ifdef G_OS_WIN32
    if (g_ascii_strncasecmp(c->device, "COM", 3) || !g_ascii_isdigit(c->device[3])) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Enter a COM port, for example COM3"); return NULL;
    }
    for (const char *p = c->device + 3; *p; p++) if (!g_ascii_isdigit(*p)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid COM port"); return NULL;
    }
#endif
    TioNativeSerial *s = g_new0(TioNativeSerial, 1);
    s->config = *c; s->device = g_strdup(c->device); s->config.device = s->device;
    s->initial_devices = tio_native_devices();
    if (c->exclude_devices && *c->exclude_devices) {
        s->exclude = g_regex_new(c->exclude_devices, G_REGEX_OPTIMIZE, 0, error);
        if (!s->exclude) { g_strfreev(s->initial_devices); g_free(s->device); g_free(s); return NULL; }
    }
    if (!c->auto_connect) s->device_id = c->device_id && g_str_has_prefix(c->device_id, "macos-usb:")
        ? g_strdup(c->device_id) : tio_native_device_identity(c->device);

    s->events = g_async_queue_new(); s->commands = g_async_queue_new(); g_mutex_init(&s->mutex);
#ifdef G_OS_WIN32
    s->port = INVALID_HANDLE_VALUE;
#else
    s->port = -1;
#endif
    s->thread = g_thread_new("serial", worker, s); return s;
}
gboolean tio_native_send(TioNativeSerial *s, GBytes *bytes, GError **error)
{
    gsize size = g_bytes_get_size(bytes);
    if (!size) return TRUE;
    if (s) {
        g_mutex_lock(&s->mutex);
        if (s->connected && size <= SEND_LIMIT && s->sending + size <= SEND_LIMIT) {
            s->sending += size;
            Command *c = g_new0(Command, 1); c->bytes = g_bytes_ref(bytes);
            g_async_queue_push(s->commands, c); g_mutex_unlock(&s->mutex); return TRUE;
        }
        g_mutex_unlock(&s->mutex);
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Not connected or send queue full"); return FALSE;
}
gboolean tio_native_line(TioNativeSerial *s, guint line, gboolean high, GError **error)
{
    if (s && line <= 2 && !(line == 1 && s->config.flow == 1)) {
        g_mutex_lock(&s->mutex);
        if (s->connected && s->sending < SEND_LIMIT) {
            Command *c = g_new0(Command, 1); c->line = line; c->high = high;
            s->sending++; g_async_queue_push(s->commands, c); g_mutex_unlock(&s->mutex); return TRUE;
        }
        g_mutex_unlock(&s->mutex);
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Connect first; manual RTS is unavailable with RTS/CTS flow control"); return FALSE;
}
TioNativeEvent *tio_native_poll(TioNativeSerial *s)
{
    TioNativeEvent *e = g_async_queue_try_pop(s->events);
    if (e && e->bytes) { g_mutex_lock(&s->mutex); s->queued -= g_bytes_get_size(e->bytes); g_mutex_unlock(&s->mutex); }
    return e;
}
void tio_native_finish(TioNativeSerial *s)
{
    if (!s) return;
    g_mutex_lock(&s->mutex); s->stop = TRUE; g_mutex_unlock(&s->mutex);
    if (s->thread) { g_thread_join(s->thread); s->thread = NULL; }
}
gboolean tio_native_pending(TioNativeSerial *s) { return g_async_queue_length(s->events) > 0; }
void tio_native_stop(TioNativeSerial *s)
{
    if (!s) return;
    tio_native_finish(s); close_port(s); discard_commands(s);
    TioNativeEvent *e; while ((e = tio_native_poll(s))) tio_native_event_free(e);
    g_async_queue_unref(s->events); g_async_queue_unref(s->commands); g_mutex_clear(&s->mutex); g_free(s->device); g_free(s->device_id); g_clear_pointer(&s->exclude, g_regex_unref); g_strfreev(s->initial_devices); g_free(s);
}
static gint compare_names(gconstpointer a, gconstpointer b) { return g_strcmp0(*(char *const *)a, *(char *const *)b); }
GStrv tio_native_devices(void)
{
    GPtrArray *list = g_ptr_array_new();
#ifdef G_OS_WIN32
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        for (DWORD index = 0;; index++) {
            char name[512], value[512]; DWORD namesize = sizeof name, size = sizeof value, type;
            LONG result = RegEnumValueA(key, index, name, &namesize, NULL, &type, (BYTE *)value, &size);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result == ERROR_SUCCESS && type == REG_SZ && size > 0 && size <= sizeof value) { value[sizeof value - 1] = 0; g_ptr_array_add(list, g_strdup(value)); }
            else if (result != ERROR_MORE_DATA) break;
        }
        RegCloseKey(key);
    }
#else
#ifdef __APPLE__
    const char *patterns[] = {"/dev/cu.*", NULL};
#else
    const char *patterns[] = {"/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS*", NULL};
#endif
    for (guint i = 0; patterns[i]; i++) {
        glob_t paths = {0};
        if (!glob(patterns[i], 0, NULL, &paths)) for (gsize j = 0; j < paths.gl_pathc; j++) g_ptr_array_add(list, g_strdup(paths.gl_pathv[j]));
        globfree(&paths);
    }
#endif
    g_ptr_array_sort(list, compare_names); g_ptr_array_add(list, NULL);
    return (GStrv)g_ptr_array_free(list, FALSE);
}

gboolean tio_native_idle(TioNativeSerial *s)
{
    if (!s) return FALSE;
    g_mutex_lock(&s->mutex); gboolean idle = s->connected && !s->sending; g_mutex_unlock(&s->mutex);
    return idle;
}
gboolean tio_native_pulse(TioNativeSerial *s, guint line, guint milliseconds, GError **error)
{
    if (s && line <= 1 && milliseconds >= 1 && milliseconds <= 10000 && !(line == 1 && s->config.flow == 1)) {
        g_mutex_lock(&s->mutex);
        if (s->connected && s->sending < SEND_LIMIT) {
            Command *c = g_new0(Command, 1); c->line = line; c->high = TRUE; c->pulse_ms = milliseconds;
            s->sending++; g_async_queue_push(s->commands, c); g_mutex_unlock(&s->mutex); return TRUE;
        }
        g_mutex_unlock(&s->mutex);
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Connect first; manual RTS is unavailable with RTS/CTS flow control"); return FALSE;
}

#ifdef __APPLE__
static CFTypeRef device_property(io_registry_entry_t service, const char *name)
{
    CFStringRef key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    CFTypeRef value = IORegistryEntrySearchCFProperty(service, kIOServicePlane, key, NULL,
        kIORegistryIterateParents | kIORegistryIterateRecursively); CFRelease(key); return value;
}
static guint device_number(io_registry_entry_t service, const char *name)
{
    guint value = 0; CFTypeRef property = device_property(service, name);
    if (property && CFGetTypeID(property) == CFNumberGetTypeID()) CFNumberGetValue(property, kCFNumberIntType, &value);
    if (property) CFRelease(property); return value;
}
static gchar *service_identity(io_registry_entry_t service)
{
    CFTypeRef property = device_property(service, "USB Serial Number"); char serial[1024] = {0};
    if (property && CFGetTypeID(property) == CFStringGetTypeID()) CFStringGetCString(property, serial, sizeof serial, kCFStringEncodingUTF8);
    if (property) CFRelease(property);
    if (!*serial) return NULL;
    return g_strdup_printf("macos-usb:%04x:%04x:%u:%s", device_number(service, "idVendor"),
        device_number(service, "idProduct"), device_number(service, "bInterfaceNumber"), serial);
}
#endif
gchar *tio_native_device_identity(const char *device)
{
#ifdef __APPLE__
    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOSerialBSDServiceValue), &iterator) != KERN_SUCCESS) return NULL;
    gchar *identity = NULL; io_object_t service;
    while ((service = IOIteratorNext(iterator))) {
        CFTypeRef property = IORegistryEntryCreateCFProperty(service, CFSTR(kIOCalloutDeviceKey), NULL, 0);
        char path[1024] = {0};
        if (property && CFGetTypeID(property) == CFStringGetTypeID()) CFStringGetCString(property, path, sizeof path, kCFStringEncodingUTF8);
        if (property) CFRelease(property);
        if (!g_strcmp0(device, path)) identity = service_identity(service);
        IOObjectRelease(service); if (identity) break;
    }
    IOObjectRelease(iterator); return identity;
#else
    (void)device; return NULL;
#endif
}
