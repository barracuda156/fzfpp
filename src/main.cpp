#include "options.hpp"
#include "reader.hpp"
#include "terminal.hpp"
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>

namespace {

// Flush/sync stdout and terminate immediately, without running static/thread
// destructors. Once Reader::start_async_fd() has kicked off the background
// reader thread, ~Reader (stack-allocated in main) joins it -- and that
// thread only ever exits at producer EOF. For `find / | fzf`, accepting a
// result would otherwise leave the process (and the pipe's writer) alive
// and draining stdin long after the user got their answer. Real fzf exits
// immediately and lets the producer take SIGPIPE. Every return point in
// main() that executes after start_async_fd() must funnel through here
// instead of `return`; points before the thread starts may still `return`
// normally.
[[noreturn]] void finish(int code) {
    fflush(stdout);
    fsync(STDOUT_FILENO);
    _exit(code);
}

} // namespace

int main(int argc, char* argv[]) {
    // The reader thread's wake callback writes to a self-pipe whose read end
    // terminal teardown closes, and stdout may itself be a closed pipe (the
    // downstream reader exited already). Either can deliver SIGPIPE; without
    // this, an unhandled SIGPIPE kills fzf before it prints/restores the
    // terminal. The writes involved already tolerate EPIPE via existing error
    // handling, so ignoring the signal is sufficient.
    signal(SIGPIPE, SIG_IGN);

    try {
        auto opts = fzf::parse_options(argc, argv);

        fzf::Reader reader;

        if (!opts.delimiter.empty()) {
            reader.set_delimiter(opts.delimiter);
        }

        if (opts.read_zero) {
            reader.set_read_zero(true);
        }

        bool stdin_is_tty = isatty(STDIN_FILENO);

        // Non-interactive filter mode (--filter/-f) reads stdin, prints matches to
        // stdout, and exits. It never touches /dev/tty, so it must run BEFORE the
        // tty redirection below — otherwise `cmd | fzf -f query` in a headless
        // context (cron, CI, subprocess with no controlling terminal) aborts with
        // "Failed to open /dev/tty". Real fzf's --filter is the scripting path and
        // works without a tty; this mirrors that.
        if (opts.filter) {
            if (stdin_is_tty) {
                std::cerr << "fzf: no input provided (try: command | fzf)" << std::endl;
                return 2;
            }

            int filter_fd = dup(STDIN_FILENO);
            if (filter_fd == -1) {
                std::cerr << "Error: Failed to duplicate stdin" << std::endl;
                return 2;
            }

            reader.start_async_fd(filter_fd);

            fzf::Terminal terminal(opts, reader);
            auto results = terminal.run_filter(opts.query);

            if (opts.print_query) {
                std::cout << opts.query << std::endl;
            }

            for (const auto& result : results) {
                std::cout << result << std::endl;
            }

            finish(results.empty() ? 1 : 0);
        }

        if (stdin_is_tty) {
            std::cerr << "fzf: no input provided (try: command | fzf)" << std::endl;
            return 2;
        }

        int pipe_fd = dup(STDIN_FILENO);
        if (pipe_fd == -1) {
            std::cerr << "Error: Failed to duplicate stdin" << std::endl;
            return 2;
        }

        int tty_fd = open("/dev/tty", O_RDONLY);
        if (tty_fd == -1) {
            std::cerr << "Error: Failed to open /dev/tty" << std::endl;
            close(pipe_fd);
            return 2;
        }

        if (dup2(tty_fd, STDIN_FILENO) == -1) {
            std::cerr << "Error: Failed to dup2 tty to stdin" << std::endl;
            close(pipe_fd);
            close(tty_fd);
            return 2;
        }
        close(tty_fd);

        reader.start_async_fd(pipe_fd);

        const size_t initial_items_target = 25;
        const int max_wait_ms = 500;
        const int poll_interval_ms = 10;
        int waited_ms = 0;

        while (waited_ms < max_wait_ms) {
            size_t item_count = reader.item_count();
            if (item_count >= initial_items_target || reader.is_finished()) {
                break;
            }
            usleep(poll_interval_ms * 1000);
            waited_ms += poll_interval_ms;
        }

        int stdout_copy = dup(STDOUT_FILENO);
        if (stdout_copy == -1) {
            std::cerr << "Error: Failed to duplicate stdout" << std::endl;
            finish(2);
        }

        int tty_out = open("/dev/tty", O_WRONLY);
        if (tty_out == -1) {
            std::cerr << "Error: Failed to open /dev/tty for output" << std::endl;
            close(stdout_copy);
            finish(2);
        }

        if (dup2(tty_out, STDOUT_FILENO) == -1) {
            std::cerr << "Error: Failed to dup2 tty to stdout" << std::endl;
            close(stdout_copy);
            close(tty_out);
            finish(2);
        }
        close(tty_out);

        if (opts.select_1 || opts.exit_0) {
            fzf::Terminal terminal(opts, reader);
            auto results = terminal.run_filter(opts.query);

            if (opts.select_1 && results.size() == 1) {
                dup2(stdout_copy, STDOUT_FILENO);
                close(stdout_copy);

                if (opts.print_query) {
                    std::cout << opts.query << std::endl;
                }
                if (!opts.expect_keys.empty()) {
                    // Auto-accept via --select-1 is never a matched expect
                    // key -- print the blank key line so output stays
                    // positional, same as a plain-Enter accept.
                    std::cout << std::endl;
                }
                std::cout << results[0] << std::endl;
                finish(0);
            }

            if (opts.exit_0 && results.empty()) {
                dup2(stdout_copy, STDOUT_FILENO);
                close(stdout_copy);

                if (opts.print_query) {
                    std::cout << opts.query << std::endl;
                }
                finish(1);
            }
        }

        fzf::Terminal terminal(opts, reader);
        auto results = terminal.run();

        fflush(stdout);
        dup2(stdout_copy, STDOUT_FILENO);
        close(stdout_copy);

        setvbuf(stdout, nullptr, _IONBF, 0);

        bool aborted = terminal.was_aborted();

        // --print-query always prints the live query first -- on accept,
        // no-match, and abort alike -- so scripts that re-prompt with
        // whatever the user typed can rely on it always being there.
        if (opts.print_query) {
            std::cout << terminal.final_query() << std::endl;
        }

        if (!aborted) {
            // --expect always prints the key line right after the query
            // line (blank for a plain-Enter accept) so downstream output
            // stays positional; only skipped entirely on abort, matching
            // real fzf (no expect/results lines at all once cancelled).
            if (!opts.expect_keys.empty()) {
                std::cout << terminal.get_matched_expect_key() << std::endl;
            }

            for (const auto& result : results) {
                std::cout << result << std::endl;
            }
        }
        std::cout.flush();

        int code;
        if (aborted) {
            code = 130;
        } else if (!results.empty()) {
            code = 0;
        } else {
            code = 1;
        }

        finish(code);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 2;
    }
}
