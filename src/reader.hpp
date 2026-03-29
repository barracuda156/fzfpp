#pragma once

#include "item.hpp"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

namespace fzf {

// Reader for input data
class Reader {
public:
    Reader() : read_finished_(false), item_count_(0), read_zero_(false) {}

    ~Reader() {
        if (read_thread_.joinable()) {
            read_thread_.join();
        }
    }

    // Set delimiter for field parsing
    void set_delimiter(const std::string& delimiter) {
        delimiter_ = delimiter;
    }

    // Set read-zero mode (null-delimited input)
    void set_read_zero(bool read_zero) {
        read_zero_ = read_zero;
    }

    // Read from stdin (blocking)
    void read_from_stdin();

    // Read from file
    bool read_from_file(const std::string& filename);

    // Read from string (for testing)
    void read_from_string(const std::string& content);

    // Get all items (thread-safe)
    std::vector<std::shared_ptr<Item>> get_items() const;

    // Get item count
    size_t item_count() const {
        return item_count_.load(std::memory_order_relaxed);
    }

    // Check if reading is finished
    bool is_finished() const {
        return read_finished_.load(std::memory_order_acquire);
    }

    // Start async reading in background thread
    void start_async_stdin();

    // Start async reading from a specific file descriptor
    void start_async_fd(int fd);

    // Wait for reading to finish
    void wait_for_finish();

private:
    void add_item(std::string line);

    std::vector<std::shared_ptr<Item>> items_;
    mutable std::mutex items_mutex_;
    std::atomic<bool> read_finished_;
    std::atomic<size_t> item_count_;
    std::thread read_thread_;
    std::string delimiter_;
    bool read_zero_;  // Read null-delimited input
};

} // namespace fzf
