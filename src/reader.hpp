#pragma once

// Input reader (fzf: src/reader.go) and item builder (the closures in
// src/core.go Run: ansiProcessor, transformItem, buildItemTransformer).
// See docs/DESIGN.md section 7.
//
// One reader thread select()s on {source fd, cancel pipe}, splits the
// stream on '\n' (or '\0' with --read0) in a 64 KiB buffer, builds each
// record into an Item and appends it to the current ChunkList generation.
// `reload` cancels the thread through the pipe (and kills the command's
// process group), joins it, and starts a fresh generation -- it never
// waits for a producer's EOF.

#include "chunklist.hpp"
#include "options.hpp"
#include "tokenizer.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace fzf {

// Turns raw records into items and items back into output text.
// build() is only ever called from the reader thread; the const members
// are safe from any thread.
class ItemBuilder {
public:
    // Throws OptionError for an invalid --with-nth / --accept-nth template
    // (fzf reports these at option-parsing time).
    explicit ItemBuilder(const Options& opts);

    // True when items carry side data (--ansi colors, --with-nth original
    // text, --nth ranges); the ChunkList allocates the aux tables then.
    bool with_aux() const { return with_aux_; }

    // Reader thread: one raw record -> ItemSpec. The views point into the
    // builder's scratch buffers and stay valid until the next build().
    // `index` is the item's input ordinal ({n} in a --with-nth template).
    const ItemSpec& build(std::string_view raw, uint32_t index);

    // fzf: Item.AsString(stripAnsi) -- the original line, ANSI-stripped
    // when --ansi (what {} expands to and what accept starts from).
    std::string original_text(const ItemRef& ref) const;

    // fzf: buildItemTransformer -- what accept prints for the item:
    // original_text() through --accept-nth.
    std::string output_text(const ItemRef& ref) const;

    // The 1-based field of the item's original line, without its
    // delimiter (for the {N} placeholders until T1.6 ports the grammar).
    std::string field_text(const ItemRef& ref, int field) const;

    const Delimiter& delimiter() const { return delimiter_; }
    bool ansi() const { return ansi_; }

private:
    bool ansi_;
    Delimiter delimiter_;
    Tokenizer tokenizer_;
    std::vector<Range> nth_;
    std::unique_ptr<NthTransformer> with_nth_;
    std::unique_ptr<NthTransformer> accept_nth_;
    bool with_aux_ = false;

    // Reader-thread scratch.
    std::string stripped_;
    std::string transformed_;
    std::vector<ColorRun> colors_;
    std::vector<RuneRange> nth_ranges_;
    std::vector<Token> tokens_;
    ItemSpec spec_;
};

class Reader {
public:
    Reader(const Options& opts, ItemBuilder& builder);
    ~Reader();

    // Called (from the reader thread) when new items were published or the
    // source finished. Wakes are coalesced: after a wake, no further wake
    // is sent until the consumer calls ack_wake().
    void set_wake_callback(std::function<void()> cb);
    void ack_wake() { wake_pending_.store(false, std::memory_order_release); }

    // Start reading `fd` (ownership taken) into a fresh ChunkList generation.
    // Any read in flight is cancelled first.
    std::shared_ptr<ChunkList> start_fd(int fd);
    // Start reading the stdout of `command` run under $SHELL -c in its own
    // process group (fzf: readFromCommand).
    std::shared_ptr<ChunkList> start_command(const std::string& command,
                                             const std::vector<std::string>& env = {});

    // Stop the read in flight: cancel pipe, SIGKILL the command's process
    // group, join. The current generation is marked finished.
    void cancel();
    // Wait for the current read to reach EOF (--sync, filter mode).
    void wait();

    std::shared_ptr<ChunkList> list() const;
    bool finished() const;
    // The --header-lines records diverted from the current generation.
    std::vector<std::string> header_lines() const;

private:
    std::shared_ptr<ChunkList> start(int fd, pid_t pid);
    void run(int fd, pid_t pid, std::shared_ptr<ChunkList> list);
    void wake();

    const Options& opts_;
    ItemBuilder& builder_;

    mutable std::mutex mu_;
    std::shared_ptr<ChunkList> list_;
    std::vector<std::string> header_;

    std::thread thread_;
    int cancel_r_ = -1;
    int cancel_w_ = -1;
    std::atomic<pid_t> child_pid_{-1};

    std::atomic<bool> wake_pending_{false};
    std::mutex wake_mu_;
    std::function<void()> wake_cb_;
};

} // namespace fzf
