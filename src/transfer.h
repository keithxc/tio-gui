/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <gio/gio.h>
typedef struct _TioTransfer TioTransfer;
/* Protocol 0=XMODEM-CRC, 1=YMODEM, 2=ZMODEM. Receive path is an empty
 * destination directory; XMODEM writes received.bin. No shell is involved. */
TioTransfer *tio_transfer_start(const char *socket_path, const char *path,
                              guint protocol, gboolean receive, guint timeout_seconds,
                              GError **error);
void tio_transfer_cancel(TioTransfer *transfer);
void tio_transfer_free(TioTransfer *transfer);
gboolean tio_transfer_active(const TioTransfer *transfer);
const char *tio_transfer_status(const TioTransfer *transfer);
gboolean tio_transfer_success(const TioTransfer *transfer);
