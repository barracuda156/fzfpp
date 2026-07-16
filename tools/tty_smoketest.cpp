// Standalone smoke test for src/tty.hpp — exercises raw mode, terminal size
// query, and the SIGWINCH self-pipe with zero dependency on Terminal/Reader/
// Matcher. Not linked into the fzf binary; run manually:
//
//   ./tty_smoketest
//
// Prints the current terminal size, then waits (via poll()) for either a
// keypress or a SIGWINCH (resize the terminal window to trigger it), prints
// what happened, and exits — restoring the original termios on the way out
// regardless of which one fired.

#include "../src/tty.hpp"

#include <cstdio>
#include <poll.h>
#include <unistd.h>

int main() {
    if (!isatty(STDIN_FILENO)) {
        std::fprintf(stderr, "tty_smoketest: stdin is not a tty\n");
        return 1;
    }

    int rows = 0, cols = 0;
    if (fzf::get_terminal_size(STDIN_FILENO, rows, cols)) {
        std::printf("Initial size: %d rows x %d cols\n", rows, cols);
    } else {
        std::printf("Initial size: TIOCGWINSZ failed\n");
    }

    int winch_read = -1, winch_write = -1;
    if (!fzf::make_self_pipe(winch_read, winch_write)) {
        std::fprintf(stderr, "tty_smoketest: failed to create self-pipe\n");
        return 1;
    }
    if (!fzf::install_sigwinch_handler(winch_write)) {
        std::fprintf(stderr, "tty_smoketest: failed to install SIGWINCH handler\n");
        return 1;
    }

    fzf::RawMode raw(STDIN_FILENO);
    if (!raw.active()) {
        std::fprintf(stderr, "tty_smoketest: failed to enter raw mode\n");
        fzf::restore_sigwinch_handler();
        return 1;
    }

    std::printf("Raw mode active. Press a key, or resize the terminal window...\r\n");
    std::fflush(stdout);

    struct pollfd fds[2] = {
        {STDIN_FILENO, POLLIN, 0},
        {winch_read, POLLIN, 0},
    };

    bool done = false;
    while (!done) {
        int n = poll(fds, 2, -1);
        if (n < 0) {
            continue;
        }
        if (fds[1].revents & POLLIN) {
            fzf::drain_pipe(winch_read);
            int new_rows = 0, new_cols = 0;
            fzf::get_terminal_size(STDIN_FILENO, new_rows, new_cols);
            std::printf("SIGWINCH fired. New size: %d rows x %d cols\r\n", new_rows, new_cols);
            std::fflush(stdout);
            done = true;
        }
        if (fds[0].revents & POLLIN) {
            char c = 0;
            ssize_t r = read(STDIN_FILENO, &c, 1);
            if (r == 1) {
                std::printf("Keypress: 0x%02x\r\n", static_cast<unsigned char>(c));
                std::fflush(stdout);
                done = true;
            }
        }
    }

    fzf::restore_sigwinch_handler();
    std::printf("Exiting, restoring termios...\r\n");
    return 0;
}
