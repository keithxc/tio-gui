/* GPL-3.0-only. Keep a portable runtime visible inside the plugin sandbox.
 * The application already hides /tmp and clears the environment. Add only a
 * read-only bind after those mounts, retaining all isolation/seccomp options.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *root = getenv("APPDIR");
    if (!root || root[0] != '/') {
        fputs("Portable bubblewrap requires an absolute APPDIR\n", stderr);
        return 2;
    }
    char *program = NULL;
    if (asprintf(&program, "%s/usr/bin/bwrap-real", root) < 0) return 2;
    char **args = calloc((size_t)argc + 8, sizeof(char *));
    if (!args) return 2;
    args[0] = program;
    int index = 1, inserted = 0;
    for (int i = 1; i < argc; ++i) {
        if (!inserted && strcmp(argv[i], "--seccomp") == 0) {
            args[index++] = "--ro-bind";
            args[index++] = (char *)root;
            args[index++] = (char *)root;
            /* The launcher's CA discovery must not emit diagnostics into a
               plugin's combined output. Bind only the public host trust
               bundle, not /etc or user data; networking remains denied. */
            args[index++] = "--ro-bind-try";
            args[index++] = "/etc/ssl/certs/ca-certificates.crt";
            args[index++] = "/etc/ssl/certs/ca-certificates.crt";
            inserted = 1;
        }
        args[index++] = argv[i];
    }
    if (!inserted) {
        fputs("Portable bubblewrap expects the application's seccomp sandbox\n", stderr);
        return 2;
    }
    execv(program, args);
    perror("bwrap-real");
    return 127;
}
