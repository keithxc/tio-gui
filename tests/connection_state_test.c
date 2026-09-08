/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 600
#include "connection_state.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
static void descriptor_lifecycle(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    g_assert_cmpint(master, >=, 0);
    g_assert_cmpint(grantpt(master), ==, 0);
    g_assert_cmpint(unlockpt(master), ==, 0);
    g_autofree gchar *path = g_strdup(ptsname(master));
    g_autofree gchar *before = tio_connection_device(getpid(), path);
    g_assert_null(before);
    int slave = open(path, O_RDWR | O_NOCTTY);
    g_assert_cmpint(slave, >=, 0);
    g_autofree gchar *connected = tio_connection_device(getpid(), path);
    g_assert_cmpstr(connected, ==, path);
    close(slave);
    g_autofree gchar *after = tio_connection_device(getpid(), path);
    g_assert_null(after);
    close(master);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/connection/descriptor-lifecycle", descriptor_lifecycle);
    return g_test_run();
}
