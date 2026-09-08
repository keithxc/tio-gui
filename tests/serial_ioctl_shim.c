/* SPDX-License-Identifier: GPL-3.0-only */
/* Test-only virtual modem lines. Loaded ONLY into the isolated PTY's tio. */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/serial.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
static int lines;
static struct serial_rs485 rs485;
int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *argument = va_arg(args, void *);
    va_end(args);
    const char *log = getenv("TIO_TEST_IOCTL_LOG");
    if (log && (request == TIOCMGET || request == TIOCMSET || request == TIOCGRS485 || request == TIOCSRS485)) {
        if (request == TIOCMGET) *(int *)argument = lines;
        if (request == TIOCMSET) lines = *(int *)argument;
        if (request == TIOCGRS485) memcpy(argument, &rs485, sizeof rs485);
        if (request == TIOCSRS485) memcpy(&rs485, argument, sizeof rs485);
        FILE *file = fopen(log, "a");
        if (file) {
            fprintf(file, "%lu %d %u %u %u\n", request, lines, rs485.flags,
                    rs485.delay_rts_before_send, rs485.delay_rts_after_send);
            fclose(file);
        }
        return 0;
    }
    return (int)syscall(SYS_ioctl, fd, request, argument);
}
