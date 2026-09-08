# Analysis plugin API 1

Select a retained entry in Analyze and choose **Run analysis plugin**. The input
is a snapshot of that normalized UTF-8 log line. Load or edit a `.lua` or `.js`
file, choose its language and run it. Results are selectable text in a separate
window. Plugins are invoked manually; they do not change raw logs, send commands,
connect devices or run automatically on import.

Define `transform(input)`. Lua 5.4 must return a string. JavaScript must return a
synchronous string or JSON-serializable value; promises are rejected.
`TIO_PLUGIN_API` is `1`. Source and input each have a 64 KiB limit; output including
errors has a 64 KiB limit. Examples are in `examples/plugins/`.

Each invocation starts a fresh bubblewrap process with separate network, PID,
mount and other namespaces, empty environment, read-only runtime libraries and
source/input, and no home directory. Lua exposes only string/math/table/utf8 and
basic conversion/iteration/error functions. JavaScript uses QuickJS. Seccomp
blocks process/thread creation, network sockets, ptrace and namespace/mount changes.
The runner enforces a two-second wall deadline, two CPU seconds, 256 MiB address
space and zero file/core output limits. Closing the window cancels execution.
An unavailable sandbox is an error; the application never retries unsandboxed.

Runtime dependencies are bubblewrap plus Lua 5.4 or QuickJS; the Nix package
includes all three. The tested sandbox target is Linux x86-64. Runtime libraries
may be readable; user files and host serial devices are not mounted. This API is
for bounded log transformations, not general application automation.
