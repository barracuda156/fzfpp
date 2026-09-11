#include "shellcmd.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fzf {

namespace {

// argv/envp arrays that stay valid across fork: the strings live in the
// owning vectors, the pointer arrays are built before the fork.
struct ExecImage {
    std::vector<std::string> argv_storage;
    std::vector<std::string> env_storage;
    std::vector<char*> argv;
    std::vector<char*> envp;

    ExecImage(const Executor& ex, const std::string& command, const std::vector<std::string>& extra_env)
        : argv_storage(ex.argv(command)), env_storage(merge_environment(extra_env)) {
        for (auto& s : argv_storage) argv.push_back(s.data());
        argv.push_back(nullptr);
        for (auto& s : env_storage) envp.push_back(s.data());
        envp.push_back(nullptr);
    }
};

std::string_view env_name(const std::string& kv) {
    size_t eq = kv.find('=');
    return std::string_view(kv).substr(0, eq == std::string::npos ? kv.size() : eq);
}

} // namespace

std::vector<std::string> merge_environment(const std::vector<std::string>& extra) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) env.emplace_back(*e);
    for (const auto& kv : extra) {
        std::string_view name = env_name(kv);
        bool replaced = false;
        for (auto& existing : env) {
            if (env_name(existing) == name) {
                existing = kv;
                replaced = true;
                break;
            }
        }
        if (!replaced) env.push_back(kv);
    }
    return env;
}

void shell_kill(pid_t pid, int sig) {
    if (pid > 0) kill(-pid, sig);
}

int shell_wait(pid_t pid) {
    if (pid <= 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return status;
}

SpawnedCommand shell_spawn(const Executor& ex, const std::string& command,
                           const std::vector<std::string>& extra_env, bool null_stdin,
                           bool setpgid_child, bool stderr_to_stdout) {
    SpawnedCommand result;
    int fds[2];
    if (pipe(fds) != 0) return result;

    ExecImage image(ex, command, extra_env);
    int devnull = -1;
    if (null_stdin) devnull = open("/dev/null", O_RDONLY);

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        if (devnull >= 0) close(devnull);
        return result;
    }
    if (pid == 0) {
        if (setpgid_child) setpgid(0, 0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        if (stderr_to_stdout) dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execve(image.argv[0], image.argv.data(), image.envp.data());
        _exit(127);
    }
    close(fds[1]);
    if (devnull >= 0) close(devnull);
    result.fd = fds[0];
    result.pid = pid;
    return result;
}

int shell_run_foreground(const Executor& ex, const std::string& command,
                         const std::vector<std::string>& extra_env, int tty_in, int tty_out) {
    ExecImage image(ex, command, extra_env);
    bool out_is_tty = isatty(STDOUT_FILENO);
    bool err_is_tty = isatty(STDERR_FILENO);

    struct sigaction ign {}, old_int {};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGINT, &ign, &old_int);

    pid_t pid = fork();
    if (pid == 0) {
        if (tty_in >= 0) dup2(tty_in, STDIN_FILENO);
        if (!out_is_tty && tty_out >= 0) dup2(tty_out, STDOUT_FILENO);
        if (!err_is_tty && tty_out >= 0) dup2(tty_out, STDERR_FILENO);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execve(image.argv[0], image.argv.data(), image.envp.data());
        _exit(127);
    }
    int status = pid > 0 ? shell_wait(pid) : -1;
    sigaction(SIGINT, &old_int, nullptr);
    return status;
}

int shell_run_silent(const Executor& ex, const std::string& command,
                     const std::vector<std::string>& extra_env) {
    ExecImage image(ex, command, extra_env);
    int devnull = open("/dev/null", O_RDWR);
    pid_t pid = fork();
    if (pid == 0) {
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execve(image.argv[0], image.argv.data(), image.envp.data());
        _exit(127);
    }
    if (devnull >= 0) close(devnull);
    return pid > 0 ? shell_wait(pid) : -1;
}

std::string shell_capture(const Executor& ex, const std::string& command,
                          const std::vector<std::string>& extra_env, bool first_line_only) {
    SpawnedCommand sp = shell_spawn(ex, command, extra_env, /*null_stdin=*/true, /*setpgid=*/false);
    if (sp.fd < 0) return std::string();
    std::string out;
    char buf[4096];
    bool done = false;
    while (!done) {
        ssize_t n = read(sp.fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (first_line_only) {
            const char* nl = static_cast<const char*>(memchr(buf, '\n', static_cast<size_t>(n)));
            if (nl) {
                out.append(buf, static_cast<size_t>(nl - buf));
                done = true;
            } else {
                out.append(buf, static_cast<size_t>(n));
            }
        } else {
            out.append(buf, static_cast<size_t>(n));
        }
    }
    close(sp.fd);
    shell_wait(sp.pid);
    if (first_line_only) {
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    }
    return out;
}

void shell_become(const Executor& ex, const std::string& command,
                  const std::vector<std::string>& extra_env, int tty_in, int out_fd) {
    ExecImage image(ex, command, extra_env);
    if (tty_in >= 0) dup2(tty_in, STDIN_FILENO);
    if (out_fd >= 0) dup2(out_fd, STDOUT_FILENO);
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    execve(image.argv[0], image.argv.data(), image.envp.data());
}

} // namespace fzf
