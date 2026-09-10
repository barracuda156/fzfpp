#pragma once

// Matching engine on top of the chunk list. Ports, in order of appearance:
//   fzf: src/pattern.go  BuildPattern / CacheKey / Match / matchChunk
//   fzf: src/cache.go    ChunkCache (per-chunk query bitmaps)
//   fzf: src/result.go   buildResult (tiebreak points), compareRanks
//   fzf: src/merger.go   PassMerger / Merger
//   fzf: src/matcher.go  Matcher.Loop / scan (the worker thread)
// See docs/DESIGN.md section 8. The string algorithms themselves live in
// matcher.{hpp,cpp}.

#include "chunklist.hpp"
#include "matcher.hpp"
#include "options.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fzf {

// fzf: Pattern. Built once per distinct query string (see
// Searcher::build_pattern for the cache), immutable afterwards, safe to
// share between threads.
class Pattern {
public:
    // `cacheable` is fzf's `opts.Filter == nil`: chunk results are only
    // cached in interactive mode.
    Pattern(const Options& opts, const std::string& query, bool cacheable);

    const std::string& text() const { return text_; }
    bool empty() const { return empty_; }
    bool sortable() const { return sortable_; }
    bool cacheable() const { return cacheable_; }
    const std::string& cache_key() const { return cache_key_; }
    const Matcher& matcher() const { return matcher_; }

    // Match one item of a chunk; `positions` (optional) receives the sorted
    // codepoint indices of the matched characters, for highlighting.
    MatchResult match(const Chunk& c, uint32_t idx,
                      std::vector<uint32_t>* positions = nullptr) const;

private:
    Matcher matcher_;
    std::string text_;
    bool empty_ = true;
    bool sortable_ = true;
    bool cacheable_ = false;
    std::string cache_key_;
};

// fzf: ChunkBitmap -- one bit per item of a chunk.
struct ChunkBitmap {
    std::array<uint64_t, kChunkBitWords> bits{};
    bool test(uint32_t i) const { return (bits[i / 64] >> (i % 64)) & 1u; }
    void set(uint32_t i) { bits[i / 64] |= uint64_t{1} << (i % 64); }
};

// fzf: ChunkCache. Keyed by the chunk's unique id (never reused, so a
// freed and reallocated chunk can never hit a stale entry).
class ChunkCache {
public:
    void clear();
    void add(const Chunk& chunk, const std::string& key, const ChunkBitmap& bm, uint32_t match_count);
    bool lookup(const Chunk& chunk, const std::string& key, ChunkBitmap& out) const;
    // Longest cached prefix or suffix of `key` (fzf: Search).
    bool search(const Chunk& chunk, const std::string& key, ChunkBitmap& out) const;

private:
    using QueryCache = std::unordered_map<std::string, ChunkBitmap>;
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, QueryCache> cache_;
};

// One matched item. `rank` is fzf's points[4] packed into one integer
// (lower sorts first); ties are broken by input order.
struct Result {
    uint32_t chunk;   // index into the snapshot's chunk vector
    uint32_t idx;     // item index within that chunk
    uint64_t rank;
};

// fzf: result.go buildResultFromBounds. `text`/`ascii` describe the item's
// text; for a non-ASCII item the matcher's scratch buffer must still hold
// its decoded runes (i.e. call this right after Pattern::match).
uint64_t compute_rank(const std::vector<Criterion>& criteria, const MatchResult& r,
                      std::string_view text, bool ascii);

// fzf: Merger. Owns the snapshot its results point into.
class Merger {
public:
    // Empty merger.
    Merger();
    // fzf: PassMerger -- every item, in input order (reversed with --tac).
    Merger(ChunkList::Snapshot snap, bool tac);
    // fzf: NewMerger -- one result list per worker, each in input order and,
    // when `sorted`, sorted by rank.
    Merger(ChunkList::Snapshot snap, std::shared_ptr<const Pattern> pattern,
           std::vector<std::vector<Result>> lists, bool sorted, bool tac);

    uint32_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool pass() const { return pass_; }
    bool tac() const { return tac_; }
    ItemRef get(uint32_t i) const;
    // Position of the item with the given input ordinal, or -1.
    int64_t find(uint32_t item_index) const;
    const Pattern* pattern() const { return pattern_.get(); }
    const ChunkList::Snapshot& snapshot() const { return snap_; }
    // fzf: Merger.final -- built after the reader finished.
    bool final = false;

private:
    Result at(uint32_t i) const;
    Result merged_get(uint32_t i) const;

    ChunkList::Snapshot snap_;
    std::shared_ptr<const Pattern> pattern_;
    std::vector<uint32_t> offsets_;              // pass mode: prefix sums of counts
    std::vector<std::vector<Result>> lists_;
    mutable std::vector<Result> merged_;         // lazy k-way merge (or lists_[0] moved in)
    mutable std::vector<size_t> cursors_;
    mutable std::mutex merge_mu_;
    uint32_t count_ = 0;
    bool sorted_ = false;
    bool tac_ = false;
    bool pass_ = false;
};

// fzf: Matcher (the coordinator side). One worker thread scans the chunks
// of the latest request and publishes a Merger; a newer request cancels
// the in-flight scan at the next chunk boundary. Only the latest request
// is ever served (coalescing).
class Searcher {
public:
    // `interactive` enables the chunk and merger caches (fzf: Filter == nil).
    Searcher(const Options& opts, bool interactive);
    ~Searcher();

    // Called from the worker when a result is ready (must be cheap and
    // thread-safe, e.g. a self-pipe write).
    void set_wake_callback(std::function<void()> cb);

    // Starts the worker thread.
    void start();

    // Main thread: replace the pending request. `final` is "the reader has
    // finished" (fzf's !reading); `sort` is the live --sort state.
    void request(ChunkList::Snapshot snap, const std::string& query, bool final, bool sort);

    // Main thread: the latest completed merger, once; nullptr if nothing
    // new since the last take().
    std::shared_ptr<Merger> take();

    // Scan on the calling thread (filter mode, --select-1/--exit-0, and
    // the "query then accept in one paste" case).
    std::shared_ptr<Merger> scan_sync(const ChunkList::Snapshot& snap, const std::string& query,
                                      bool final, bool sort);

    // Drop every cached chunk bitmap and merger (a reload replaced the list).
    void clear_cache();

    std::shared_ptr<const Pattern> build_pattern(const std::string& query);

    bool tac() const { return tac_; }

private:
    struct Request {
        ChunkList::Snapshot snap;
        std::string query;
        bool final = false;
        bool sort = true;
    };

    void loop();
    std::shared_ptr<Merger> scan(const ChunkList::Snapshot& snap, std::shared_ptr<const Pattern> pattern,
                                 bool sort, const std::atomic<bool>* cancel);
    void match_chunk(const Chunk& chunk, uint32_t count, uint32_t chunk_no,
                     const Pattern& pattern, std::vector<Result>& out);

    const Options& opts_;
    bool interactive_;
    bool tac_;
    ChunkCache cache_;

    std::mutex pattern_mu_;
    std::unordered_map<std::string, std::shared_ptr<const Pattern>> patterns_;

    std::mutex req_mu_;
    std::condition_variable req_cv_;
    Request req_;
    bool has_req_ = false;
    bool stop_ = false;
    std::atomic<bool> cancel_{false};

    std::mutex res_mu_;
    std::shared_ptr<Merger> result_;
    std::function<void()> wake_;

    // fzf: mergerCache -- valid while the list, its count and the sort
    // setting are unchanged (worker thread only).
    std::unordered_map<std::string, std::shared_ptr<Merger>> merger_cache_;
    uint64_t cache_list_id_ = 0;
    uint32_t cache_count_ = 0;
    bool cache_sort_ = true;

    std::thread thread_;
};

} // namespace fzf
