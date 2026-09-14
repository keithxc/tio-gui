/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "plugin.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include <libproc.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#define PLUGIN_LIMIT 65536

typedef struct { guint language; gchar *source, *input; } Job;
static void job_free(gpointer data) { Job *job = data; g_free(job->source); g_free(job->input); g_free(job); }
static void child_limits(gpointer data)
{
    (void)data;
    /* Darwin rejects RLIMIT_AS/DATA reductions; the parent monitors RSS below. */
    struct rlimit cpu = {2, 2}, files = {0, 0}, core = {0, 0};
    if (setrlimit(RLIMIT_CPU, &cpu) || setrlimit(RLIMIT_FSIZE, &files) || setrlimit(RLIMIT_CORE, &core)) _exit(126);
}
static const char lua_wrapper[] =
    "local f=assert(io.open(arg[1],'rb')); local source=f:read('*a'); f:close()\n"
    "f=assert(io.open(arg[2],'rb')); local input=f:read('*a'); f:close()\n"
    "local env={string=string,math=math,table=table,utf8=utf8,tonumber=tonumber,tostring=tostring,type=type,pairs=pairs,ipairs=ipairs,select=select,assert=assert,error=error,pcall=pcall,TIO_PLUGIN_API=1}\n"
    "assert(load(source,'plugin','t',env))()\n"
    "local output=assert(env.transform,'Define transform(input)')(input)\n"
    "assert(type(output)=='string','transform must return a string')\n"
    "io.write(output)\n";
static const char js_wrapper[] =
    "import * as std from 'std';\n"
    "const source=std.loadFile(scriptArgs[1]), input=std.loadFile(scriptArgs[2]);\n"
    "const transform=new Function('TIO_PLUGIN_API',source+'; return transform;')(1);\n"
    "const result=transform(input);\n"
    "if(result && typeof result.then==='function') throw new Error('API 1 requires a synchronous result');\n"
    "std.out.puts(typeof result==='string'?result:JSON.stringify(result));\n";
static void child_waited(GObject *source, GAsyncResult *result, gpointer data)
{
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
    *(gboolean *)data = TRUE;
}
static void worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancel)
{
    (void)source_object; Job *job = task_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *bwrap = g_strdup("/usr/bin/sandbox-exec");
    g_autofree gchar *runtime = g_find_program_in_path(job->language ? "qjs" : "lua");
    if (!bwrap || !runtime) { g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Lua 5.4 / QuickJS runtime is missing from the application"); return; }
    g_autofree gchar *directory = g_dir_make_tmp("tio-plugin-XXXXXX", &error);
    if (!directory) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    char *resolved_directory = realpath(directory, NULL);
    if (resolved_directory) { g_free(directory); directory = resolved_directory; }
    g_autofree gchar *source = g_build_filename(directory, "source", NULL);
    g_autofree gchar *input = g_build_filename(directory, "input", NULL);
    g_autofree gchar *wrapper = g_build_filename(directory, "wrapper", NULL);
    g_autofree gchar *filter_path = g_build_filename(directory, "filter", NULL);
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GString) output = g_string_new(NULL);
    if (!g_file_set_contents(source, job->source, -1, &error) || !g_file_set_contents(input, job->input, -1, &error) ||
        !g_file_set_contents(wrapper, job->language ? js_wrapper : lua_wrapper, -1, &error)) goto done;
    {
        g_autofree gchar *runtime_dir = g_path_get_dirname(runtime);
        g_autofree gchar *bundle_dir = g_canonicalize_filename("..", runtime_dir);
        char *resolved_bundle = realpath(bundle_dir, NULL);
        if (resolved_bundle) { g_free(bundle_dir); bundle_dir = resolved_bundle; }
        /* sandbox parameters quote paths, including spaces, independently of policy text. */
        g_autofree gchar *job_arg = g_strdup_printf("JOB=%s", directory);
        g_autofree gchar *app_arg = g_strdup_printf("APP=%s", bundle_dir);
        const char *profile = "(version 1)(deny default)(allow process-exec)(allow process-info*)(allow sysctl-read)"
            "(allow file-read-metadata)(allow file-read* (literal \"/\") (subpath (param \"JOB\")) (subpath (param \"APP\"))"
            "(subpath \"/System\") (subpath \"/usr/lib\") (literal \"/dev/null\") (literal \"/dev/urandom\"))"
            "(allow file-write-data (literal \"/dev/null\"))";
        g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
        g_subprocess_launcher_set_child_setup(launcher, child_limits, NULL, NULL);
        g_subprocess_launcher_set_cwd(launcher, directory);
        g_autoptr(GString) policy = g_string_new(profile);
        g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
#define ARG(s) g_ptr_array_add(argv, g_strdup(s))
        ARG(bwrap); ARG("-D"); ARG(job_arg); ARG("-D"); ARG(app_arg);
        /* The devShell declares Lua's two external library roots. The shipped
         * bundle ignores this setting: all its code lives beneath APP. Never
         * grant access to the whole Nix store (which can contain user files). */
        if (g_str_has_prefix(runtime, "/nix/store/")) {
            g_auto(GStrv) libraries = g_strsplit(g_getenv("TIO_PLUGIN_NIX_LIBS") ? g_getenv("TIO_PLUGIN_NIX_LIBS") : "", ":", -1);
            for (guint i = 0; libraries[i]; i++) if (g_str_has_prefix(libraries[i], "/nix/store/")) {
                g_autofree gchar *parameter = g_strdup_printf("LIB%u=%s", i, libraries[i]);
                ARG("-D"); ARG(parameter);
                g_string_append_printf(policy, "(allow file-read* (subpath (param \"LIB%u\")))", i);
            }
        }
        ARG("-p"); ARG(policy->str); ARG(runtime);
        if (job->language) ARG("--module");
        ARG(wrapper); ARG(source); ARG(input); g_ptr_array_add(argv, NULL);
#undef ARG
        process = g_subprocess_launcher_spawnv(launcher, (const char * const *)argv->pdata, &error);
    }
    if (!process) goto done;
    {
        GInputStream *stream = g_subprocess_get_stdout_pipe(process);
        g_autoptr(GMainContext) context = g_main_context_new();
        g_main_context_push_thread_default(context);
        gboolean exited = FALSE, eof = FALSE;
        g_subprocess_wait_async(process, NULL, child_waited, &exited);
        gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
        while (TRUE) {
            g_main_context_iteration(context, FALSE);
            if (eof && exited) break;
            struct proc_taskinfo info;
            int pid = atoi(g_subprocess_get_identifier(process) ? g_subprocess_get_identifier(process) : "0");
            if (!exited && pid && proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof info) == sizeof info &&
                info.pti_resident_size > 256 * 1024 * 1024) {
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Plugin exceeded its 256 MiB resident memory limit"); break;
            }
            if (g_cancellable_is_cancelled(cancel) || g_get_monotonic_time() >= deadline) {
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Plugin cancelled or exceeded its 2-second deadline"); break;
            }
            if (eof) { g_usleep(1000); continue; }
            gchar block[4096];
            gssize length = g_pollable_input_stream_read_nonblocking(G_POLLABLE_INPUT_STREAM(stream), block, sizeof block, cancel, &error);
            if (length == 0) { eof = TRUE; continue; }
            if (length < 0) {
                if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) { g_clear_error(&error); g_usleep(1000); continue; }
                break;
            }
            if (output->len + (gsize)length > PLUGIN_LIMIT) {
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Plugin output exceeds 64 KiB"); break;
            }
            g_string_append_len(output, block, length);
        }
        if (error) g_subprocess_force_exit(process);
        while (!exited) g_main_context_iteration(context, TRUE);
        g_main_context_pop_thread_default(context);
        if (!error && !g_subprocess_get_successful(process))
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Plugin failed (wait status %d): %.2048s", g_subprocess_get_status(process), output->str);
        if (!error && (memchr(output->str, 0, output->len) || !g_utf8_validate(output->str, (gssize)output->len, NULL)))
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Plugin output must be UTF-8 text without NUL bytes");
    }
done:
    g_unlink(source); g_unlink(input); g_unlink(wrapper); g_unlink(filter_path); g_rmdir(directory);
    if (error) g_task_return_error(task, g_steal_pointer(&error));
    else g_task_return_pointer(task, g_string_free(g_steal_pointer(&output), FALSE), g_free);
}
void tio_plugin_run_async(guint language, const char *source, const char *input,
                         GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
    g_autoptr(GTask) task = g_task_new(NULL, cancel, callback, data);
    if (language > 1 || !source || !input || strlen(source) > PLUGIN_LIMIT || strlen(input) > PLUGIN_LIMIT ||
        !g_utf8_validate(source, -1, NULL) || !g_utf8_validate(input, -1, NULL)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Plugin source and input must be UTF-8 text up to 64 KiB"); return;
    }
    Job *job = g_new0(Job, 1); *job = (Job){language, g_strdup(source), g_strdup(input)};
    g_task_set_task_data(task, job, job_free); g_task_run_in_thread(task, worker);
}
gchar *tio_plugin_run_finish(GAsyncResult *result, GError **error) { return g_task_propagate_pointer(G_TASK(result), error); }
