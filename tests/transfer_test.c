/* SPDX-License-Identifier: GPL-3.0-only */
#include "transfer.h"
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6) return 2;
    g_autoptr(GError) error = NULL;
    TioTransfer *t = tio_transfer_start(argv[1], argv[2], (guint)atoi(argv[3]), atoi(argv[4]), argc == 6 ? (guint)atoi(argv[5]) : 15, &error);
    if (!t) { g_printerr("%s\n", error->message); return 1; }
    while (tio_transfer_active(t)) g_main_context_iteration(NULL, TRUE);
    gboolean success = tio_transfer_success(t);
    g_print("%s\n", tio_transfer_status(t));
    tio_transfer_free(t);
    return success ? 0 : 1;
}
