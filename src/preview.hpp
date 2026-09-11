#pragma once

// Preview worker (fzf: src/terminal.go, the previewer goroutine in Loop).
// One background thread runs the preview command for the current target,
// streams its output into a buffer that the main thread paints, and kills
// the command's process group when a newer target supersedes it.
//
// The main thread expands the placeholders (it owns the item state) and
// hands over the final command line plus the FZF_* environment and the
// temp files created for {f}; the worker removes those when the command
// finishes. Content is published under a mutex together with a version:
// output of a superseded command never reaches the screen or the cache.

#include "executor.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace fzf {

class PreviewWorker {
public:
    struct Request {
        std::string command;
        std::vector<std::string> env;
        std::vector<std::string> temp_files;
    };
    struct Content {
        std::string text;
        uint64_t version = 0;   // request that produced it
        bool complete = false;  // the command exited
    };

    // `wake` is called (from the worker thread) whenever new output was
    // published; it must be cheap and thread-safe.
    PreviewWorker(const Executor& executor, std::function<void()> wake);
    ~PreviewWorker();

    void start();
    // Stops the thread and kills the running command.
    void shutdown();

    // Replace the target: the in-flight command is killed, its pending
    // output dropped, and `r` runs next. Returns the new version.
    uint64_t request(Request r);
    // Kill the in-flight command without starting another one (hide-preview,
    // toggle-preview off). The published content is kept.
    void cancel();
    // Install content from the cache (or an empty pane) as the current one.
    void set_content(std::string text, bool complete);

    Content content() const;
    uint64_t version() const { return version_.load(std::memory_order_acquire); }
    bool running() const { return child_pid_.load(std::memory_order_acquire) > 0; }

private:
    void loop();

    const Executor& executor_;
    std::function<void()> wake_;
    std::thread thread_;

    std::mutex req_mu_;
    std::condition_variable req_cv_;
    bool has_request_ = false;
    bool stop_ = false;
    Request pending_;

    std::atomic<uint64_t> version_{0};
    std::atomic<pid_t> child_pid_{-1};

    mutable std::mutex content_mu_;
    Content content_;
};

} // namespace fzf
