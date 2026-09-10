#include "chunklist.hpp"

#include <algorithm>
#include <cstring>

namespace fzf {

namespace {
std::atomic<uint64_t> g_next_id{1};
}

Chunk::Chunk(size_t arena_bytes, bool with_aux)
    : id(g_next_id.fetch_add(1, std::memory_order_relaxed)),
      arena(new char[arena_bytes]), arena_cap(arena_bytes) {
    items.reserve(kChunkSize);
    if (with_aux) {
        aux.reserve(kChunkSize);
        colors.reserve(kChunkColorRuns);
        nth_ranges.reserve(kChunkNthRanges);
    }
}

ChunkList::ChunkList(bool with_aux, uint32_t first_index)
    : with_aux_(with_aux), first_index_(first_index),
      id_(g_next_id.fetch_add(1, std::memory_order_relaxed)) {}

void ChunkList::start_chunk(size_t min_arena) {
    size_t cap = std::max(kChunkArenaBytes, min_arena);
    auto chunk = std::make_shared<Chunk>(cap, with_aux_);
    chunk->first_index = first_index_ + count_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    if (tail_) tail_->sealed_.store(true, std::memory_order_release);
    chunks_.push_back(chunk);
    tail_ = chunk;
}

uint32_t ChunkList::append(const ItemSpec& spec) {
    size_t need = spec.text.size() + (spec.has_orig ? spec.orig.size() : 0);
    bool fits = tail_ && tail_->items.size() < kChunkSize &&
                tail_->arena_used + need <= tail_->arena_cap &&
                (!with_aux_ || (tail_->colors.size() + spec.color_count <= kChunkColorRuns &&
                                tail_->nth_ranges.size() + spec.nth_count <= kChunkNthRanges));
    if (!fits) {
        start_chunk(need);
    }
    // A single record with more runs/ranges than a chunk can hold keeps
    // its text and drops the extras (pathological input).
    Chunk& c = *tail_;
    Item it;
    it.offset = static_cast<uint32_t>(c.arena_used);
    it.len = static_cast<uint32_t>(spec.text.size());
    it.index = first_index_ + count_.load(std::memory_order_relaxed);
    it.rune_len = spec.rune_len;
    it.flags = spec.ascii ? static_cast<uint32_t>(kItemAscii) : 0u;
    if (!spec.text.empty()) {
        std::memcpy(c.arena.get() + c.arena_used, spec.text.data(), spec.text.size());
        c.arena_used += spec.text.size();
    }
    if (with_aux_) {
        ItemAux a;
        if (spec.color_count > 0 && spec.color_count <= kChunkColorRuns) {
            a.color_begin = static_cast<uint32_t>(c.colors.size());
            a.color_count = spec.color_count;
            c.colors.insert(c.colors.end(), spec.colors, spec.colors + spec.color_count);
            it.flags |= kItemHasColors;
        }
        if (spec.has_orig) {
            a.orig_offset = static_cast<uint32_t>(c.arena_used);
            a.orig_len = static_cast<uint32_t>(spec.orig.size());
            if (!spec.orig.empty()) {
                std::memcpy(c.arena.get() + c.arena_used, spec.orig.data(), spec.orig.size());
                c.arena_used += spec.orig.size();
            }
            it.flags |= kItemHasOrig;
        }
        if (spec.nth_count > 0 && spec.nth_count <= kChunkNthRanges) {
            a.nth_begin = static_cast<uint32_t>(c.nth_ranges.size());
            a.nth_count = spec.nth_count;
            c.nth_ranges.insert(c.nth_ranges.end(), spec.nth, spec.nth + spec.nth_count);
            it.flags |= kItemHasNth;
        }
        c.aux.push_back(a);
    }
    c.items.push_back(it);
    // Publish: the item and its bytes are complete before count moves.
    c.count.store(static_cast<uint32_t>(c.items.size()), std::memory_order_release);
    uint32_t ordinal = it.index - first_index_;
    count_.store(ordinal + 1, std::memory_order_release);
    return it.index;
}

void ChunkList::finish() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (tail_) tail_->sealed_.store(true, std::memory_order_release);
    }
    finished_.store(true, std::memory_order_release);
}

ChunkList::Snapshot ChunkList::snapshot() const {
    Snapshot s;
    {
        std::lock_guard<std::mutex> lock(mu_);
        s.chunks = chunks_;
    }
    // Record what is visible in the snapshot (each chunk's published
    // count); consumers use these counts, not the live ones, so a merger
    // built from the snapshot is internally consistent.
    s.counts.reserve(s.chunks.size());
    uint32_t n = 0;
    for (const auto& c : s.chunks) {
        uint32_t cnt = c->size();
        s.counts.push_back(cnt);
        n += cnt;
    }
    s.count = n;
    s.finished = finished();
    s.list_id = id_;
    return s;
}

ItemRef ChunkList::item_at(uint32_t index) const {
    if (index < first_index_) return ItemRef{};
    std::shared_ptr<Chunk> found;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Chunks are in index order; binary search on first_index.
        auto it = std::upper_bound(chunks_.begin(), chunks_.end(), index,
            [](uint32_t idx, const std::shared_ptr<Chunk>& c) { return idx < c->first_index; });
        if (it == chunks_.begin()) return ItemRef{};
        found = *(it - 1);
    }
    uint32_t local = index - found->first_index;
    if (local >= found->size()) return ItemRef{};
    return ItemRef{std::move(found), local};
}

} // namespace fzf
