#include "reader.hpp"
#include "util.hpp"
#include "shellcmd.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <unistd.h>

namespace fzf {

void Reader::add_item(std::string line, bool trim_newline) {
    if (trim_newline) {
        // fzf semantics: trim exactly one trailing \n and at most one \r
        // before it — not a strip-all-trailing-CR/LF loop ("data\r\r\n"
        // becomes "data\r\r", not "data").
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
    }
    // --read0 records (trim_newline == false) are kept verbatim (minus the
    // \0 delimiter) — NUL-delimited input exists precisely to carry
    // embedded/trailing newlines.

    // Keep all lines like fzf does (including empty lines)
    {
        std::lock_guard<std::mutex> lock(items_mutex_);
        size_t index = items_.size();
        auto item = std::make_shared<Item>(std::move(line), index);

        // Parse fields if delimiter is set
        if (!delimiter_.empty()) {
            item->parse_fields(delimiter_);
        }

        items_.push_back(item);
        item_count_.fetch_add(1, std::memory_order_relaxed);
    }

    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        callback = wake_callback_;
    }
    if (callback) {
        callback();
    }
}

void Reader::read_from_stdin() {
    if (read_zero_) {
        // Read null-delimited input
        std::string line;
        char ch;
        while (std::cin.get(ch)) {
            if (ch == '\0') {
                add_item(std::move(line), /*trim_newline=*/false);
                line.clear();
            } else {
                line += ch;
            }
        }
        // Add last item if any
        if (!line.empty()) {
            add_item(std::move(line), /*trim_newline=*/false);
        }
    } else {
        // Read newline-delimited input (default)
        std::string line;
        while (std::getline(std::cin, line)) {
            add_item(std::move(line), /*trim_newline=*/true);
        }
    }
    read_finished_.store(true, std::memory_order_release);
}

bool Reader::read_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        add_item(std::move(line), /*trim_newline=*/true);
    }

    read_finished_.store(true, std::memory_order_release);
    return true;
}

void Reader::read_from_string(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        add_item(std::move(line), /*trim_newline=*/true);
    }
    read_finished_.store(true, std::memory_order_release);
}

std::vector<std::shared_ptr<Item>> Reader::get_items() const {
    std::lock_guard<std::mutex> lock(items_mutex_);
    return items_;
}

void Reader::start_async_stdin() {
    read_thread_ = std::thread([this]() {
        read_from_stdin();
    });
}

void Reader::start_async_fd(int fd) {
    read_thread_ = std::thread([this, fd]() {
        // Read from the provided file descriptor
        FILE* fp = fdopen(fd, "r");
        if (!fp) {
            close(fd);
            read_finished_.store(true, std::memory_order_release);
            return;
        }

        if (read_zero_) {
            // Read null-delimited input
            std::string line;
            int ch;
            while ((ch = fgetc(fp)) != EOF) {
                if (ch == '\0') {
                    add_item(std::move(line), /*trim_newline=*/false);
                    line.clear();
                } else {
                    line += static_cast<char>(ch);
                }
            }
            // Add last item if any
            if (!line.empty()) {
                add_item(std::move(line), /*trim_newline=*/false);
            }
        } else {
            // Read newline-delimited input (default)
            char* line = nullptr;
            size_t len = 0;
            ssize_t nread;

            while ((nread = getline(&line, &len, fp)) != -1) {
                // Construct with the known length rather than from the
                // char* — strlen() would truncate at an embedded NUL.
                add_item(std::string(line, static_cast<size_t>(nread)), /*trim_newline=*/true);
            }

            free(line);
        }

        fclose(fp);
        read_finished_.store(true, std::memory_order_release);
    });
}

void Reader::load_from_command(const std::string& command) {
    // Cancel any in-flight streaming read before swapping the item set, so the
    // background thread can't append stale items after we clear.
    if (read_thread_.joinable()) {
        read_thread_.join();
    }

    // Reset state for the fresh item set.
    {
        std::lock_guard<std::mutex> lock(items_mutex_);
        items_.clear();
        item_count_.store(0, std::memory_order_relaxed);
    }
    read_finished_.store(false, std::memory_order_release);

    // Run reload commands under $SHELL like preview/execute do (shell_popen);
    // libc popen hardcodes /bin/sh, which broke bashisms in reload() specs
    // the same way it once broke preview scripts (commit cbd93cd).
    ShellPipe pipe = shell_popen(command);
    FILE* fp = pipe.stream;
    if (!fp) {
        read_finished_.store(true, std::memory_order_release);
        return;
    }

    if (read_zero_) {
        std::string line;
        int ch;
        while ((ch = fgetc(fp)) != EOF) {
            if (ch == '\0') {
                add_item(std::move(line), /*trim_newline=*/false);
                line.clear();
            } else {
                line += static_cast<char>(ch);
            }
        }
        if (!line.empty()) {
            add_item(std::move(line), /*trim_newline=*/false);
        }
    } else {
        char* line = nullptr;
        size_t len = 0;
        ssize_t nread;
        while ((nread = getline(&line, &len, fp)) != -1) {
            // Construct with the known length rather than from the char* —
            // strlen() would truncate at an embedded NUL.
            add_item(std::string(line, static_cast<size_t>(nread)), /*trim_newline=*/true);
        }
        free(line);
    }

    shell_pclose(pipe);
    read_finished_.store(true, std::memory_order_release);
}

void Reader::wait_for_finish() {
    if (read_thread_.joinable()) {
        read_thread_.join();
    }
}

} // namespace fzf
