// Unit tests for the item store and the matching engine (T1.4 / T1.5):
//   - chunklist: publication, sealing, item_at, index base
//   - ansi: extract_color / strip_ansi
//   - reader: record splitting, --read0, --header-lines diversion, cancel
//     while the producer is still open (must not block), --nth/--with-nth/
//     --accept-nth item building
//   - pattern: cache key, cacheable/sortable flags
//   - merger: order equals a full sort for random inputs; chunk-cache
//     prefix narrowing yields the same results as a cold scan for 10k
//     random query extensions; --tac; --no-sort; every tiebreak
//
// Build: part of the default CMake build; run via ctest or ./search_test.

#include "ansi.hpp"
#include "chunklist.hpp"
#include "options.hpp"
#include "reader.hpp"
#include "search.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace fzf;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Options opts_for(std::vector<std::string> args) {
    return parse_option_args(args, /*use_defaults=*/false);
}

// Feeds `lines` through the real reader (pipe -> reader thread -> builder ->
// chunk list) and waits for EOF.
static std::shared_ptr<ChunkList> make_list(const std::vector<std::string>& lines,
                                            const Options& opts, ItemBuilder& builder,
                                            Reader& reader) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    std::string data;
    for (const auto& l : lines) { data += l; data += '\n'; }
    auto list = reader.start_fd(fds[0]);
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = write(fds[1], data.data() + off, data.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    close(fds[1]);
    reader.wait();
    (void)opts;
    return list;
}

static std::vector<uint32_t> indices(const Merger& m) {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < m.size(); ++i) out.push_back(m.get(i).index());
    return out;
}

// ---------------------------------------------------------------------------

static void test_chunklist() {
    ChunkList list(/*with_aux=*/false, /*first_index=*/2);
    std::vector<std::string> texts;
    for (int i = 0; i < 3000; ++i) texts.push_back("item-" + std::to_string(i));
    for (const auto& t : texts) {
        ItemSpec spec;
        spec.text = t;
        spec.rune_len = static_cast<uint32_t>(t.size());
        spec.ascii = true;
        list.append(spec);
    }
    CHECK(list.count() == 3000);
    CHECK(!list.finished());
    auto snap = list.snapshot();
    CHECK(snap.count == 3000);
    CHECK(snap.chunks.size() >= 3);
    CHECK(snap.counts.size() == snap.chunks.size());
    CHECK(snap.chunks[0]->sealed());
    CHECK(!snap.chunks.back()->sealed());
    CHECK(snap.chunks[0]->id != snap.chunks[1]->id);
    list.finish();
    CHECK(list.finished());
    CHECK(snap.chunks.back()->sealed());

    ItemRef r = list.item_at(2);
    CHECK(r && r.text() == "item-0" && r.index() == 2);
    r = list.item_at(2 + 2999);
    CHECK(r && r.text() == "item-2999");
    CHECK(!list.item_at(1));
    CHECK(!list.item_at(2 + 3000));
    r = list.item_at(2 + 1500);
    CHECK(r && r.text() == "item-1500" && r.ascii());

    // A record larger than the default arena gets its own chunk.
    ChunkList big(false);
    std::string huge(200 * 1024, 'x');
    ItemSpec spec;
    spec.text = huge;
    spec.rune_len = static_cast<uint32_t>(huge.size());
    big.append(spec);
    spec.text = "small";
    spec.rune_len = 5;
    big.append(spec);
    CHECK(big.count() == 2);
    CHECK(big.item_at(0).text().size() == huge.size());
    CHECK(big.item_at(1).text() == "small");
}

static void test_ansi() {
    std::string out;
    std::vector<ColorRun> runs;
    bool ascii = true;
    uint32_t n = extract_color("\x1b[31mred\x1b[0m plain", out, runs, ascii);
    CHECK(out == "red plain");
    CHECK(n == 9 && ascii);
    CHECK(runs.size() == 1 && runs[0].start == 0 && runs[0].end == 3 && runs[0].fg == 1 && runs[0].bg == -1);

    n = extract_color("a\x1b[1;38;5;208mb\xC3\xA9\x1b[m", out, runs, ascii);
    CHECK(out == "ab\xC3\xA9");
    CHECK(n == 3 && !ascii);
    CHECK(runs.size() == 1 && runs[0].start == 1 && runs[0].end == 3 && runs[0].fg == 208 && (runs[0].attr & kAttrBold));

    // OSC and non-SGR CSI are removed without producing runs.
    extract_color("\x1b]0;title\x07x\x1b[2Ky", out, runs, ascii);
    CHECK(out == "xy" && runs.empty());
    CHECK(strip_ansi("\x1b[32mgreen\x1b[0m") == "green");
    CHECK(!has_escape("plain") && has_escape("\x1b[m"));
}

static void test_reader_basic() {
    Options opts = opts_for({});
    ItemBuilder builder(opts);
    Reader reader(opts, builder);
    auto list = make_list({"foo", "bar\r", "", "baz"}, opts, builder, reader);
    CHECK(list->count() == 4);
    CHECK(list->finished());
    CHECK(list->item_at(0).text() == "foo");
    CHECK(list->item_at(1).text() == "bar");     // one trailing \r trimmed
    CHECK(list->item_at(2).text().empty());      // empty lines are items
    CHECK(list->item_at(3).text() == "baz");
    CHECK(builder.output_text(list->item_at(3)) == "baz");
    CHECK(reader.header_lines().empty());

    // No trailing newline: the leftover is the last record.
    {
        int fds[2];
        CHECK(pipe(fds) == 0);
        auto l2 = reader.start_fd(fds[0]);
        const char* data = "one\ntwo";
        CHECK(write(fds[1], data, std::strlen(data)) == 7);
        close(fds[1]);
        reader.wait();
        CHECK(l2->count() == 2 && l2->item_at(1).text() == "two");
        CHECK(l2->id() != list->id());
    }
}

static void test_reader_read0() {
    Options opts = opts_for({"--read0"});
    ItemBuilder builder(opts);
    Reader reader(opts, builder);
    int fds[2];
    CHECK(pipe(fds) == 0);
    auto list = reader.start_fd(fds[0]);
    const char data[] = "a\nb\0c\r\0";
    CHECK(write(fds[1], data, sizeof(data) - 1) == static_cast<ssize_t>(sizeof(data) - 1));
    close(fds[1]);
    reader.wait();
    CHECK(list->count() == 2);
    CHECK(list->item_at(0).text() == "a\nb");
    CHECK(list->item_at(1).text() == "c\r");    // verbatim with --read0
}

static void test_reader_header_lines() {
    Options opts = opts_for({"--header-lines", "2"});
    ItemBuilder builder(opts);
    Reader reader(opts, builder);
    auto list = make_list({"H1", "H2", "body1", "body2"}, opts, builder, reader);
    auto header = reader.header_lines();
    CHECK(header.size() == 2 && header[0] == "H1" && header[1] == "H2");
    CHECK(list->count() == 2);
    CHECK(list->first_index() == 2);
    // fzf numbers items after the header lines ({n} compatibility).
    CHECK(list->item_at(2).text() == "body1" && list->item_at(2).index() == 2);
    CHECK(list->item_at(3).text() == "body2");
    CHECK(!list->item_at(0));
    auto snap = list->snapshot();
    Merger pass(snap, false);
    CHECK(pass.size() == 2 && pass.get(0).text() == "body1");
}

static void test_reader_cancel() {
    Options opts = opts_for({});
    ItemBuilder builder(opts);
    Reader reader(opts, builder);
    int fds[2];
    CHECK(pipe(fds) == 0);
    auto list = reader.start_fd(fds[0]);
    CHECK(write(fds[1], "a\n", 2) == 2);
    for (int i = 0; i < 200 && list->count() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(list->count() == 1);
    CHECK(!list->finished());
    // The producer is still open: cancel must return without waiting for
    // its EOF (the reload-while-streaming bug of the old getline loop).
    auto t0 = std::chrono::steady_clock::now();
    reader.cancel();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 500);
    CHECK(list->finished());
    CHECK(list->count() == 1);
    close(fds[1]);

    // A command source: reload replaces the list, the old one stays intact.
    auto l2 = reader.start_command("printf 'x\\ny\\n'");
    reader.wait();
    CHECK(l2->count() == 2 && l2->item_at(0).text() == "x" && l2->item_at(1).text() == "y");
    CHECK(list->count() == 1 && list->item_at(0).text() == "a");
    CHECK(reader.list() == l2);

    // Cancelling a command that never ends kills its process group.
    auto l3 = reader.start_command("while :; do echo x; sleep 0.01; done");
    for (int i = 0; i < 400 && l3->count() < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(l3->count() >= 5);
    t0 = std::chrono::steady_clock::now();
    reader.cancel();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000);
    CHECK(l3->finished());

    // A wake is delivered once per ack (coalescing).
    std::atomic<int> wakes{0};
    reader.set_wake_callback([&] { wakes.fetch_add(1); });
    auto l4 = reader.start_command("seq 1 5000");
    reader.wait();
    CHECK(l4->count() == 5000);
    CHECK(wakes.load() >= 1 && wakes.load() < 100);
    reader.ack_wake();
}

static void test_item_builder_nth() {
    {
        Options opts = opts_for({"--nth", "2", "-d", ":"});
        ItemBuilder builder(opts);
        CHECK(builder.with_aux());
        Reader reader(opts, builder);
        auto list = make_list({"a:bcd:e", "x", ""}, opts, builder, reader);
        ItemRef r = list->item_at(0);
        size_t n = 0;
        const RuneRange* ranges = r.nth_ranges(n);
        CHECK(n == 1 && ranges && ranges[0].start == 2 && ranges[0].len == 4);   // "bcd:"
        CHECK(r.text() == "a:bcd:e");                  // display text unchanged
        r = list->item_at(1);
        ranges = r.nth_ranges(n);
        CHECK(n == 1 && ranges && ranges[0].len == 0);   // no field 2: matches nothing
        CHECK(builder.output_text(list->item_at(0)) == "a:bcd:e");
        CHECK(builder.field_text(list->item_at(0), 2) == "bcd");
        CHECK(builder.field_text(list->item_at(0), 3) == "e");
        CHECK(builder.field_text(list->item_at(0), 4).empty());
    }
    {
        Options opts = opts_for({"--with-nth", "2..", "--accept-nth", "1"});
        ItemBuilder builder(opts);
        Reader reader(opts, builder);
        auto list = make_list({"k1 v1 v2  ", "k2"}, opts, builder, reader);
        ItemRef r = list->item_at(0);
        CHECK(r.text() == "v1 v2");             // trailing whitespace trimmed
        CHECK(r.orig_text() == "k1 v1 v2  ");
        CHECK(builder.original_text(r) == "k1 v1 v2  ");
        CHECK(builder.output_text(r) == "k1");
        CHECK(list->item_at(1).text().empty());
        CHECK(builder.output_text(list->item_at(1)) == "k2");
    }
    {
        Options opts = opts_for({"--ansi", "--accept-nth", "{2}/{1}"});
        ItemBuilder builder(opts);
        Reader reader(opts, builder);
        auto list = make_list({"\x1b[31mred\x1b[0m blue"}, opts, builder, reader);
        ItemRef r = list->item_at(0);
        CHECK(r.text() == "red blue");
        size_t n = 0;
        const ColorRun* runs = r.chunk->colors_of(r.idx, n);
        CHECK(n == 1 && runs && runs[0].start == 0 && runs[0].end == 3 && runs[0].fg == 1);
        CHECK(builder.original_text(r) == "red blue");
        CHECK(builder.output_text(r) == "blue/red");
    }
    {
        Options opts = opts_for({"--ansi", "--with-nth", "2"});
        ItemBuilder builder(opts);
        Reader reader(opts, builder);
        auto list = make_list({"\x1b[31mred\x1b[0m blue"}, opts, builder, reader);
        ItemRef r = list->item_at(0);
        CHECK(r.text() == "blue");
        CHECK(builder.output_text(r) == "red blue");   // original, ANSI-stripped
    }
    bool threw = false;
    try {
        Options opts = opts_for({"--with-nth", "abc"});
        ItemBuilder builder(opts);
    } catch (const OptionError& e) {
        threw = true;
        CHECK(e.message == "template should include at least 1 placeholder: abc");
    }
    CHECK(threw);
}

static void test_pattern() {
    Options opts = opts_for({});
    Pattern p1(opts, "foo bar", true);
    CHECK(p1.cache_key() == "foo\tbar" && p1.cacheable() && p1.sortable() && !p1.empty());
    Pattern p2(opts, "foo !bar", true);
    CHECK(p2.cache_key() == "foo" && !p2.cacheable() && p2.sortable());
    Pattern p3(opts, "!bar", true);
    CHECK(p3.cache_key().empty() && !p3.cacheable() && !p3.sortable() && !p3.empty());
    Pattern p4(opts, "a | b", true);
    CHECK(p4.cache_key().empty() && !p4.cacheable() && p4.sortable());
    Pattern p5(opts, "  ", true);
    CHECK(p5.empty() && p5.text().empty());
    Pattern p6(opts, " foo ", true);
    CHECK(p6.text() == "foo");
    // fzf: an exact term still contributes to the key in fuzzy mode (its
    // matches are a subset of the fuzzy prefix's), but is never cached.
    Pattern p7(opts, "'foo", true);
    CHECK(!p7.cacheable() && p7.cache_key() == "foo");
    Pattern p8(opts, "foo", false);
    CHECK(!p8.cacheable() && p8.cache_key() == "foo");

    Options exact = opts_for({"--exact"});
    Pattern p9(exact, "foo", true);
    CHECK(p9.cacheable() && p9.cache_key() == "foo");
    Pattern p10(exact, "'foo", true);
    CHECK(!p10.cacheable());

    Options plain = opts_for({"--no-extended"});
    Pattern p11(plain, "foo bar", true);
    CHECK(p11.text() == "foo bar" && p11.cacheable() && p11.cache_key() == "foo bar");
    CHECK(p11.matcher().match("xfoo barx").matched);
    CHECK(p11.matcher().match("foo  bar").matched);   // still fuzzy: a subsequence
    CHECK(!p11.matcher().match("foobar").matched);    // the space is part of the term
    CHECK(!p11.matcher().match("bar foo").matched);   // not two AND-ed terms
}

// Brute-force order: every matching item sorted by (rank, index).
static std::vector<uint32_t> brute_order(const ChunkList::Snapshot& snap, const Pattern& p,
                                         const Options& opts, bool sort, bool tac) {
    struct Entry { uint64_t rank; uint32_t index; };
    std::vector<Entry> entries;
    for (size_t c = 0; c < snap.chunks.size(); ++c) {
        const Chunk& chunk = *snap.chunks[c];
        for (uint32_t i = 0; i < snap.counts[c]; ++i) {
            MatchResult r = p.match(chunk, i);
            if (!r.matched) continue;
            const Item& it = chunk.items[i];
            uint64_t rank = compute_rank(opts.criteria, r, chunk.text(it), (it.flags & kItemAscii) != 0);
            entries.push_back({rank, it.index});
        }
    }
    if (sort && p.sortable()) {
        std::sort(entries.begin(), entries.end(), [tac](const Entry& a, const Entry& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return tac ? a.index > b.index : a.index < b.index;
        });
    } else if (tac) {
        std::reverse(entries.begin(), entries.end());
    }
    std::vector<uint32_t> out;
    for (const auto& e : entries) out.push_back(e.index);
    return out;
}

static std::vector<std::string> random_lines(std::mt19937& rng, int n) {
    const std::string alpha = "abcabcxyz/_-. ABC0123";
    std::vector<std::string> lines;
    std::uniform_int_distribution<int> len(0, 24);
    for (int i = 0; i < n; ++i) {
        std::string s;
        int l = len(rng);
        for (int k = 0; k < l; ++k) s += alpha[rng() % alpha.size()];
        lines.push_back(s);
    }
    return lines;
}

static void test_merger_matches_full_sort() {
    std::mt19937 rng(42);
    for (const char* scheme : {"default", "path", "history"}) {
        for (bool tac : {false, true}) {
            std::vector<std::string> args = {"--scheme", scheme};
            if (tac) args.push_back("--tac");
            Options opts = opts_for(args);
            ItemBuilder builder(opts);
            Reader reader(opts, builder);
            auto list = make_list(random_lines(rng, 2500), opts, builder, reader);
            auto snap = list->snapshot();
            Searcher searcher(opts, /*interactive=*/false);
            const std::string qalpha = "abcxyz ";
            for (int it = 0; it < 60; ++it) {
                std::string q;
                int l = 1 + static_cast<int>(rng() % 4);
                for (int k = 0; k < l; ++k) q += qalpha[rng() % qalpha.size()];
                if (it % 10 == 9) q = "!a";           // unsortable
                if (it % 10 == 4) q = "'b";           // exact
                auto pattern = searcher.build_pattern(q);
                auto m = searcher.scan_sync(snap, q, true, true);
                auto expect = brute_order(snap, *pattern, opts, true, tac);
                auto got = indices(*m);
                CHECK(got == expect);
                if (got != expect) {
                    std::fprintf(stderr, "  scheme=%s tac=%d query=\"%s\" got %zu expect %zu\n",
                                 scheme, tac, q.c_str(), got.size(), expect.size());
                }
                // The rank order is by score first: verify it never increases.
                if (!m->pass() && pattern->sortable()) {
                    int32_t prev = INT32_MAX;
                    for (uint32_t i = 0; i < m->size(); ++i) {
                        ItemRef r = m->get(i);
                        MatchResult mr = pattern->match(*r.chunk, r.idx);
                        CHECK(mr.matched && mr.score <= prev);
                        prev = mr.score;
                    }
                }
            }
            // Empty query: input order (pass merger), reversed with --tac.
            auto m = searcher.scan_sync(snap, "", true, true);
            CHECK(m->pass() && m->size() == 2500);
            CHECK(m->get(0).index() == (tac ? 2499u : 0u));
            CHECK(m->get(2499).index() == (tac ? 0u : 2499u));
            CHECK(m->find(1234) == (tac ? 2500 - 1 - 1234 : 1234));
            CHECK(m->find(9999) == -1);
            // --no-sort: matching items in input order (reversed with --tac).
            auto ns = searcher.scan_sync(snap, "a", true, false);
            auto pattern = searcher.build_pattern("a");
            CHECK(indices(*ns) == brute_order(snap, *pattern, opts, false, tac));
            if (ns->size() > 1) {
                CHECK(ns->find(ns->get(1).index()) == 1);
            }
        }
    }
}

static void test_prefix_narrowing() {
    std::mt19937 rng(7);
    Options opts = opts_for({});
    ItemBuilder builder(opts);
    Reader reader(opts, builder);
    auto list = make_list(random_lines(rng, 1500), opts, builder, reader);
    auto snap = list->snapshot();
    CHECK(snap.chunks.size() >= 2);
    for (const auto& c : snap.chunks) CHECK(c->sealed());

    Searcher warm(opts, /*interactive=*/true);    // uses the chunk cache
    Searcher cold(opts, /*interactive=*/false);   // never caches
    const std::string qalpha = "abcxyz";
    int hits = 0;
    for (int it = 0; it < 10000; ++it) {
        std::string q;
        int l = 1 + static_cast<int>(rng() % 3);
        for (int k = 0; k < l; ++k) q += qalpha[rng() % qalpha.size()];
        // Extend by one character (occasionally by a second term).
        std::string ext = q + qalpha[rng() % qalpha.size()];
        if (it % 7 == 0) ext = q + " " + qalpha[rng() % qalpha.size()];
        if (it % 11 == 0) ext = qalpha[rng() % qalpha.size()] + q;   // prefix on the other side
        auto base = warm.scan_sync(snap, q, true, true);
        auto narrowed = warm.scan_sync(snap, ext, true, true);
        auto reference = cold.scan_sync(snap, ext, true, true);
        CHECK(indices(*narrowed) == indices(*reference));
        if (narrowed->size() > 0) ++hits;
        (void)base;
    }
    CHECK(hits > 100);

    // The cache is populated: a direct lookup for a cacheable key hits.
    ChunkCache cache;
    ChunkBitmap bm;
    bm.set(3);
    cache.add(*snap.chunks[0], "ab", bm, 1);
    ChunkBitmap out;
    CHECK(cache.lookup(*snap.chunks[0], "ab", out) && out.test(3) && !out.test(4));
    CHECK(!cache.lookup(*snap.chunks[0], "abc", out));
    CHECK(cache.search(*snap.chunks[0], "abc", out) && out.test(3));     // prefix
    CHECK(cache.search(*snap.chunks[0], "xab", out) && out.test(3));     // suffix
    CHECK(!cache.search(*snap.chunks[0], "xyz", out));
    CHECK(!cache.lookup(*snap.chunks[1], "ab", out));
    cache.clear();
    CHECK(!cache.lookup(*snap.chunks[0], "ab", out));
}

static void test_tiebreaks() {
    auto rank = [](std::vector<std::string> args, const char* text, int32_t score,
                   int32_t begin, int32_t min_end, int32_t end) {
        Options opts = opts_for(args);
        MatchResult r;
        r.matched = true;
        r.score = score;
        r.begin = begin;
        r.min_end = min_end;
        r.end = end;
        return compute_rank(opts.criteria, r, text, true);
    };
    // score: higher score sorts first
    CHECK(rank({}, "abc", 50, 0, 1, 1) < rank({}, "abc", 40, 0, 1, 1));
    // length (default second criterion): shorter trimmed text first
    CHECK(rank({}, "  ab  ", 10, 2, 4, 4) < rank({}, "abcd", 10, 0, 2, 2));
    CHECK(rank({}, "abcd", 10, 0, 2, 2) == rank({}, "abcd  ", 10, 0, 2, 2));
    // begin: earlier match first
    CHECK(rank({"--tiebreak", "begin"}, "xxab", 10, 2, 4, 4) > rank({"--tiebreak", "begin"}, "abxx", 10, 0, 2, 2));
    // leading whitespace does not count against begin
    CHECK(rank({"--tiebreak", "begin"}, "  ab", 10, 2, 4, 4) == rank({"--tiebreak", "begin"}, "ab", 10, 0, 2, 2));
    // end: match ending later (relative to length) first
    CHECK(rank({"--tiebreak", "end"}, "abxx", 10, 0, 2, 2) > rank({"--tiebreak", "end"}, "xxab", 10, 2, 4, 4));
    // chunk: shorter whitespace-delimited chunk around the match first
    CHECK(rank({"--tiebreak", "chunk"}, "xx abc yy", 10, 4, 5, 5) < rank({"--tiebreak", "chunk"}, "xx abcdef yy", 10, 4, 5, 5));
    // pathname: match in the last path component first
    CHECK(rank({"--tiebreak", "pathname"}, "/a/b/foo", 10, 5, 8, 8) < rank({"--tiebreak", "pathname"}, "/a/foo/b", 10, 3, 6, 6));
    // index is the implicit last tiebreak: equal ranks
    CHECK(rank({"--tiebreak", "index"}, "zzz", 10, 0, 1, 1) == rank({"--tiebreak", "index"}, "a", 10, 0, 1, 1));
    // Non-ASCII text ranks by rune length (the matcher decoded it).
    {
        Options opts = opts_for({});
        Pattern p(opts, "b", true);
        std::string text = "\xC3\xA9\xC3\xA9" "b";     // "ééb": 3 runes, 5 bytes
        MatchResult r = p.matcher().match(text);
        CHECK(r.matched);
        uint64_t ru = compute_rank(opts.criteria, r, text, false);
        uint64_t ra = compute_rank(opts.criteria, r, "xxb", true);
        CHECK(ru == ra);
    }

    // End to end: the acceptance cases of T1.5.
    {
        Options opts = opts_for({});
        ItemBuilder builder(opts);
        Reader reader(opts, builder);
        auto list = make_list({"xxab", "ab"}, opts, builder, reader);
        Searcher s(opts, false);
        auto sorted = s.scan_sync(list->snapshot(), "ab", true, true);
        CHECK(sorted->size() == 2 && sorted->get(0).text() == "ab");
        auto unsorted = s.scan_sync(list->snapshot(), "ab", true, false);   // +s
        CHECK(unsorted->size() == 2 && unsorted->get(0).text() == "xxab");
    }
    {
        Options opts = opts_for({"--nth", "2"});
        ItemBuilder builder(opts);
        Reader reader(opts, builder);
        auto list = make_list({"foo bar", "bar foo"}, opts, builder, reader);
        Searcher s(opts, false);
        auto m = s.scan_sync(list->snapshot(), "^foo", true, true);
        CHECK(m->size() == 1 && m->get(0).text() == "bar foo");
        // Positions are reported on the whole line.
        std::vector<uint32_t> pos;
        ItemRef r = m->get(0);
        m->pattern()->match(*r.chunk, r.idx, &pos);
        CHECK(pos.size() == 3 && pos[0] == 4 && pos[2] == 6);
    }
    {
        Options opts = opts_for({"-d", "\\t", "--nth", "2"});
        ItemBuilder builder(opts);
        Reader reader(opts, builder);
        auto list = make_list({"a\tb"}, opts, builder, reader);
        Searcher s(opts, false);
        CHECK(s.scan_sync(list->snapshot(), "b", true, true)->size() == 1);
        CHECK(s.scan_sync(list->snapshot(), "a", true, true)->size() == 0);
    }
    {
        Options opts = opts_for({"--tac"});
        ItemBuilder builder(opts);
        Reader reader(opts, builder);
        auto list = make_list({"a", "b", "c"}, opts, builder, reader);
        Searcher s(opts, false);
        auto m = s.scan_sync(list->snapshot(), "", true, true);
        CHECK(m->size() == 3 && m->get(0).text() == "c" && m->get(2).text() == "a");
    }
}

static void test_searcher_thread() {
    Options opts = opts_for({});
    ItemBuilder builder(opts);
    Reader reader(opts, builder);
    std::mt19937 rng(3);
    auto list = make_list(random_lines(rng, 3000), opts, builder, reader);
    Searcher s(opts, true);
    std::mutex mu;
    std::condition_variable cv;
    int wakes = 0;
    s.set_wake_callback([&] {
        std::lock_guard<std::mutex> lock(mu);
        ++wakes;
        cv.notify_all();
    });
    s.start();
    // A burst of requests: only the latest must be answered (coalescing),
    // and its result must equal a synchronous scan.
    for (const char* q : {"a", "ab", "abc", "b"}) {
        s.request(list->snapshot(), q, true, true);
    }
    // Earlier requests may or may not have completed before being
    // superseded; the last one always arrives.
    std::shared_ptr<Merger> m;
    for (int i = 0; i < 200; ++i) {
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::milliseconds(20));
        }
        auto got = s.take();
        if (got) m = got;
        if (m && m->pattern() && m->pattern()->text() == "b") break;
    }
    CHECK(m != nullptr);
    if (m) {
        CHECK(m->final);
        CHECK(m->pattern() && m->pattern()->text() == "b");
        auto ref = s.scan_sync(list->snapshot(), "b", true, true);
        CHECK(indices(*m) == indices(*ref));
    }
    CHECK(s.take() == nullptr);
    // A cached merger is returned for a repeated query on the same list.
    s.request(list->snapshot(), "b", true, true);
    std::shared_ptr<Merger> again;
    for (int i = 0; i < 200 && !again; ++i) {
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::milliseconds(20));
        }
        again = s.take();
    }
    CHECK(again == m);
}

int main() {
    test_chunklist();
    test_ansi();
    test_reader_basic();
    test_reader_read0();
    test_reader_header_lines();
    test_reader_cancel();
    test_item_builder_nth();
    test_pattern();
    test_merger_matches_full_sort();
    test_prefix_narrowing();
    test_tiebreaks();
    test_searcher_thread();
    std::printf("search_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
