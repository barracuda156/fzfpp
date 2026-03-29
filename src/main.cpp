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
        // Parse command-line options
        auto opts = fzf::parse_options(argc, argv);

        // Create reader
        fzf::Reader reader;

        // Set delimiter if specified
        if (!opts.delimiter.empty()) {
            reader.set_delimiter(opts.delimiter);
        }

        // Set read-zero mode if specified
        if (opts.read_zero) {
            reader.set_read_zero(true);
        }

        // Save whether stdin is a tty before any redirections
        bool stdin_is_tty = isatty(STDIN_FILENO);

        // If stdin is a TTY (no piped input), exit with error
        // TODO: Implement "find" command like golang fzf for better UX
        if (stdin_is_tty) {
            std::cerr << "fzf: no input provided (try: command | fzf)" << std::endl;
            return 1;
        }

        // Handle stdin/tty setup for piped input
        // Stdin is a pipe - we need to separate it from keyboard input

        // Duplicate the pipe to a high fd for the reader
        int pipe_fd = dup(STDIN_FILENO);
        if (pipe_fd == -1) {
            std::cerr << "Error: Failed to duplicate stdin" << std::endl;
            return 2;
        }

        // Open /dev/tty for keyboard
        int tty_fd = open("/dev/tty", O_RDONLY);
        if (tty_fd == -1) {
            std::cerr << "Error: Failed to open /dev/tty" << std::endl;
            close(pipe_fd);
            return 2;
        }

        // Make /dev/tty become stdin (fd 0) for FTXUI
        if (dup2(tty_fd, STDIN_FILENO) == -1) {
            std::cerr << "Error: Failed to dup2 tty to stdin" << std::endl;
            close(pipe_fd);
            close(tty_fd);
            return 2;
        }
        close(tty_fd);  // Close the original tty_fd, we have it as stdin now

        // Start reader on the pipe fd
        reader.start_async_fd(pipe_fd);

        // Handle filter mode (non-interactive)
        if (opts.filter) {
            fzf::Terminal terminal(opts, reader);
            auto results = terminal.run_filter(opts.query);

            // Print query if requested
            if (opts.print_query) {
                std::cout << opts.query << std::endl;
            }

            // Print results
            for (const auto& result : results) {
                std::cout << result << std::endl;
            }

            return results.empty() ? 1 : 0;
        }

        // Wait for initial items before showing TUI
        // This improves UX - user sees results immediately rather than empty screen
        // Wait for either: 25 items, reader finished, or 500ms timeout
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

        // Save original stdout (might be a pipe in command substitution)
        int stdout_copy = dup(STDOUT_FILENO);
        if (stdout_copy == -1) {
            std::cerr << "Error: Failed to duplicate stdout" << std::endl;
            return 2;
        }

        // Redirect stdout to /dev/tty for interactive TUI display
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
        close(tty_out);  // Close original fd, we have it as stdout now

        // Check for select-1 and exit-0 modes
        if (opts.select_1 || opts.exit_0) {
            fzf::Terminal terminal(opts, reader);
            auto results = terminal.run_filter(opts.query);

            if (opts.select_1 && results.size() == 1) {
                // Restore original stdout before printing
                dup2(stdout_copy, STDOUT_FILENO);
                close(stdout_copy);
                std::cout << results[0] << std::endl;
                return 0;
            }

            if (opts.exit_0 && results.empty()) {
                close(stdout_copy);
                return 1;
            }

            // Fall through to interactive mode if conditions not met
        }

        // Run interactive terminal
        fzf::Terminal terminal(opts, reader);
        auto results = terminal.run();

        // Restore original stdout file descriptor
        fflush(stdout);  // Flush /dev/tty output first
        dup2(stdout_copy, STDOUT_FILENO);
        close(stdout_copy);

        // Unbuffer stdout to avoid mixing buffered/unbuffered I/O
        setvbuf(stdout, nullptr, _IONBF, 0);

        // Print query if requested
        if (opts.print_query && !results.empty()) {
            std::cout << opts.query << std::endl;
        }

        // Print selected results directly
        for (const auto& result : results) {
            std::cout << result << std::endl;
        }
        std::cout.flush();

        // Force flush C stream and sync file descriptor to disk
        fflush(stdout);
        fsync(STDOUT_FILENO);

        return results.empty() ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 2;
    }
}
