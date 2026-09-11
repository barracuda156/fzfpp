#include "preview.hpp"

#include "placeholder.hpp"
#include "shellcmd.hpp"

#include <cerrno>
#include <csignal>

#include <unistd.h>

namespace fzf {

PreviewWorker::PreviewWorker(const Executor& executor, std::function<void()> wake)
    : executor_(executor), wake_(std::move(wake)) {}

PreviewWorker::~PreviewWorker() { shutdown(); }

void PreviewWorker::start() {
    if (!thread_.joinable()) thread_ = std::thread(&PreviewWorker::loop, this);
}

void PreviewWorker::shutdown() {
    {
        std::lock_guard<std::mutex> lock(req_mu_);
        stop_ = true;
        has_request_ = false;
    }
    version_.fetch_add(1, std::memory_order_acq_rel);
    shell_kill(child_pid_.load(std::memory_order_acquire), SIGKILL);
    req_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

uint64_t PreviewWorker::request(Request r) {
    uint64_t v;
    {
        std::lock_guard<std::mutex> lock(req_mu_);
        if (has_request_) remove_files(pending_.temp_files);   // never ran
        has_request_ = false;
        v = version_.fetch_add(1, std::memory_order_acq_rel) + 1;
        // A prefetch of exactly this command line is already running: let
        // it finish as the real request instead of starting over.
        if (running_prefetch_ && running_command_ == r.command) {
            remove_files(r.temp_files);
            promoted_version_ = v;
            return v;
        }
        pending_ = std::move(r);
        has_request_ = true;
    }
    // The command for the previous target may block forever in a pipe read
    // (tail -f style); killing its process group gives the worker EOF.
    shell_kill(child_pid_.load(std::memory_order_acquire), SIGKILL);
    req_cv_.notify_one();
    return v;
}

void PreviewWorker::cancel() {
    {
        std::lock_guard<std::mutex> lock(req_mu_);
        if (has_request_) remove_files(pending_.temp_files);
        has_request_ = false;
        for (auto& q : prefetch_queue_) remove_files(q.temp_files);
        prefetch_queue_.clear();
        ++prefetch_gen_;
        version_.fetch_add(1, std::memory_order_acq_rel);
    }
    shell_kill(child_pid_.load(std::memory_order_acquire), SIGKILL);
}

void PreviewWorker::prefetch(std::vector<Request> requests) {
    {
        std::lock_guard<std::mutex> lock(req_mu_);
        for (auto& q : prefetch_queue_) remove_files(q.temp_files);
        prefetch_queue_ = std::move(requests);
    }
    req_cv_.notify_one();
}

void PreviewWorker::cancel_prefetch(bool kill_running) {
    pid_t to_kill = -1;
    {
        std::lock_guard<std::mutex> lock(req_mu_);
        for (auto& q : prefetch_queue_) remove_files(q.temp_files);
        prefetch_queue_.clear();
        if (kill_running && running_prefetch_) {
            ++prefetch_gen_;
            to_kill = child_pid_.load(std::memory_order_acquire);
        }
    }
    shell_kill(to_kill, SIGKILL);
}

std::vector<PreviewWorker::Prefetched> PreviewWorker::take_prefetched() {
    std::lock_guard<std::mutex> lock(req_mu_);
    std::vector<Prefetched> out;
    out.swap(prefetched_);
    return out;
}

void PreviewWorker::set_content(std::string text, bool complete) {
    std::lock_guard<std::mutex> lock(content_mu_);
    content_.text = std::move(text);
    content_.complete = complete;
    content_.version = version_.load(std::memory_order_acquire);
}

PreviewWorker::Content PreviewWorker::content() const {
    std::lock_guard<std::mutex> lock(content_mu_);
    return content_;
}

void PreviewWorker::loop() {
    for (;;) {
        Request r;
        uint64_t my_version = 0;
        uint64_t my_pgen = 0;
        bool is_prefetch = false;
        {
            std::unique_lock<std::mutex> lk(req_mu_);
            req_cv_.wait(lk, [&] { return has_request_ || !prefetch_queue_.empty() || stop_; });
            if (stop_) return;
            if (has_request_) {
                r = std::move(pending_);
                has_request_ = false;
            } else {
                r = std::move(prefetch_queue_.front());
                prefetch_queue_.erase(prefetch_queue_.begin());
                is_prefetch = true;
                my_pgen = prefetch_gen_;
            }
            my_version = version_.load(std::memory_order_acquire);
            running_prefetch_ = is_prefetch;
            running_command_ = r.command;
            promoted_version_ = 0;
        }
        // Whether this run is dead: a newer real request (kills either
        // kind) or, for a prefetch, a cancel_prefetch().
        auto superseded = [&]() {
            if (version_.load(std::memory_order_acquire) != my_version) return true;
            if (is_prefetch) {
                std::lock_guard<std::mutex> lock(req_mu_);
                return prefetch_gen_ != my_pgen;
            }
            return false;
        };
        // A prefetch adopted by the real request for the same command.
        auto check_promotion = [&](const std::string& so_far) {
            if (!is_prefetch) return;
            uint64_t promoted = 0;
            {
                std::lock_guard<std::mutex> lock(req_mu_);
                if (promoted_version_ != 0) {
                    promoted = promoted_version_;
                    promoted_version_ = 0;
                    running_prefetch_ = false;
                }
            }
            if (promoted == 0) return;
            is_prefetch = false;
            my_version = promoted;
            {
                std::lock_guard<std::mutex> lock(content_mu_);
                content_ = Content{so_far, my_version, false};
            }
            if (wake_) wake_();
        };

        // fzf: cmd.Stderr = cmd.Stdout for the previewer.
        SpawnedCommand sp = shell_spawn(executor_, r.command, r.env, /*null_stdin=*/true,
                                        /*setpgid=*/true, /*stderr_to_stdout=*/true);
        if (sp.fd < 0) {
            if (!is_prefetch) {
                std::lock_guard<std::mutex> lock(content_mu_);
                content_ = Content{"Failed to start the preview command", my_version, true};
            }
            remove_files(r.temp_files);
            std::lock_guard<std::mutex> lock(req_mu_);
            running_prefetch_ = false;
            if (wake_) wake_();
            continue;
        }
        child_pid_.store(sp.pid, std::memory_order_release);
        // A request that arrived between the version load and the spawn
        // has already tried to kill a pid that was not published yet.
        if (superseded()) shell_kill(sp.pid, SIGKILL);

        std::string accumulated;
        char buf[8192];
        bool stale = false;
        if (!is_prefetch) {
            // Keep the previous output on screen until the new command
            // prints something (fzf keeps the pane until then, too).
            std::lock_guard<std::mutex> lock(content_mu_);
            content_.version = my_version;
            content_.complete = false;
        }
        for (;;) {
            ssize_t n = read(sp.fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            check_promotion(accumulated);
            if (superseded()) {
                stale = true;
                break;
            }
            accumulated.append(buf, static_cast<size_t>(n));
            if (!is_prefetch) {
                {
                    std::lock_guard<std::mutex> lock(content_mu_);
                    if (content_.version == my_version) content_.text = accumulated;
                }
                if (wake_) wake_();
            }
        }
        check_promotion(accumulated);
        if (superseded()) stale = true;

        close(sp.fd);
        child_pid_.store(-1, std::memory_order_release);
        if (stale) shell_kill(sp.pid, SIGKILL);
        shell_wait(sp.pid);
        remove_files(r.temp_files);

        {
            std::lock_guard<std::mutex> lock(req_mu_);
            running_prefetch_ = false;
            running_command_.clear();
            if (!stale && is_prefetch) prefetched_.push_back(Prefetched{r.command, std::move(accumulated)});
        }
        if (!stale && !is_prefetch) {
            std::lock_guard<std::mutex> lock(content_mu_);
            if (content_.version == my_version) {
                content_.text = std::move(accumulated);
                content_.complete = true;
            }
        }
        if (!stale && wake_) wake_();
    }
}

} // namespace fzf
