/* SPDX-License-Identifier: GPL-3.0-only */
#include "modbus.h"
#include <string.h>
static void codec(void)
{
    TioModbusRequest request = {.unit=1,.function=3,.quantity=10};
    g_autoptr(GBytes) frame = tio_modbus_frame(&request, NULL);
    const guint8 expected[] = {1,3,0,0,0,10,0xc5,0xcd};
    gsize length; const guint8 *bytes = g_bytes_get_data(frame, &length);
    g_assert_cmpmem(bytes, length, expected, sizeof expected);
    request.tcp=TRUE; request.transaction=7; request.quantity=2;
    guint8 response[] = {0,7,0,0,0,7,1,3,4,0x12,0x34,0xab,0xcd};
    g_autoptr(GBytes) reply = g_bytes_new(response, sizeof response);
    g_autofree gchar *text = tio_modbus_decode(&request, reply, NULL);
    g_assert_nonnull(strstr(text, "4660")); g_assert_nonnull(strstr(text, "43981"));
    response[1]=8; g_autoptr(GBytes) wrong = g_bytes_new(response, sizeof response);
    g_assert_null(tio_modbus_decode(&request, wrong, NULL));
    guint8 exception[] = {0,7,0,0,0,3,1,0x83,2};
    g_autoptr(GBytes) ex = g_bytes_new(exception, sizeof exception); g_autoptr(GError) error = NULL;
    g_assert_null(tio_modbus_decode(&request, ex, &error)); g_assert_nonnull(strstr(error->message, "exception 0x02"));
    request.address=65535; request.quantity=2; g_assert_null(tio_modbus_frame(&request, NULL));
    request.function=5; request.value=2; g_assert_null(tio_modbus_frame(&request, NULL));
    request.value=1; g_autoptr(GBytes) write = tio_modbus_frame(&request, NULL);
    g_autofree gchar *ack = tio_modbus_decode(&request, write, NULL); g_assert_nonnull(strstr(ack, "acknowledged"));
}
typedef struct { gboolean done; gchar *text; GError *error; } Result;
static void completed(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; Result *sink = data; sink->text=tio_modbus_request_finish(result,&sink->error);sink->done=TRUE;
}
static void roundtrip(void)
{
    const char *endpoint=g_getenv("TIO_TEST_MODBUS_ENDPOINT");
    if (!endpoint) {g_test_skip("Run modbus_acceptance.py");return;}
    const char *port=g_getenv("TIO_TEST_MODBUS_PORT");
    TioModbusRequest request={.unit=1,.function=3,.quantity=2,.endpoint=endpoint,.tcp=port!=NULL,.port=port?(guint16)atoi(port):0,.transaction=7};
    Result result={0};tio_modbus_request_async(&request,NULL,completed,&result);
    while(!result.done)g_main_context_iteration(NULL,TRUE);
    if (g_getenv("TIO_TEST_EXPECT_ERROR")) { g_assert_nonnull(result.error); g_assert_null(result.text); g_clear_error(&result.error); return; }
    if(result.error)g_test_message("%s",result.error->message);
    g_assert_no_error(result.error);g_assert_nonnull(strstr(result.text,"4660"));g_assert_nonnull(strstr(result.text,"43981"));g_free(result.text);
}
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL);g_test_add_func("/modbus/codec-validation",codec);g_test_add_func("/modbus/roundtrip",roundtrip);return g_test_run();
}
