#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

extern char** environ;

namespace fzf {

// popen(3) always execs the *system* shell (/bin/sh), never the user's
// $SHELL. Real fzf runs preview/execute/reload commands under $SHELL
// (falling back to sh if unset) -- on most Linux distros /bin/sh is dash,
// whose printf builtin doesn't support bash's \xHH hex escapes and differs
// from bash in other ways preview scripts routinely rely on, so a script
// that works under interactive bash can silently misbehave when run through
// dash. This is a minimal popen(cmd, "r") replacement that execs $SHELL -c
// instead, keeping the same "read stdout, later reap the child" shape.
//
// The child is placed in its own process group so a caller can kill the
// whole preview pipeline (shell + whatever it spawned) with one
// kill(-pid, ...) -- fzf does the same to cancel superseded previews.
struct ShellPipe {
    FILE* stream = nullptr;
    pid_t pid = -1;
};

// extra_env: "NAME=value" strings appended to the child's environment
// (overriding inherited values by coming later in the array). Passed via
// execve rather than setenv so a multithreaded parent never mutates its own
// environ (setenv racing getenv on another thread is UB in glibc).
inline ShellPipe shell_popen(const std::string& cmd,
                             const std::vector<std::string>& extra_env = {}) {
    ShellPipe result;

    int fds[2];
    if (pipe(fds) != 0) {
        return result;
    }

    // Resolve everything that allocates BEFORE fork: in a multithreaded
    // process the child may only use async-signal-safe calls until exec.
    const char* shell = getenv("SHELL");
    if (!shell || shell[0] == '\0') {
        shell = "/bin/sh";
    }

    std::vector<char*> envp;
    std::vector<std::string> env_storage(extra_env);
    for (char** e = environ; *e; ++e) {
        envp.push_back(*e);
    }
    for (auto& s : env_storage) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    const char* argv[4] = {shell, "-c", cmd.c_str(), nullptr};

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return result;
    }

    if (pid == 0) {
        // Child: own process group, stdout -> pipe, exec the shell.
        setpgid(0, 0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execve(shell, const_cast<char* const*>(argv), envp.data());
        _exit(127);  // exec failed
    }

    close(fds[1]);
    result.stream = fdopen(fds[0], "r");
    if (!result.stream) {
        close(fds[0]);
        int status;
        waitpid(pid, &status, 0);
        return ShellPipe{};
    }
    result.pid = pid;
    return result;
}

// Signal the child's whole process group (the shell and anything it spawned).
// Safe to call from a thread other than the one that will shell_pclose().
inline void shell_kill(pid_t pid, int sig) {
    if (pid > 0) {
        kill(-pid, sig);
    }
}

inline void shell_pclose(ShellPipe& p) {
    if (p.stream) {
        fclose(p.stream);
        p.stream = nullptr;
    }
    if (p.pid > 0) {
        int status;
        waitpid(p.pid, &status, 0);
        p.pid = -1;
    }
}

} // namespace fzf
