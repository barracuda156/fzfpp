#include "search.hpp"

#include <utf8.h>

#include <algorithm>
#include <cstring>

namespace fzf {

namespace {

constexpr uint32_t kQueryCacheMax = kChunkSize / 2;   // fzf: queryCacheMax
constexpr uint32_t kMergerCacheMax = 100000;          // fzf: mergerCacheMax
constexpr size_t kMergerCacheEntries = 32;            // ours: bound the merger cache
constexpr size_t kPatternCacheEntries = 512;          // ours: bound the pattern cache
constexpr size_t kChunkCacheEntries = 256;            // ours: keys kept per chunk

std::string ascii_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::vector<CodePoint> to_code_points(const std::string& s) {
    std::vector<CodePoint> out;
    out.reserve(s.size());
    try {
        utf8::utf8to32(s.begin(), s.end(), std::back_inserter(out));
    } catch (...) {
        out.clear();
        for (unsigned char c : s) out.push_back(c);
    }
    return out;
}

inline uint16_t as_u16(long v) {
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return static_cast<uint16_t>(v);
}

// fzf: util/chars.go TrimLength -- rune count without leading/trailing
// whitespace.
template <class Text>
uint16_t trim_length(const Text& t) {
    long n = static_cast<long>(t.size());
    long i = n - 1;
    for (; i >= 0; --i) {
        if (!is_unicode_space(t[static_cast<size_t>(i)])) break;
    }
    if (i < 0) return 0;
    long j = 0;
    for (; j < n; ++j) {
        if (!is_unicode_space(t[static_cast<size_t>(j)])) break;
    }
    return as_u16(i - j + 1);
}

// fzf: result.go buildResultFromBounds
template <class Text>
uint64_t rank_for(const std::vector<Criterion>& criteria, const MatchResult& r, const Text& text) {
    uint16_t points[4] = {0, 0, 0, 0};
    const bool valid = r.begin >= 0 && r.begin < r.end;
    const long min_begin = r.begin;
    const long min_end = r.min_end;
    const long max_end = r.end;
    const long n = static_cast<long>(text.size());

    for (size_t k = 0; k < criteria.size() && k < 4; ++k) {
        uint16_t val = 0xFFFF;
        switch (criteria[k]) {
            case Criterion::Score:
                // Higher is better
                val = static_cast<uint16_t>(0xFFFF - as_u16(r.score));
                break;
            case Criterion::Chunk:
                if (valid) {
                    long b = min_begin;
                    long e = max_end;
                    for (; b >= 1; --b) {
                        if (is_unicode_space(text[static_cast<size_t>(b - 1)])) break;
                    }
                    for (; e < n; ++e) {
                        if (is_unicode_space(text[static_cast<size_t>(e)])) break;
                    }
                    val = as_u16(e - b);
                }
                break;
            case Criterion::Length:
                val = trim_length(text);
                break;
            case Criterion::Pathname:
                if (valid) {
                    // Rune index, to be comparable with minBegin
                    long last_delim = -1;
                    for (long i = n - 1; i >= 0; --i) {
                        CodePoint c = text[static_cast<size_t>(i)];
                        if (c == '/' || c == '\\') { last_delim = i; break; }
                    }
                    if (last_delim <= min_begin) val = as_u16(min_begin - last_delim);
                }
                break;
            case Criterion::Begin:
            case Criterion::End:
                if (valid) {
                    long white_prefix = 0;
                    for (long idx = 0; idx < n; ++idx) {
                        white_prefix = idx;
                        if (idx == min_begin || !is_unicode_space(text[static_cast<size_t>(idx)])) break;
                    }
                    if (criteria[k] == Criterion::Begin) {
                        val = as_u16(min_end - white_prefix);
                    } else {
                        long tl = trim_length(text);
                        val = as_u16(65535 - 65535 * (max_end - white_prefix) / (tl + 1));
                    }
                }
                break;
        }
        points[3 - k] = val;
    }
    return (static_cast<uint64_t>(points[3]) << 48) | (static_cast<uint64_t>(points[2]) << 32) |
           (static_cast<uint64_t>(points[1]) << 16) | static_cast<uint64_t>(points[0]);
}

// fzf: radixSortResults -- LSD radix sort on the rank, stable, so equal
// ranks keep input (index) order; with --tac equal-rank runs are reversed.
void sort_results(std::vector<Result>& a, bool tac) {
    size_t n = a.size();
    if (n < 128) {
        std::stable_sort(a.begin(), a.end(),
                         [](const Result& x, const Result& y) { return x.rank < y.rank; });
    } else {
        std::vector<Result> buf(n);
        Result* src = a.data();
        Result* dst = buf.data();
        int scattered = 0;
        uint64_t key_or = 0;
        for (size_t i = 0; i < n; ++i) key_or |= src[i].rank;
        for (int pass = 0; pass < 8; ++pass) {
            unsigned shift = static_cast<unsigned>(pass) * 8;
            if (((key_or >> shift) & 0xff) == 0) continue;
            size_t count[256] = {0};
            for (size_t i = 0; i < n; ++i) count[(src[i].rank >> shift) & 0xff]++;
            if (count[(src[0].rank >> shift) & 0xff] == n) continue;
            size_t offset[256];
            offset[0] = 0;
            for (int i = 1; i < 256; ++i) offset[i] = offset[i - 1] + count[i - 1];
            for (size_t i = 0; i < n; ++i) {
                unsigned b = (src[i].rank >> shift) & 0xff;
                dst[offset[b]++] = src[i];
            }
            std::swap(src, dst);
            ++scattered;
        }
        if (scattered % 2 == 1) std::memcpy(a.data(), src, n * sizeof(Result));
    }
    if (tac) {
        size_t i = 0;
        while (i < n) {
            size_t j = i + 1;
            while (j < n && a[j].rank == a[i].rank) ++j;
            if (j - i > 1) std::reverse(a.begin() + static_cast<long>(i), a.begin() + static_cast<long>(j));
            i = j;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Pattern (fzf: BuildPattern)
// ---------------------------------------------------------------------------

Pattern::Pattern(const Options& opts, const std::string& query, bool cacheable)
    : matcher_(opts.case_mode, opts.algo, !opts.fuzzy, opts.normalize) {
    const bool fuzzy = opts.fuzzy;
    std::string as_string;
    if (opts.extended) {
        size_t b = query.find_first_not_of(' ');
        as_string = (b == std::string::npos) ? std::string() : query.substr(b);
        while (!as_string.empty() && as_string.back() == ' ' &&
               !(as_string.size() >= 2 && as_string[as_string.size() - 2] == '\\')) {
            as_string.pop_back();
        }
        matcher_.set_pattern(as_string);
        empty_ = matcher_.empty();
        // We should not sort the result if there are only inverse search
        // terms; if the query contains inverse terms or OR operators, we
        // cannot cache the search scope.
        sortable_ = false;
        cacheable_ = cacheable;
        for (const TermSet& set : matcher_.terms()) {
            for (size_t idx = 0; idx < set.size(); ++idx) {
                const PatternTerm& term = set[idx];
                if (!term.inverse) sortable_ = true;
                if (!cacheable_ || idx > 0 || term.inverse ||
                    (fuzzy && term.type != PatternTerm::Type::Fuzzy) ||
                    (!fuzzy && term.type != PatternTerm::Type::Exact)) {
                    cacheable_ = false;
                }
            }
        }
        // fzf: buildCacheKey -- the cacheable terms joined by tab.
        for (const TermSet& set : matcher_.terms()) {
            if (set.size() == 1 && !set[0].inverse &&
                (fuzzy || set[0].type == PatternTerm::Type::Exact)) {
                if (!cache_key_.empty()) cache_key_ += '\t';
                std::string term_text;
                utf8::utf32to8(set[0].text.begin(), set[0].text.end(), std::back_inserter(term_text));
                cache_key_ += term_text;
            }
        }
    } else {
        // fzf: the non-extended branch -- the whole query is one term.
        as_string = query;
        empty_ = as_string.empty();
        sortable_ = true;
        cacheable_ = cacheable;
        cache_key_ = as_string;
        if (!empty_) {
            PatternTerm term;
            term.type = fuzzy ? PatternTerm::Type::Fuzzy : PatternTerm::Type::Exact;
            term.case_sensitive = Matcher::term_case_sensitive(opts.case_mode, as_string);
            term.text = to_code_points(term.case_sensitive ? as_string : ascii_lower(as_string));
            std::vector<TermSet> sets;
            sets.push_back(TermSet{term});
            matcher_.set_terms(std::move(sets));
        }
    }
    text_ = as_string;
}

MatchResult Pattern::match(const Chunk& c, uint32_t idx, std::vector<uint32_t>* positions) const {
    const Item& it = c.items[idx];
    size_t nth_count = 0;
    const RuneRange* nth = c.nth_ranges_of(idx, nth_count);
    return matcher_.match(c.text(it), (it.flags & kItemAscii) != 0, nth, nth_count, positions);
}

// ---------------------------------------------------------------------------
// ChunkCache (fzf: cache.go)
// ---------------------------------------------------------------------------

void ChunkCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    cache_.clear();
}

void ChunkCache::add(const Chunk& chunk, const std::string& key, const ChunkBitmap& bm, uint32_t match_count) {
    if (key.empty() || !chunk.sealed() || match_count > kQueryCacheMax) return;
    std::lock_guard<std::mutex> lock(mu_);
    QueryCache& qc = cache_[chunk.id];
    if (qc.size() >= kChunkCacheEntries) qc.clear();
    qc[key] = bm;
}

bool ChunkCache::lookup(const Chunk& chunk, const std::string& key, ChunkBitmap& out) const {
    if (key.empty() || !chunk.sealed()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cache_.find(chunk.id);
    if (it == cache_.end()) return false;
    auto jt = it->second.find(key);
    if (jt == it->second.end()) return false;
    out = jt->second;
    return true;
}

bool ChunkCache::search(const Chunk& chunk, const std::string& key, ChunkBitmap& out) const {
    if (key.empty() || !chunk.sealed()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cache_.find(chunk.id);
    if (it == cache_.end()) return false;
    const QueryCache& qc = it->second;
    std::string sub;
    for (size_t idx = 1; idx < key.size(); ++idx) {
        // [---------| ] | [ |---------]
        // [--------|  ] | [  |--------]
        sub.assign(key, 0, key.size() - idx);
        auto jt = qc.find(sub);
        if (jt != qc.end()) { out = jt->second; return true; }
        sub.assign(key, idx, std::string::npos);
        jt = qc.find(sub);
        if (jt != qc.end()) { out = jt->second; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Rank
// ---------------------------------------------------------------------------

uint64_t compute_rank(const std::vector<Criterion>& criteria, const MatchResult& r,
                      std::string_view text, bool ascii) {
    if (ascii) {
        AsciiText t{text.data(), text.size()};
        return rank_for(criteria, r, t);
    }
    const std::vector<CodePoint>& runes = Matcher::scratch_runes();
    RuneText t{runes.data(), runes.size()};
    return rank_for(criteria, r, t);
}

// ---------------------------------------------------------------------------
// Merger (fzf: merger.go)
// ---------------------------------------------------------------------------

Merger::Merger() : pass_(true) {}

Merger::Merger(ChunkList::Snapshot snap, bool tac)
    : snap_(std::move(snap)), tac_(tac), pass_(true) {
    offsets_.reserve(snap_.counts.size() + 1);
    uint32_t acc = 0;
    for (uint32_t c : snap_.counts) {
        offsets_.push_back(acc);
        acc += c;
    }
    offsets_.push_back(acc);
    count_ = acc;
}

Merger::Merger(ChunkList::Snapshot snap, std::shared_ptr<const Pattern> pattern,
               std::vector<std::vector<Result>> lists, bool sorted, bool tac)
    : snap_(std::move(snap)), pattern_(std::move(pattern)), sorted_(sorted), tac_(tac), pass_(false) {
    count_ = 0;
    for (const auto& l : lists) count_ += static_cast<uint32_t>(l.size());
    if (lists.size() == 1) {
        merged_ = std::move(lists[0]);
    } else {
        lists_ = std::move(lists);
        cursors_.assign(lists_.size(), 0);
        merged_.reserve(count_);
    }
}

Result Merger::merged_get(uint32_t i) const {
    if (lists_.empty()) return merged_[i];
    std::lock_guard<std::mutex> lock(merge_mu_);
    auto index_of = [this](const Result& r) { return snap_.chunks[r.chunk]->items[r.idx].index; };
    while (merged_.size() <= i) {
        size_t min_list = SIZE_MAX;
        for (size_t k = 0; k < lists_.size(); ++k) {
            if (cursors_[k] >= lists_[k].size()) continue;
            if (min_list == SIZE_MAX) { min_list = k; continue; }
            const Result& cand = lists_[k][cursors_[k]];
            const Result& best = lists_[min_list][cursors_[min_list]];
            bool better;
            if (sorted_) {
                // fzf: compareRanks
                if (cand.rank != best.rank) {
                    better = cand.rank < best.rank;
                } else {
                    better = (index_of(cand) <= index_of(best)) != tac_;
                }
            } else {
                better = index_of(cand) < index_of(best);
            }
            if (better) min_list = k;
        }
        if (min_list == SIZE_MAX) break;   // cannot happen for i < count_
        merged_.push_back(lists_[min_list][cursors_[min_list]++]);
    }
    return merged_[i];
}

Result Merger::at(uint32_t i) const {
    if (!sorted_ && tac_) i = count_ - 1 - i;
    return merged_get(i);
}

ItemRef Merger::get(uint32_t i) const {
    if (i >= count_) return ItemRef{};
    if (pass_) {
        if (tac_) i = count_ - 1 - i;
        auto it = std::upper_bound(offsets_.begin(), offsets_.end(), i);
        size_t k = static_cast<size_t>(it - offsets_.begin()) - 1;
        return ItemRef{snap_.chunks[k], i - offsets_[k]};
    }
    Result r = at(i);
    return ItemRef{snap_.chunks[r.chunk], r.idx};
}

int64_t Merger::find(uint32_t item_index) const {
    if (pass_) {
        for (size_t k = 0; k < snap_.chunks.size(); ++k) {
            const Chunk& c = *snap_.chunks[k];
            if (item_index < c.first_index) return -1;
            if (item_index - c.first_index < snap_.counts[k]) {
                uint32_t pos = offsets_[k] + (item_index - c.first_index);
                return tac_ ? static_cast<int64_t>(count_ - 1 - pos) : static_cast<int64_t>(pos);
            }
        }
        return -1;
    }
    for (uint32_t i = 0; i < count_; ++i) {
        Result r = at(i);
        if (snap_.chunks[r.chunk]->items[r.idx].index == item_index) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Searcher (fzf: matcher.go)
// ---------------------------------------------------------------------------

Searcher::Searcher(const Options& opts, bool interactive)
    : opts_(opts), interactive_(interactive), tac_(opts.tac) {}

Searcher::~Searcher() {
    {
        std::lock_guard<std::mutex> lock(req_mu_);
        stop_ = true;
        cancel_.store(true);
    }
    req_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Searcher::set_wake_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(res_mu_);
    wake_ = std::move(cb);
}

void Searcher::start() {
    if (!thread_.joinable()) thread_ = std::thread(&Searcher::loop, this);
}

void Searcher::request(ChunkList::Snapshot snap, const std::string& query, bool final, bool sort) {
    {
        std::lock_guard<std::mutex> lock(req_mu_);
        req_.snap = std::move(snap);
        req_.query = query;
        req_.final = final;
        req_.sort = sort;
        has_req_ = true;
        cancel_.store(true);
    }
    req_cv_.notify_one();
}

std::shared_ptr<Merger> Searcher::take() {
    std::lock_guard<std::mutex> lock(res_mu_);
    return std::move(result_);
}

std::shared_ptr<const Pattern> Searcher::build_pattern(const std::string& query) {
    std::lock_guard<std::mutex> lock(pattern_mu_);
    auto it = patterns_.find(query);
    if (it != patterns_.end()) return it->second;
    if (patterns_.size() >= kPatternCacheEntries) patterns_.clear();
    auto p = std::make_shared<const Pattern>(opts_, query, interactive_);
    patterns_.emplace(query, p);
    return p;
}

void Searcher::clear_cache() {
    cache_.clear();
    // The merger cache lives on the worker thread; it invalidates itself
    // on the next request because the list id changes.
}

std::shared_ptr<Merger> Searcher::scan_sync(const ChunkList::Snapshot& snap, const std::string& query,
                                            bool final, bool sort) {
    auto m = scan(snap, build_pattern(query), sort, nullptr);
    m->final = final;
    return m;
}

void Searcher::loop() {
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> lk(req_mu_);
            req_cv_.wait(lk, [&] { return has_req_ || stop_; });
            if (stop_) return;
            r = std::move(req_);
            has_req_ = false;
            cancel_.store(false);
        }

        if (r.snap.list_id != cache_list_id_ || r.snap.count != cache_count_ || r.sort != cache_sort_) {
            merger_cache_.clear();
            cache_list_id_ = r.snap.list_id;
            cache_count_ = r.snap.count;
            cache_sort_ = r.sort;
        }

        auto pattern = build_pattern(r.query);
        std::shared_ptr<Merger> m;
        auto it = merger_cache_.find(pattern->text());
        if (it != merger_cache_.end()) m = it->second;
        if (!m) {
            m = scan(r.snap, pattern, r.sort, &cancel_);
            if (!m) continue;   // superseded by a newer request
            if (interactive_ && m->size() < kMergerCacheMax) {
                if (merger_cache_.size() >= kMergerCacheEntries) merger_cache_.clear();
                merger_cache_[pattern->text()] = m;
            }
        }
        m->final = r.final;
        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> lock(res_mu_);
            result_ = m;
            wake = wake_;
        }
        if (wake) wake();
    }
}

std::shared_ptr<Merger> Searcher::scan(const ChunkList::Snapshot& snap, std::shared_ptr<const Pattern> pattern,
                                       bool sort, const std::atomic<bool>* cancel) {
    if (snap.chunks.empty() || pattern->empty()) {
        return std::make_shared<Merger>(snap, tac_);
    }
    const bool sorted = sort && pattern->sortable();
    std::vector<Result> list;
    for (size_t ci = 0; ci < snap.chunks.size(); ++ci) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return nullptr;
        match_chunk(*snap.chunks[ci], snap.counts[ci], static_cast<uint32_t>(ci), *pattern, list);
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) return nullptr;
    if (sorted) sort_results(list, tac_);
    std::vector<std::vector<Result>> lists;
    lists.push_back(std::move(list));
    return std::make_shared<Merger>(snap, std::move(pattern), std::move(lists), sorted, tac_);
}

// fzf: Pattern.Match + matchChunk
void Searcher::match_chunk(const Chunk& chunk, uint32_t count, uint32_t chunk_no,
                           const Pattern& pattern, std::vector<Result>& out) {
    const std::string& key = pattern.cache_key();
    // Only a sealed chunk whose snapshot count is complete can use or
    // feed the cache (a bitmap must describe every item of the chunk).
    const bool complete = chunk.sealed() && count == chunk.size();
    ChunkBitmap cached;
    bool have = false;
    if (interactive_ && complete && !key.empty()) {
        if (pattern.cacheable()) have = cache_.lookup(chunk, key, cached);
        if (!have) have = cache_.search(chunk, key, cached);
    }

    ChunkBitmap bm;
    uint32_t matches = 0;
    for (uint32_t idx = 0; idx < count; ++idx) {
        if (have && !cached.test(idx)) continue;
        MatchResult r = pattern.match(chunk, idx);
        if (!r.matched) continue;
        bm.set(idx);
        ++matches;
        const Item& it = chunk.items[idx];
        uint64_t rank = compute_rank(opts_.criteria, r, chunk.text(it), (it.flags & kItemAscii) != 0);
        out.push_back(Result{chunk_no, idx, rank});
    }
    if (interactive_ && complete && pattern.cacheable()) {
        cache_.add(chunk, key, bm, matches);
    }
}

} // namespace fzf
