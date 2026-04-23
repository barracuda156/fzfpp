#include "options.hpp"
#include "reader.hpp"
#include "terminal.hpp"
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char* argv[]) {
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

        if (stdin_is_tty) {
            std::cerr << "fzf: no input provided (try: command | fzf)" << std::endl;
            return 1;
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

        if (opts.filter) {
            fzf::Terminal terminal(opts, reader);
            auto results = terminal.run_filter(opts.query);

            if (opts.print_query) {
                std::cout << opts.query << std::endl;
            }

            for (const auto& result : results) {
                std::cout << result << std::endl;
            }

            return results.empty() ? 1 : 0;
        }

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
            return 2;
        }

        int tty_out = open("/dev/tty", O_WRONLY);
        if (tty_out == -1) {
            std::cerr << "Error: Failed to open /dev/tty for output" << std::endl;
            close(stdout_copy);
            return 2;
        }

        if (dup2(tty_out, STDOUT_FILENO) == -1) {
            std::cerr << "Error: Failed to dup2 tty to stdout" << std::endl;
            close(stdout_copy);
            close(tty_out);
            return 2;
        }
        close(tty_out);

        if (opts.select_1 || opts.exit_0) {
            fzf::Terminal terminal(opts, reader);
            auto results = terminal.run_filter(opts.query);

            if (opts.select_1 && results.size() == 1) {
                dup2(stdout_copy, STDOUT_FILENO);
                close(stdout_copy);
                std::cout << results[0] << std::endl;
                return 0;
            }

            if (opts.exit_0 && results.empty()) {
                close(stdout_copy);
                return 1;
            }
        }

        fzf::Terminal terminal(opts, reader);
        auto results = terminal.run();

        fflush(stdout);
        dup2(stdout_copy, STDOUT_FILENO);
        close(stdout_copy);

        setvbuf(stdout, nullptr, _IONBF, 0);

        if (opts.print_query && !results.empty()) {
            std::cout << opts.query << std::endl;
        }

        std::string expect_key = terminal.get_matched_expect_key();
        if (!expect_key.empty()) {
            std::cout << expect_key << std::endl;
        }

        for (const auto& result : results) {
            std::cout << result << std::endl;
        }
        std::cout.flush();

        fflush(stdout);
        fsync(STDOUT_FILENO);

        return results.empty() ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 2;
    }
}
