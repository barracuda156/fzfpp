#include "options.hpp"
#include "reader.hpp"
#include "search.hpp"
#include "terminal.hpp"

#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

// Flush stdout and terminate immediately, without running static/thread
// destructors. The reader thread may still be draining a producer that
// never closes its end (`find / | fzf`): real fzf exits immediately and
// lets the producer take SIGPIPE, and so do we. Every return point in
// main() that executes after the reader started must funnel through here.
[[noreturn]] void finish(int code) {
    fflush(stdout);
    _exit(code);
}

// The output protocol (fzf: opts.Printer): one line per entry, terminated
// by "\n" or, with --print0, by NUL.
void write_fully(int fd, const std::string& out) {
    size_t off = 0;
    while (off < out.size()) {
        ssize_t w = write(fd, out.data() + off, out.size() - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        off += static_cast<size_t>(w);
    }
}

void print_lines(int fd, const std::vector<std::string>& lines, bool print0) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += print0 ? '\0' : '\n';
        if (out.size() >= 64 * 1024) {
            write_fully(fd, out);
            out.clear();
        }
    }
    if (!out.empty()) write_fully(fd, out);
}

// Non-interactive match of the whole input (fzf: core.go's filtering
// branch, also used for --select-1/--exit-0): waits for EOF, scans once on
// this thread, and returns the output text of every match in order.
std::vector<std::string> filter_results(const fzf::Options& opts, fzf::ItemBuilder& builder,
                                        fzf::Reader& reader, const std::string& query) {
    reader.wait();
    fzf::Searcher searcher(opts, /*interactive=*/false);
    auto list = reader.list();
    auto merger = searcher.scan_sync(list->snapshot(), query, /*final=*/true, opts.sort > 0);
    std::vector<std::string> out;
    out.reserve(merger->size());
    for (uint32_t i = 0; i < merger->size(); ++i) out.push_back(builder.output_text(merger->get(i)));
    return out;
}

// fzf: core.go streamingFilter -- when the output order is the input order
// anyway (--no-sort without --tac/--sync), matches are printed as the input
// arrives instead of after EOF. Returns whether anything matched.
bool streaming_filter(const fzf::Options& opts, fzf::ItemBuilder& builder, fzf::Reader& reader,
                      const std::string& query) {
    fzf::Searcher searcher(opts, /*interactive=*/false);
    auto pattern = searcher.build_pattern(query);
    std::mutex mu;
    std::condition_variable cv;
    bool woke = false;
    reader.set_wake_callback([&]() {
        {
            std::lock_guard<std::mutex> lock(mu);
            woke = true;
        }
        cv.notify_one();
    });
    bool found = false;
    size_t chunk_i = 0;
    uint32_t item_i = 0;
    const char sep = opts.print0 ? '\0' : '\n';
    for (;;) {
        auto list = reader.list();
        fzf::ChunkList::Snapshot snap = list->snapshot();
        std::string out;
        for (; chunk_i < snap.chunks.size(); ++chunk_i) {
            const auto& chunk = snap.chunks[chunk_i];
            uint32_t count = snap.counts[chunk_i];
            for (; item_i < count; ++item_i) {
                if (pattern->empty() || pattern->match(*chunk, item_i).matched) {
                    out += builder.output_text(fzf::ItemRef{chunk, item_i});
                    out += sep;
                    found = true;
                }
            }
            if (chunk_i + 1 < snap.chunks.size()) item_i = 0;
        }
        if (chunk_i > 0 && chunk_i == snap.chunks.size()) --chunk_i;   // stay on the growing tail chunk
        if (!out.empty()) write_fully(STDOUT_FILENO, out);
        if (snap.finished) break;
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return woke; });
        woke = false;
        reader.ack_wake();
    }
    reader.set_wake_callback(nullptr);
    return found;
}

} // namespace

int main(int argc, char* argv[]) {
    // The reader thread's wake callback writes to a self-pipe whose read end
    // terminal teardown closes, and stdout may itself be a closed pipe (the
    // downstream reader exited already). Either can deliver SIGPIPE; without
    // this, an unhandled SIGPIPE kills fzf before it prints/restores the
    // terminal.
    signal(SIGPIPE, SIG_IGN);

    try {
        auto opts = fzf::parse_options(argc, argv);

        // --with-nth / --accept-nth templates are validated here, like
        // fzf's option parser does (exit 2 with the same message).
        std::unique_ptr<fzf::ItemBuilder> builder_ptr;
        try {
            builder_ptr = std::make_unique<fzf::ItemBuilder>(opts);
        } catch (const fzf::OptionError& e) {
            std::cerr << e.message << std::endl;
            return 2;
        }
        fzf::ItemBuilder& builder = *builder_ptr;
        fzf::Reader reader(opts, builder);

        bool stdin_is_tty = isatty(STDIN_FILENO);

        // Non-interactive filter mode (--filter/-f) reads stdin (or, with a
        // tty stdin, $FZF_DEFAULT_COMMAND / the walker like fzf), prints
        // matches to stdout, and exits. It never touches /dev/tty.
        if (opts.filter) {
            if (stdin_is_tty) {
                reader.start_default_source();
            } else {
                int filter_fd = dup(STDIN_FILENO);
                if (filter_fd == -1) {
                    std::cerr << "Error: Failed to duplicate stdin" << std::endl;
                    return 2;
                }
                reader.start_fd(filter_fd);
            }
            if (opts.print_query) print_lines(STDOUT_FILENO, {*opts.filter}, opts.print0);
            bool streaming = opts.sort == 0 && !opts.tac && !opts.sync;
            if (streaming) {
                finish(streaming_filter(opts, builder, reader, *opts.filter) ? 0 : 1);
            }
            auto results = filter_results(opts, builder, reader, *opts.filter);
            print_lines(STDOUT_FILENO, results, opts.print0);
            finish(results.empty() ? 1 : 0);
        }

        // fzf: ReadSource -- a pipe on stdin is the input; a tty stdin means
        // $FZF_DEFAULT_COMMAND or the built-in directory walker.
        int pipe_fd = -1;
        if (!stdin_is_tty) {
            pipe_fd = dup(STDIN_FILENO);
            if (pipe_fd == -1) {
                std::cerr << "Error: Failed to duplicate stdin" << std::endl;
                return 2;
            }
        }

        int tty_fd = open("/dev/tty", O_RDONLY);
        if (tty_fd == -1) {
            std::cerr << "Error: Failed to open /dev/tty" << std::endl;
            if (pipe_fd >= 0) close(pipe_fd);
            return 2;
        }
        if (dup2(tty_fd, STDIN_FILENO) == -1) {
            std::cerr << "Error: Failed to dup2 tty to stdin" << std::endl;
            if (pipe_fd >= 0) close(pipe_fd);
            close(tty_fd);
            return 2;
        }
        close(tty_fd);

        if (pipe_fd >= 0) {
            reader.start_fd(pipe_fd);
        } else {
            reader.start_default_source();
        }

        // --sync: wait for EOF before the first frame.
        if (opts.sync) reader.wait();

        // The UI is drawn on the tty through fd 1 (so child commands of
        // execute() inherit a terminal); the output protocol goes to the
        // original stdout kept in `stdout_copy`.
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

        // fzf: the deferred start of the terminal -- --select-1 / --exit-0
        // are decided on the first complete match before the UI is shown.
        if (opts.select_1 || opts.exit_0) {
            auto results = filter_results(opts, builder, reader, opts.query);
            bool select_1 = opts.select_1 && results.size() == 1;
            bool exit_0 = opts.exit_0 && results.empty();
            if (select_1 || exit_0) {
                std::vector<std::string> lines;
                if (opts.print_query) lines.push_back(opts.query);
                if (!opts.expect_specs.empty()) lines.push_back("");
                for (const auto& r : results) lines.push_back(r);
                print_lines(stdout_copy, lines, opts.print0);
                finish(results.empty() ? 1 : 0);
            }
        }

        std::unique_ptr<fzf::Terminal> terminal;
        try {
            terminal = std::make_unique<fzf::Terminal>(opts, builder, reader, STDIN_FILENO, STDOUT_FILENO, stdout_copy);
        } catch (const fzf::OptionError& e) {
            std::cerr << e.message << std::endl;
            finish(2);
        }
        fzf::RunResult result = terminal->run();
        terminal.reset();

        print_lines(stdout_copy, result.lines, opts.print0);
        finish(result.exit_code);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 2;
    }
}
