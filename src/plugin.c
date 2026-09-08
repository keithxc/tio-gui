/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "plugin.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <sys/syscall.h>
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
    struct rlimit memory = {256 * 1024 * 1024, 256 * 1024 * 1024};
    struct rlimit cpu = {2, 2}, files = {0, 0}, core = {0, 0};
    if (setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_CPU, &cpu) || setrlimit(RLIMIT_FSIZE, &files) || setrlimit(RLIMIT_CORE, &core)) _exit(126);
}
static const char lua_wrapper[] =
    "local f=assert(io.open('/source','rb')); local source=f:read('*a'); f:close()\n"
    "f=assert(io.open('/input','rb')); local input=f:read('*a'); f:close()\n"
    "local env={string=string,math=math,table=table,utf8=utf8,tonumber=tonumber,tostring=tostring,type=type,pairs=pairs,ipairs=ipairs,select=select,assert=assert,error=error,pcall=pcall,TIO_PLUGIN_API=1}\n"
    "assert(load(source,'plugin','t',env))()\n"
    "local output=assert(env.transform,'Define transform(input)')(input)\n"
    "assert(type(output)=='string','transform must return a string')\n"
    "io.write(output)\n";
static const char js_wrapper[] =
    "import * as std from 'std';\n"
    "const source=std.loadFile('/source'), input=std.loadFile('/input');\n"
    "const transform=new Function('TIO_PLUGIN_API',source+'; return transform;')(1);\n"
    "const result=transform(input);\n"
    "if(result && typeof result.then==='function') throw new Error('API 1 requires a synchronous result');\n"
    "std.out.puts(typeof result==='string'?result:JSON.stringify(result));\n";
#define DENY_SYSCALL(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (n), 0, 1), BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO | EPERM)
static void child_waited(GObject *source, GAsyncResult *result, gpointer data)
{
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
    *(gboolean *)data = TRUE;
}
static void worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancel)
{
    (void)source_object; Job *job = task_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *bwrap = g_find_program_in_path("bwrap");
    g_autofree gchar *runtime = g_find_program_in_path(job->language ? "qjs" : "lua");
    if (!bwrap || !runtime) { g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Install bubblewrap and Lua 5.4 / QuickJS for analysis plugins"); return; }
    gchar *resolved_runtime = realpath(runtime, NULL);
    if (resolved_runtime) { g_free(runtime); runtime = resolved_runtime; }
    g_autofree gchar *directory = g_dir_make_tmp("tio-plugin-XXXXXX", &error);
    if (!directory) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    g_autofree gchar *source = g_build_filename(directory, "source", NULL);
    g_autofree gchar *input = g_build_filename(directory, "input", NULL);
    g_autofree gchar *wrapper = g_build_filename(directory, "wrapper", NULL);
    g_autofree gchar *filter_path = g_build_filename(directory, "filter", NULL);
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GString) output = g_string_new(NULL);
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K, 0x40000000, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
        DENY_SYSCALL(__NR_clone), DENY_SYSCALL(__NR_clone3), DENY_SYSCALL(__NR_fork),
        DENY_SYSCALL(__NR_vfork), DENY_SYSCALL(__NR_socket), DENY_SYSCALL(__NR_ptrace),
        DENY_SYSCALL(__NR_unshare), DENY_SYSCALL(__NR_setns), DENY_SYSCALL(__NR_mount), DENY_SYSCALL(__NR_umount2),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
    };
    if (!g_file_set_contents(source, job->source, -1, &error) || !g_file_set_contents(input, job->input, -1, &error) ||
        !g_file_set_contents(wrapper, job->language ? js_wrapper : lua_wrapper, -1, &error) ||
        !g_file_set_contents(filter_path, (const char *)filter, sizeof filter, &error)) goto done;
    {
        int fd = g_open(filter_path, O_RDONLY, 0);
        if (fd < 0) { g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not prepare plugin sandbox"); goto done; }
        g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
        g_subprocess_launcher_set_child_setup(launcher, child_limits, NULL, NULL);
        g_subprocess_launcher_take_fd(launcher, fd, 3);
        g_autoptr(GPtrArray) argv = g_ptr_array_new();
        const char *base[] = {bwrap, "--unshare-all", "--die-with-parent", "--new-session", "--clearenv",
            "--ro-bind-try", "/nix/store", "/nix/store", "--ro-bind-try", "/usr", "/usr",
            "--ro-bind-try", "/bin", "/bin", "--ro-bind-try", "/lib", "/lib",
            "--ro-bind-try", "/lib64", "/lib64", "--ro-bind-try", "/etc/ld.so.cache", "/etc/ld.so.cache",
            "--dev", "/dev", "--proc", "/proc",
            "--tmpfs", "/tmp", "--remount-ro", "/tmp", "--chdir", "/tmp",
            "--ro-bind", source, "/source", "--ro-bind", input, "/input", "--ro-bind", wrapper, "/wrapper",
            "--seccomp", "3", runtime, NULL};
        for (guint i = 0; base[i]; ++i) g_ptr_array_add(argv, (gpointer)base[i]);
        if (job->language) g_ptr_array_add(argv, "--module");
        g_ptr_array_add(argv, "/wrapper"); g_ptr_array_add(argv, NULL);
        process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
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
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Plugin failed: %.2048s", output->str);
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
