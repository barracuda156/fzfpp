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

    // Low-priority queue (FZFPP_PREVIEW_PREFETCH): commands for the items
    // around the cursor, run one at a time only while no real request is
    // pending. Their output is never shown directly; it is handed back
    // through take_prefetched() for the main thread's cache. A real request
    // for the same command line adopts a running prefetch instead of
    // killing it; any other real request kills it.
    struct Prefetched {
        std::string command;
        std::string text;
    };
    void prefetch(std::vector<Request> requests);   // replaces the queue
    // Drop the queue; `kill_running` also kills a prefetch in flight.
    void cancel_prefetch(bool kill_running);
    std::vector<Prefetched> take_prefetched();

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

    // Prefetch state (guarded by req_mu_ unless atomic).
    std::vector<Request> prefetch_queue_;
    uint64_t prefetch_gen_ = 0;
    bool running_prefetch_ = false;        // the command in flight is a prefetch
    std::string running_command_;
    uint64_t promoted_version_ = 0;        // adopt the running prefetch as this request
    std::vector<Prefetched> prefetched_;

    mutable std::mutex content_mu_;
    Content content_;
};

} // namespace fzf
