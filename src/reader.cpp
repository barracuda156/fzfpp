#include "reader.hpp"
#include "util.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>

namespace fzf {

void Reader::add_item(std::string line) {
    // Remove trailing newline/carriage return
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }

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
}

void Reader::read_from_stdin() {
    if (read_zero_) {
        // Read null-delimited input
        std::string line;
        char ch;
        while (std::cin.get(ch)) {
            if (ch == '\0') {
                add_item(std::move(line));
                line.clear();
            } else {
                line += ch;
            }
        }
        // Add last item if any
        if (!line.empty()) {
            add_item(std::move(line));
        }
    } else {
        // Read newline-delimited input (default)
        std::string line;
        while (std::getline(std::cin, line)) {
            add_item(std::move(line));
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
        add_item(std::move(line));
    }

    read_finished_.store(true, std::memory_order_release);
    return true;
}

void Reader::read_from_string(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        add_item(std::move(line));
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
            read_finished_.store(true, std::memory_order_release);
            return;
        }

        if (read_zero_) {
            // Read null-delimited input
            std::string line;
            int ch;
            while ((ch = fgetc(fp)) != EOF) {
                if (ch == '\0') {
                    add_item(std::move(line));
                    line.clear();
                } else {
                    line += static_cast<char>(ch);
                }
            }
            // Add last item if any
            if (!line.empty()) {
                add_item(std::move(line));
            }
        } else {
            // Read newline-delimited input (default)
            char* line = nullptr;
            size_t len = 0;
            ssize_t nread;

            while ((nread = getline(&line, &len, fp)) != -1) {
                // Remove trailing newline
                if (nread > 0 && line[nread-1] == '\n') {
                    line[nread-1] = '\0';
                }
                add_item(std::string(line));
            }

            free(line);
        }

        fclose(fp);
        read_finished_.store(true, std::memory_order_release);
    });
}

void Reader::wait_for_finish() {
    if (read_thread_.joinable()) {
        read_thread_.join();
    }
}

} // namespace fzf
