#pragma once

// Child processes (fzf: src/util/util_unix.go ExecCommand / KillCommand /
// Become, src/terminal.go executeCommand). Every command runs through the
// Executor ($SHELL -c or --with-shell) via fork + execve with an explicit
// environment array: the FZF_* variables are merged into a copy of environ
// before the fork, so a multithreaded parent never calls setenv.
//
// Only async-signal-safe calls are used between fork and exec.

#include "executor.hpp"

#include <string>
#include <vector>

#include <sys/types.h>

namespace fzf {

struct SpawnedCommand {
    int fd = -1;      // read end of the child's stdout pipe
    pid_t pid = -1;   // also the process group id when spawned with setpgid
};

// environ with `extra` ("NAME=value") merged in; a later entry replaces an
// earlier one with the same name (Go's exec.Cmd.Env semantics).
std::vector<std::string> merge_environment(const std::vector<std::string>& extra);

// Start `command` with its stdout piped back. `null_stdin` gives the child
// /dev/null on stdin (reload, preview: Go's exec does that for a nil Stdin);
// `setpgid` puts it in its own process group so the whole pipeline can be
// killed at once; `stderr_to_stdout` merges stderr into the pipe (preview).
SpawnedCommand shell_spawn(const Executor& ex, const std::string& command,
                           const std::vector<std::string>& extra_env, bool null_stdin,
                           bool setpgid, bool stderr_to_stdout = false);

// execute(...): run in the foreground with stdin on `tty_in`, stdout and
// stderr on the tty when they are not ttys already; same process group as
// fzf (so it may read the terminal); waits. SIGINT is ignored by fzf itself
// meanwhile (fzf: "Don't quit by SIGINT while executing"). Returns the
// wait status.
int shell_run_foreground(const Executor& ex, const std::string& command,
                         const std::vector<std::string>& extra_env, int tty_in, int tty_out);

// execute-silent(...): stdin, stdout and stderr on /dev/null; waits.
int shell_run_silent(const Executor& ex, const std::string& command,
                     const std::vector<std::string>& extra_env);

// transform-*(...): stdout captured (stdin /dev/null, stderr inherited).
// `first_line_only` returns the first line without its "\r\n" (fzf:
// captureLine), otherwise everything (captureLines).
std::string shell_capture(const Executor& ex, const std::string& command,
                          const std::vector<std::string>& extra_env, bool first_line_only);

// become(...): execve the shell with stdin = tty_in and stdout = out_fd.
// Returns only when exec failed.
void shell_become(const Executor& ex, const std::string& command,
                  const std::vector<std::string>& extra_env, int tty_in, int out_fd);

// Signal the child's whole process group (the shell and anything it spawned).
void shell_kill(pid_t pid, int sig);

// waitpid(), retrying on EINTR. Returns the raw wait status or -1.
int shell_wait(pid_t pid);

} // namespace fzf
