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
        pending_ = std::move(r);
        has_request_ = true;
        v = version_.fetch_add(1, std::memory_order_acq_rel) + 1;
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
        version_.fetch_add(1, std::memory_order_acq_rel);
    }
    shell_kill(child_pid_.load(std::memory_order_acquire), SIGKILL);
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
        uint64_t my_version;
        {
            std::unique_lock<std::mutex> lk(req_mu_);
            req_cv_.wait(lk, [&] { return has_request_ || stop_; });
            if (stop_) return;
            r = std::move(pending_);
            has_request_ = false;
            my_version = version_.load(std::memory_order_acquire);
        }

        // fzf: cmd.Stderr = cmd.Stdout for the previewer.
        SpawnedCommand sp = shell_spawn(executor_, r.command, r.env, /*null_stdin=*/true,
                                        /*setpgid=*/true, /*stderr_to_stdout=*/true);
        if (sp.fd < 0) {
            {
                std::lock_guard<std::mutex> lock(content_mu_);
                content_ = Content{"Failed to start the preview command", my_version, true};
            }
            remove_files(r.temp_files);
            if (wake_) wake_();
            continue;
        }
        child_pid_.store(sp.pid, std::memory_order_release);
        // A request that arrived between the version load and the spawn
        // has already tried to kill a pid that was not published yet.
        if (version_.load(std::memory_order_acquire) != my_version) {
            shell_kill(sp.pid, SIGKILL);
        }

        std::string accumulated;
        char buf[8192];
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(content_mu_);
            content_ = Content{"", my_version, false};
        }
        for (;;) {
            ssize_t n = read(sp.fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            if (version_.load(std::memory_order_acquire) != my_version) {
                stale = true;
                break;
            }
            accumulated.append(buf, static_cast<size_t>(n));
            {
                std::lock_guard<std::mutex> lock(content_mu_);
                if (content_.version == my_version) {
                    content_.text = accumulated;
                }
            }
            if (wake_) wake_();
        }
        if (version_.load(std::memory_order_acquire) != my_version) stale = true;

        close(sp.fd);
        child_pid_.store(-1, std::memory_order_release);
        if (stale) shell_kill(sp.pid, SIGKILL);
        shell_wait(sp.pid);
        remove_files(r.temp_files);

        if (!stale) {
            {
                std::lock_guard<std::mutex> lock(content_mu_);
                if (content_.version == my_version) {
                    content_.text = std::move(accumulated);
                    content_.complete = true;
                }
            }
            if (wake_) wake_();
        }
    }
}

} // namespace fzf
