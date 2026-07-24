#pragma once

#include <string>
#include <termios.h>

namespace fzf {

// RAII wrapper for putting a tty into raw/cbreak mode and restoring it.
// Saves the original termios on construction and restores it on destruction
// (including on exception unwind), so a crashed or killed process doesn't
// leave the user's shell in raw mode.
class RawMode {
public:
    explicit RawMode(int fd);
    ~RawMode();

    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

    // True if raw mode was successfully entered (tcgetattr/tcsetattr both
    // succeeded). If false, the destructor is a no-op.
    bool active() const { return active_; }

private:
    int fd_;
    struct termios orig_termios_{};
    bool active_ = false;
};

// Query the terminal size via TIOCGWINSZ. Returns false (leaving rows/cols
// unchanged) if the ioctl fails, e.g. fd isn't a tty.
bool get_terminal_size(int fd, int& rows, int& cols);

// Terminal mode toggles. Each pair of functions writes the corresponding
// DECSET/DECRST escape sequence to fd (normally STDOUT_FILENO). These are
// plain byte writes, not tied to any RAII lifetime — callers are responsible
// for calling the matching "leave"/"disable" function before exiting.
void enter_alt_screen(int fd);
void leave_alt_screen(int fd);
void hide_cursor(int fd);
void show_cursor(int fd);
void enable_mouse(int fd);
void disable_mouse(int fd);

// Install a SIGWINCH handler that writes one byte to write_fd on every
// resize signal (async-signal-safe: the handler only calls write()).
// Returns false if sigaction() failed. Only one handler may be installed at
// a time (matches this program's single-Terminal-instance usage).
bool install_sigwinch_handler(int write_fd);

// Restore the default SIGWINCH disposition installed by
// install_sigwinch_handler.
void restore_sigwinch_handler();

// Create a non-blocking self-pipe (read_fd, write_fd). Returns false on
// failure. Used for the SIGWINCH pipe and the generic background-thread
// wake pipe.
bool make_self_pipe(int& read_fd, int& write_fd);

// Write a single wake byte to a self-pipe's write end. Safe to call from any
// thread (and, for the SIGWINCH pipe specifically, from a signal handler).
// Ignores EAGAIN (a full pipe already means a wake is pending).
void wake_pipe(int write_fd);

// Drain all currently-buffered bytes from a self-pipe's read end. Call this
// after poll() reports the fd readable, before acting on the wake, so
// repeated signals/wakes coalesce into a single action.
void drain_pipe(int read_fd);

// EINTR-safe, short-write-safe blocking write of `len` bytes from `data` to
// `fd`. Retries on EINTR instead of giving up, and keeps writing on a short
// write (n > 0 but n < remaining) until the whole buffer is out or a real
// error (n < 0, errno != EINTR) occurs. Shared by render.cpp's chrome/preview
// writes and terminal.cpp's frame writes so a signal landing mid-write can't
// truncate output — the exact failure class the sixel/DCS hold-back logic in
// render.cpp guards against at the protocol level; this guards the same blob
// at the syscall level.
void write_all(int fd, const char* data, size_t len);
inline void write_all(int fd, const std::string& data) {
    write_all(fd, data.data(), data.size());
}

} // namespace fzf
