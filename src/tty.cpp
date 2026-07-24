#include "tty.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fzf {

RawMode::RawMode(int fd) : fd_(fd) {
    if (tcgetattr(fd_, &orig_termios_) != 0) {
        return;
    }

    struct termios raw = orig_termios_;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &raw) == 0) {
        active_ = true;
    }
}

RawMode::~RawMode() {
    if (active_) {
        // TCSAFLUSH (not TCSANOW): drain pending output and discard any
        // unread raw input before restoring the original termios. Raw mode
        // disables echo/canon, so keystrokes typed while fzf was exiting (or
        // buffered but not yet read, e.g. mouse-report bytes) would otherwise
        // leak straight into the parent shell's line editor as soon as it
        // regains the tty -- TCSANOW restores the mode but leaves that
        // typeahead queued.
        tcsetattr(fd_, TCSAFLUSH, &orig_termios_);
    }
}

bool get_terminal_size(int fd, int& rows, int& cols) {
    struct winsize w{};
    if (ioctl(fd, TIOCGWINSZ, &w) != 0) {
        return false;
    }
    if (w.ws_row == 0 || w.ws_col == 0) {
        // Some ptys (unset, or mid-attach) report a 0x0 size instead of
        // failing the ioctl outright. Treat that the same as failure so
        // callers fall back to their default dimensions instead of trying to
        // lay out a frame in a zero-size terminal.
        return false;
    }
    rows = w.ws_row;
    cols = w.ws_col;
    return true;
}

void write_all(int fd, const char* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        written += static_cast<size_t>(n);
    }
}

static void write_seq(int fd, const char* seq) {
    write_all(fd, seq, std::strlen(seq));
}

void enter_alt_screen(int fd) { write_seq(fd, "\x1b[?1049h"); }
void leave_alt_screen(int fd) { write_seq(fd, "\x1b[?1049l"); }
void hide_cursor(int fd) { write_seq(fd, "\x1b[?25l"); }
void show_cursor(int fd) { write_seq(fd, "\x1b[?25h"); }

void enable_mouse(int fd) {
    write_seq(fd, "\x1b[?1000h\x1b[?1002h\x1b[?1006h");
}

void disable_mouse(int fd) {
    write_seq(fd, "\x1b[?1006l\x1b[?1002l\x1b[?1000l");
}

namespace {
int g_winch_write_fd = -1;
struct sigaction g_old_sigwinch{};

void sigwinch_handler(int) {
    if (g_winch_write_fd < 0) {
        return;
    }
    // Save/restore errno: this handler can interrupt any libc call in the
    // main thread, including one that's mid-way through setting errno for its
    // own (unrelated) failure. write() below sets errno on EAGAIN/EPIPE/etc.,
    // which would otherwise clobber that value out from under the code the
    // signal interrupted, right before it checks errno.
    int saved_errno = errno;
    char b = 1;
    // write() is async-signal-safe; ignore the result (EAGAIN just means a
    // wake is already pending, which is fine — we only need one).
    ssize_t r = write(g_winch_write_fd, &b, 1);
    (void)r;
    errno = saved_errno;
}
} // namespace

bool install_sigwinch_handler(int write_fd) {
    g_winch_write_fd = write_fd;

    struct sigaction sa{};
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    return sigaction(SIGWINCH, &sa, &g_old_sigwinch) == 0;
}

void restore_sigwinch_handler() {
    sigaction(SIGWINCH, &g_old_sigwinch, nullptr);
    g_winch_write_fd = -1;
}

bool make_self_pipe(int& read_fd, int& write_fd) {
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }

    for (int fd : fds) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        // FD_CLOEXEC: without it, every preview/execute child (fork+exec'd
        // for $SHELL -c '...') inherits these pipe ends. That leaks our wake
        // fds into unrelated processes and, worse, keeps the write end open
        // in the child even after we close our own copy, so a reader could
        // block forever waiting for EOF that never comes. Check fcntl's
        // result but proceed either way on failure -- the pipe still works
        // for its actual purpose, it just leaks into children same as
        // before.
        int fdflags = fcntl(fd, F_GETFD, 0);
        if (fdflags != -1) {
            fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
        }
    }

    read_fd = fds[0];
    write_fd = fds[1];
    return true;
}

void wake_pipe(int write_fd) {
    if (write_fd < 0) {
        return;
    }
    char b = 1;
    ssize_t r = write(write_fd, &b, 1);
    (void)r;
}

void drain_pipe(int read_fd) {
    char buf[64];
    while (read(read_fd, buf, sizeof(buf)) > 0) {
        // keep draining
    }
}

} // namespace fzf
