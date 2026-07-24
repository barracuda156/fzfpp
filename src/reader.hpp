#pragma once

#include "item.hpp"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>

namespace fzf {

// Reader for input data
class Reader {
public:
    Reader() : read_finished_(false), item_count_(0), read_zero_(false) {}

    // Set a callback invoked (from whichever thread is reading) every time
    // add_item() appends a new item. Used to wake a poll()-driven main loop
    // via a self-pipe instead of the caller having to poll item_count() on a
    // timer. The callback must be safe to call from a background thread and
    // should be cheap (e.g. a single write() to a pipe) — it may be called
    // once per item during a fast bulk read.
    //
    // Guarded by wake_mutex_: the reader thread may be invoking the previous
    // callback concurrently with a caller reassigning/clearing it here.
    // set_wake_callback(nullptr) at teardown blocks until any in-flight
    // invocation completes, so no later invocation observes the old callback.
    void set_wake_callback(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_callback_ = std::move(callback);
    }

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

    // Replace the entire item set with the output of a shell command (used by
    // the reload(...) bind action). Runs synchronously: joins any in-flight read
    // thread, clears the current items, then repopulates from the command's
    // stdout. Item indices are reassigned from zero. Honors the read-zero and
    // delimiter settings already configured on this reader.
    void load_from_command(const std::string& command);

    // Wait for reading to finish
    void wait_for_finish();

private:
    // trim_newline: true for newline-delimited reads (strip exactly one
    // trailing \n and at most one \r before it, fzf semantics). false for
    // --read0 records, which must be added verbatim (minus the \0
    // delimiter) since NUL-delimited input exists precisely to carry
    // embedded/trailing newlines.
    void add_item(std::string line, bool trim_newline);

    std::vector<std::shared_ptr<Item>> items_;
    mutable std::mutex items_mutex_;
    std::atomic<bool> read_finished_;
    std::atomic<size_t> item_count_;
    std::thread read_thread_;
    std::string delimiter_;
    bool read_zero_;  // Read null-delimited input
    std::mutex wake_mutex_;
    std::function<void()> wake_callback_;
};

} // namespace fzf
