/* SPDX-License-Identifier: GPL-3.0-only */
#include "capture.h"
#include <glib/gstdio.h>
#include <string.h>
static void pump(guint ms)
{
    gint64 until = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
}
static void finish(TioCapture *capture)
{
    tio_capture_stop(capture);
    for (guint i = 0; i < 500 && !tio_capture_finished(capture); ++i) pump(10);
    g_assert_true(tio_capture_finished(capture));
}
static void cleanup(const char *directory)
{
    g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) { g_autofree gchar *path = g_build_filename(directory, name, NULL); g_unlink(path); }
    g_rmdir(directory);
}
typedef struct { guint count; guint8 bytes[256]; } Sink;
static void replayed(TioCaptureKind kind, const guint8 *data, gsize length, gint64 time, gpointer user_data)
{
    Sink *sink = user_data;
    g_assert_cmpuint(kind, ==, sink->count ? TIO_CAPTURE_TX : TIO_CAPTURE_RX);
    g_assert_cmpmem(data, length, sink->bytes, sizeof sink->bytes);
    g_assert_cmpint(time, ==, sink->count ? 1100000 : 1000000);
    ++sink->count;
}
static void roundtrip(void)
{
    g_autofree gchar *directory = g_dir_make_tmp("tio-capture-test-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(directory, "session.tiocap", NULL);
    TioCapture *capture = tio_capture_new(path, 4096, 0, 3, 12288, "115200 8N1", NULL);
    g_assert_nonnull(capture);
    Sink sink = {0};
    for (guint i = 0; i < 256; ++i) sink.bytes[i] = (guint8)i;
    g_assert_true(tio_capture_record(capture, TIO_CAPTURE_RX, sink.bytes, 256, 1000000));
    g_assert_true(tio_capture_record(capture, TIO_CAPTURE_TX, sink.bytes, 256, 1100000));
    finish(capture);
    g_assert_null(tio_capture_error(capture));
    tio_capture_unref(capture);
    g_autoptr(GError) error = NULL;
    TioReplay *replay = tio_replay_new(path, replayed, &sink, &error);
    g_assert_no_error(error); g_assert_nonnull(replay);
    g_assert_cmpuint(tio_replay_count(replay), ==, 2);
    tio_replay_pause(replay, TRUE); pump(40); g_assert_cmpuint(sink.count, ==, 0);
    tio_replay_speed(replay, 10); tio_replay_pause(replay, FALSE);
    pump(100); g_assert_cmpuint(sink.count, ==, 2); g_assert_true(tio_replay_finished(replay));
    tio_replay_free(replay);
    g_assert_null(tio_capture_new(path, 4096, 0, 3, 12288, "", NULL));
    cleanup(directory);
}
static void rotation(void)
{
    g_autofree gchar *directory = g_dir_make_tmp("tio-rotation-test-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(directory, "session.tiocap", NULL);
    g_autofree gchar *unrelated = g_strconcat(path, ".part9999", NULL);
    g_assert_true(g_file_set_contents(unrelated, "unrelated", -1, NULL));
    TioCapture *capture = tio_capture_new(path, 1024, 0, 2, 2048, "", NULL);
    guint8 bytes[256] = {0};
    for (guint i = 0; i < 20; ++i) g_assert_true(tio_capture_record(capture, TIO_CAPTURE_RX, bytes, sizeof bytes, i));
    finish(capture); g_assert_null(tio_capture_error(capture));
    g_assert_true(g_file_test(tio_capture_path(capture), G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(unrelated, G_FILE_TEST_EXISTS));
    g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
    guint count = 0; const char *name; guint64 total = 0;
    while ((name = g_dir_read_name(dir))) {
        if (g_str_has_suffix(name, "9999")) continue;
        g_autofree gchar *file = g_build_filename(directory, name, NULL);
        GStatBuf info; g_stat(file, &info); total += (guint64)info.st_size; ++count;
    }
    g_assert_cmpuint(count, <=, 2); g_assert_cmpuint(total, <=, 2048);
    tio_capture_unref(capture); cleanup(directory);
}
static void failures(void)
{
    g_autofree gchar *directory = g_dir_make_tmp("tio-capture-fail-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(directory, "queue.tiocap", NULL);
    TioCapture *capture = tio_capture_new(path, 1024 * 1024, 0, 2, 2097152, "", NULL);
    guint8 bytes[65536] = {0};
    guint accepted = 0;
    while (tio_capture_record(capture, TIO_CAPTURE_RX, bytes, sizeof bytes, 0)) ++accepted;
    g_assert_cmpuint(accepted, <, 130);
    finish(capture); g_assert_nonnull(tio_capture_error(capture));
    tio_capture_unref(capture);
    g_autofree gchar *rotating = g_build_filename(directory, "time.tiocap", NULL);
    capture = tio_capture_new(rotating, 4096, 1, 2, 8192, "", NULL);
    g_assert_true(tio_capture_record(capture, TIO_CAPTURE_RX, bytes, 1, 0));
    pump(1100);
    g_assert_true(tio_capture_record(capture, TIO_CAPTURE_RX, bytes, 1, 1));
    finish(capture); g_assert_null(tio_capture_error(capture));
    g_assert_true(g_str_has_suffix(tio_capture_path(capture), ".part0001"));
    tio_capture_unref(capture);
    g_autofree gchar *collision = g_build_filename(directory, "collision.tiocap", NULL);
    g_autofree gchar *next = g_strconcat(collision, ".part0001", NULL);
    g_assert_true(g_file_set_contents(next, "do not overwrite", -1, NULL));
    capture = tio_capture_new(collision, 1024, 0, 2, 2048, "", NULL);
    g_assert_true(tio_capture_record(capture, TIO_CAPTURE_RX, bytes, 2048, 0));
    finish(capture); g_assert_nonnull(tio_capture_error(capture));
    g_autofree gchar *preserved = NULL;
    g_assert_true(g_file_get_contents(next, &preserved, NULL, NULL));
    g_assert_cmpstr(preserved, ==, "do not overwrite");
    tio_capture_unref(capture);
    cleanup(directory);
}
static void malformed(void)
{
    g_autofree gchar *directory = g_dir_make_tmp("tio-replay-bad-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(directory, "bad.tiocap", NULL);
    const char *samples[] = {"", "{}\n", "{\"format\":\"tio-gui-capture\",\"version\":1}\n{\"t\":0,\"kind\":0,\"data\":\"!!!!\"}\n",
        "{\"format\":\"tio-gui-capture\",\"version\":1}\n{\"t\":0,\"kind\":0,\"data\":\"AAAA\"}"};
    for (guint i = 0; i < G_N_ELEMENTS(samples); ++i) {
        g_assert_true(g_file_set_contents(path, samples[i], -1, NULL));
        g_assert_null(tio_replay_new(path, replayed, NULL, NULL));
    }
    cleanup(directory);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/capture/exact-bytes-replay-pause-speed", roundtrip);
    g_test_add_func("/capture/rotation-retention", rotation);
    g_test_add_func("/capture/malformed", malformed);
    g_test_add_func("/capture/overflow-time-rotation-collision", failures);
    return g_test_run();
}
