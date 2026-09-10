#pragma once

// Item storage: a list of fixed-capacity chunks (fzf: src/chunklist.go,
// src/item.go), designed for the memory budget in docs/DESIGN.md section 6
// and for lock-free publication from the reader thread to the matcher.
//
// Every chunk owns a fixed byte arena and fixed-capacity vectors that never
// reallocate once the chunk is created; the reader fills a chunk in order
// and publishes each new item by storing `count` with release semantics.
// Consumers read `count` with acquire semantics and only touch items below
// it. When a record does not fit in the remaining arena (or the item /
// color-run capacity is exhausted) the chunk is sealed and a new one is
// started, so chunks hold *up to* kChunkSize items. Item text is stored
// once, as bytes (ANSI-stripped when --ansi); UTF-32 decoding happens in
// the matcher's scratch buffer only for non-ASCII items.

#include "item.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fzf {

constexpr uint32_t kChunkSize = 1024;             // max items per chunk (fzf: chunkSize)
constexpr uint32_t kChunkBitWords = kChunkSize / 64;
constexpr size_t kChunkArenaBytes = 64 * 1024;    // default arena per chunk
constexpr uint32_t kChunkColorRuns = 4096;        // color-run capacity per chunk
constexpr uint32_t kChunkNthRanges = 4096;        // --nth range capacity per chunk

// One SGR run inside an item's (stripped) text, in codepoint offsets.
// Colors use the encoding of options.hpp (kColorDefault / ANSI 0-255 /
// (1<<24)|RGB); attr uses the kAttr* bits.
struct ColorRun {
    uint32_t start;   // codepoint offset of the first colored character
    uint32_t end;     // exclusive
    int32_t fg;
    int32_t bg;
    uint32_t attr;
};

enum ItemFlags : uint32_t {
    kItemAscii = 1u << 0,        // text is pure ASCII (byte == codepoint)
    kItemHasColors = 1u << 1,    // color runs recorded (see Chunk::aux)
    kItemHasOrig = 1u << 2,      // original text kept (--with-nth changed the text)
    kItemHasNth = 1u << 3,       // --nth ranges recorded
};

// 20 bytes. Text lives in the owning chunk's arena.
struct Item {
    uint32_t offset;     // byte offset of the text in Chunk::arena
    uint32_t len;        // byte length of the text
    uint32_t index;      // input ordinal (fzf: Item.Index), header lines included
    uint32_t rune_len;   // number of codepoints in text
    uint32_t flags;      // ItemFlags
};

// Optional per-item extras (only allocated when --ansi, --nth or --with-nth
// are in effect). aux[i] belongs to items[i].
struct ItemAux {
    uint32_t color_begin = 0;   // index into Chunk::colors
    uint32_t color_count = 0;
    uint32_t orig_offset = 0;   // original text (before --with-nth) in Chunk::arena
    uint32_t orig_len = 0;
    uint32_t nth_begin = 0;     // index into Chunk::nth_ranges
    uint32_t nth_count = 0;
};

struct Chunk {
    explicit Chunk(size_t arena_bytes, bool with_aux);

    // Consumer view (valid for i < count.load(acquire)).
    std::string_view text(const Item& it) const {
        return std::string_view(arena.get() + it.offset, it.len);
    }
    std::string_view text(uint32_t i) const { return text(items[i]); }
    // Text printed on accept: the original line when --with-nth changed
    // the displayed text, otherwise the text itself.
    std::string_view orig_text(uint32_t i) const {
        const Item& it = items[i];
        if (it.flags & kItemHasOrig) {
            const ItemAux& a = aux[i];
            return std::string_view(arena.get() + a.orig_offset, a.orig_len);
        }
        return text(it);
    }
    const RuneRange* nth_ranges_of(uint32_t i, size_t& count) const {
        if (!(items[i].flags & kItemHasNth)) { count = 0; return nullptr; }
        const ItemAux& a = aux[i];
        count = a.nth_count;
        return nth_ranges.data() + a.nth_begin;
    }
    const ColorRun* colors_of(uint32_t i, size_t& count) const {
        if (!(items[i].flags & kItemHasColors)) { count = 0; return nullptr; }
        const ItemAux& a = aux[i];
        count = a.color_count;
        return colors.data() + a.color_begin;
    }
    uint32_t size() const { return count.load(std::memory_order_acquire); }
    // A sealed chunk never changes again: its match results can be cached.
    bool sealed() const { return sealed_.load(std::memory_order_acquire); }

    uint64_t id;                    // process-unique, never reused (chunk cache key)
    std::unique_ptr<char[]> arena;
    size_t arena_cap;
    size_t arena_used = 0;
    std::vector<Item> items;        // reserved to kChunkSize, never reallocates
    std::vector<ItemAux> aux;       // reserved to kChunkSize when with_aux
    std::vector<ColorRun> colors;   // reserved to kChunkColorRuns when with_aux
    std::vector<RuneRange> nth_ranges;  // reserved to kChunkNthRanges when with_aux
    uint32_t first_index = 0;       // input ordinal of items[0]
    std::atomic<uint32_t> count{0};
    std::atomic<bool> sealed_{false};
};

// A reference to one item that keeps its chunk alive.
struct ItemRef {
    std::shared_ptr<Chunk> chunk;
    uint32_t idx = 0;

    explicit operator bool() const { return chunk != nullptr; }
    const Item& item() const { return chunk->items[idx]; }
    uint32_t index() const { return item().index; }
    std::string_view text() const { return chunk->text(item()); }
    std::string_view orig_text() const { return chunk->orig_text(idx); }
    bool ascii() const { return (item().flags & kItemAscii) != 0; }
    const RuneRange* nth_ranges(size_t& count) const { return chunk->nth_ranges_of(idx, count); }
    bool operator==(const ItemRef& o) const { return chunk == o.chunk && idx == o.idx; }
    bool operator!=(const ItemRef& o) const { return !(*this == o); }
};

// One raw input record turned into an item. The reader's ItemBuilder fills
// this (ANSI stripping, --with-nth transformation, --nth ranges); the views
// must stay valid until append() returns.
struct ItemSpec {
    std::string_view text;            // final (stripped, --with-nth applied) text
    uint32_t rune_len = 0;
    bool ascii = true;
    bool has_orig = false;            // keep `orig` (differs from text)
    std::string_view orig;            // original text for output
    const ColorRun* colors = nullptr;
    uint32_t color_count = 0;
    const RuneRange* nth = nullptr;   // --nth ranges of `text`
    uint32_t nth_count = 0;
};

class ChunkList {
public:
    // `first_index`: input ordinal of the first item (fzf numbers items
    // after the --header-lines records, so {n} stays compatible).
    explicit ChunkList(bool with_aux, uint32_t first_index = 0);

    // Producer (one thread). Returns the item's input ordinal.
    uint32_t append(const ItemSpec& spec);
    void finish();

    // Consumers (any thread).
    struct Snapshot {
        std::vector<std::shared_ptr<Chunk>> chunks;   // in order
        std::vector<uint32_t> counts;                 // published count of each chunk at snapshot time
        uint32_t count = 0;                           // total published items
        bool finished = false;
        uint64_t list_id = 0;                         // owning ChunkList's id
    };
    Snapshot snapshot() const;
    uint32_t count() const { return count_.load(std::memory_order_acquire); }
    bool finished() const { return finished_.load(std::memory_order_acquire); }
    bool with_aux() const { return with_aux_; }
    uint32_t first_index() const { return first_index_; }
    uint64_t id() const { return id_; }

    // Locate an item by input ordinal. Returns a null ref when out of range.
    ItemRef item_at(uint32_t index) const;

private:
    void start_chunk(size_t min_arena);

    bool with_aux_;
    uint32_t first_index_;
    uint64_t id_;
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<Chunk>> chunks_;   // guarded by mu_
    std::shared_ptr<Chunk> tail_;                  // producer-owned growing chunk
    std::atomic<uint32_t> count_{0};
    std::atomic<bool> finished_{false};
};

} // namespace fzf
