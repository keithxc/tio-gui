/* SPDX-License-Identifier: GPL-3.0-only */
#include "plugin.h"
#include <string.h>
typedef struct { gboolean done; gchar *text; GError *error; } Result;
static void completed(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; Result *sink = data; sink->text = tio_plugin_run_finish(result, &sink->error); sink->done = TRUE;
}
static void check(guint language, const char *source, const char *expected)
{
    Result result = {0};
    tio_plugin_run_async(language, source, "temp=42", NULL, completed, &result);
    while (!result.done) g_main_context_iteration(NULL, TRUE);
    if (expected) {
        if (result.error) g_test_message("%s", result.error->message);
        g_assert_no_error(result.error); g_assert_cmpstr(result.text, ==, expected);
    } else { g_assert_nonnull(result.error); g_assert_null(result.text); }
    g_free(result.text); g_clear_error(&result.error);
}
static void transforms(void)
{
    check(0, "function transform(input) return input:upper() end", "TEMP=42");
    check(1, "function transform(input) { return {temperature:Number(input.split('=')[1]),api:TIO_PLUGIN_API}; }", "{\"temperature\":42,\"api\":1}");
    check(0, "function transform(input) return tostring(io) end", "nil");
    check(1, "import('std').then(m=>m.out.puts(m.loadFile('/etc/passwd')===null?'hidden':'exposed')); function transform(){return '';}", "hidden");
    check(1, "import('std').then(m=>{m.out.close();m.err.close();while(true){}}); function transform(){return '';}", NULL);
    check(1, "async function transform(){return 'not synchronous';}", NULL);
    check(1, "function transform(input) { while(true){} }", NULL);
    check(0, "function transform(input) return string.rep('a',70000) end", NULL);
    check(1, "function transform(input) { throw new Error('test failure'); }", NULL);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/plugin/transform-isolation-timeout-output", transforms);
    return g_test_run();
}
