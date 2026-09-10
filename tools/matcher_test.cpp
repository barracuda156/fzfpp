// Stress + unit tests for the rewritten extended-search matcher.
// Invariants (from project memory matcher-v2-dp-recurrence):
//  - subsequence exists  <=>  match returned (A2: score never disqualifies)
//  - positions: count == pattern length, strictly increasing, in bounds,
//    each highlighted char equals the pattern char (case-insensitive)
//  - 200k randomized iterations, bad=0, for BOTH V1 and V2
// Plus extended-search parsing/matching unit checks (A1) and known fzf
// score values (A3/A5).

#include "matcher.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace fzf;

// The tests were written against the item-owning Matcher API; this shim
// keeps them verbatim on top of the text-view API (T1.4/T1.5): an item is
// a string with an input index, a result carries the item, its score and
// {start,end} positions, and match_items() sorts the way fzf's default
// tiebreak chain (score, then length, then input order) does.
struct TItem {
    std::string text_;
    size_t index_;
    TItem(std::string s, size_t idx) : text_(std::move(s)), index_(idx) {}
    const std::string& text() const { return text_; }
    size_t index() const { return index_; }
};

struct TPos {
    uint32_t start;
    uint32_t end;
};

struct TResult {
    std::shared_ptr<TItem> item;
    int32_t score = 0;
    std::vector<TPos> positions;
};

class TestMatcher {
public:
    TestMatcher(CaseMode case_mode = CaseMode::Smart, AlgoType algo = AlgoType::FuzzyV2,
                bool exact = false)
        : m_(case_mode, algo, exact) {}

    TResult match(const std::shared_ptr<TItem>& item, const std::string& pattern) {
        if (pattern != last_pattern_ || !primed_) {
            m_.set_pattern(pattern);
            last_pattern_ = pattern;
            primed_ = true;
        }
        std::vector<uint32_t> pos;
        MatchResult r = m_.match(item->text(), &pos);
        TResult out;
        if (!r.matched) return out;
        out.item = item;
        out.score = r.score;
        for (uint32_t p : pos) out.positions.push_back(TPos{p, p + 1});
        return out;
    }

    std::vector<TResult> match_items(const std::vector<std::shared_ptr<TItem>>& items,
                                     const std::string& pattern) {
        std::vector<TResult> results;
        for (const auto& item : items) {
            TResult r = match(item, pattern);
            if (r.item) results.push_back(std::move(r));
        }
        if (m_.sortable()) {
            std::stable_sort(results.begin(), results.end(), [](const TResult& a, const TResult& b) {
                if (a.score != b.score) return a.score > b.score;
                size_t la = a.item->text().size(), lb = b.item->text().size();
                if (la != lb) return la < lb;
                return a.item->index() < b.item->index();
            });
        }
        return results;
    }

private:
    Matcher m_;
    std::string last_pattern_;
    bool primed_ = false;
};

static int failures = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                 \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
        }                                                                    \
    } while (0)

static std::shared_ptr<TItem> mk(const std::string& s, size_t idx = 0) {
    return std::make_shared<TItem>(s, idx);
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
}

// Oracle: case-insensitive ASCII subsequence test.
static bool is_subseq_ci(const std::string& text, const std::string& pat) {
    size_t t = 0;
    for (char pc : pat) {
        while (t < text.size() && lower(text[t]) != lower(pc)) ++t;
        if (t == text.size()) return false;
        ++t;
    }
    return true;
}

static void stress(AlgoType algo, const char* name) {
    std::mt19937 rng(0xf2f);
    // Alphabet with repeats, boundaries, camel transitions; NO extended-search
    // syntax chars and no spaces so the query is a single fuzzy term.
    const std::string text_alpha = "abcabcxyzXYZ_-/.,;:0123ABC";
    const std::string pat_alpha = "abcxyz013";  // lowercase => smart-case CI
    std::uniform_int_distribution<int> tlen(0, 80), plen(1, 8);

    long bad = 0, matches = 0, misses = 0;
    const int ITER = 200000;
    TestMatcher m(CaseMode::Smart, algo);
    for (int it = 0; it < ITER; ++it) {
        std::string text, pat;
        int tl = tlen(rng), pl = plen(rng);
        for (int i = 0; i < tl; ++i)
            text += text_alpha[rng() % text_alpha.size()];
        // Half the time, force-plant the pattern as a subsequence.
        for (int i = 0; i < pl; ++i)
            pat += pat_alpha[rng() % pat_alpha.size()];
        if (it % 2 == 0 && tl > 0) {
            size_t pos = 0;
            for (char pc : pat) {
                pos += rng() % (text.size() - pos + 1);
                if (pos >= text.size()) break;
                text[pos++] = pc;
            }
        }

        bool expect = is_subseq_ci(text, pat);
        auto item = mk(text);
        TResult r = m.match(item, pat);
        bool got = (r.item != nullptr);
        if (got != expect) {
            ++bad;
            if (bad <= 5)
                std::printf("  [%s] match mismatch text=\"%s\" pat=\"%s\" "
                            "expect=%d got=%d\n",
                            name, text.c_str(), pat.c_str(), expect, got);
            continue;
        }
        if (!got) { ++misses; continue; }
        ++matches;

        // Position invariants.
        if (r.positions.size() != pat.size()) {
            ++bad;
            if (bad <= 5)
                std::printf("  [%s] poscount text=\"%s\" pat=\"%s\" got=%zu\n",
                            name, text.c_str(), pat.c_str(),
                            r.positions.size());
            continue;
        }
        long prev = -1;
        bool ok = true;
        for (size_t k = 0; k < r.positions.size(); ++k) {
            long p = r.positions[k].start;
            if (p <= prev || p >= (long)text.size() ||
                lower(text[p]) != lower(pat[k])) {
                ok = false;
                break;
            }
            prev = p;
        }
        if (!ok) {
            ++bad;
            if (bad <= 5)
                std::printf("  [%s] badpos text=\"%s\" pat=\"%s\"\n", name,
                            text.c_str(), pat.c_str());
        }
        if (r.score < SCORE_MATCH * (int)pat.size() -
                          3 * (int)text.size() - 1000) {
            // sanity floor only; real check is match/no-match above
        }
    }
    std::printf("%s stress: iter=%d matches=%ld misses=%ld bad=%ld\n", name,
                ITER, matches, misses, bad);
    CHECK(bad == 0, "%s stress bad=%ld", name, bad);
}

int main() {
    // ---- A2 regression: gap decay must never turn a match into NOMATCH.
    {
        TestMatcher m;
        std::string t = "a" + std::string(60, 'x') + "b";
        auto r = m.match(mk(t), "ab");
        CHECK(r.item != nullptr, "A2: 'ab' vs a+60x+b must match");
        // H floors at 0, so the b re-seeds at bare SCORE_MATCH.
        CHECK(r.score == 16, "A2 score: want 16 got %d", r.score);
        CHECK(r.positions.size() == 2 && r.positions[0].start == 0 &&
                  r.positions[1].start == 61,
              "A2 positions");
    }

    // ---- A5: boundary-anchored run must propagate the first-char bonus.
    // fzf value verified in the review: "bar" vs "foo-bar" == 80.
    {
        TestMatcher m;
        auto r = m.match(mk("foo-bar"), "bar");
        CHECK(r.item && r.score == 80, "A5: foo-bar/bar want 80 got %d",
              r.item ? r.score : -1);
    }

    // ---- A3 gap model: -3 first gap column then -1.
    // "ab" vs "axxb": a(0) 16+10*2=36, gaps xx -3-1=-4, b 16+run? not
    // consecutive, bonus 0 => 36-4+16 = 48.
    {
        TestMatcher m;
        auto r = m.match(mk("axxb"), "ab");
        CHECK(r.item && r.score == 48, "A3: axxb/ab want 48 got %d",
              r.item ? r.score : -1);
        // Flat -3/column would give 36-6+16=46.
    }

    // ---- consecutive beats gapped at equal char content
    {
        TestMatcher m;
        auto ra = m.match(mk("abc"), "abc");
        auto rb = m.match(mk("axbxc"), "abc");
        CHECK(ra.item && rb.item && ra.score > rb.score,
              "consecutive > gapped");
    }

    // ---- A1: extended-search ---------------------------------------
    TestMatcher m;

    // space-AND
    CHECK(m.match(mk("foo baz bar"), "foo bar").item != nullptr,
          "AND both present");
    CHECK(m.match(mk("foo baz"), "foo bar").item == nullptr,
          "AND one missing");
    // order-independent AND
    CHECK(m.match(mk("bar foo"), "foo bar").item != nullptr,
          "AND order independent");

    // 'exact
    CHECK(m.match(mk("foobar"), "'oba").item != nullptr, "exact substring");
    CHECK(m.match(mk("obxa"), "'oba").item == nullptr,
          "exact must be contiguous");

    // ^prefix / suffix$
    CHECK(m.match(mk("foobar"), "^foo").item != nullptr, "prefix hit");
    CHECK(m.match(mk("xfoobar"), "^foo").item == nullptr, "prefix miss");
    CHECK(m.match(mk("foobar"), "bar$").item != nullptr, "suffix hit");
    CHECK(m.match(mk("barfoo"), "bar$").item == nullptr, "suffix miss");
    CHECK(m.match(mk("  foobar  "), "^foo").item != nullptr,
          "prefix skips leading ws");
    CHECK(m.match(mk("foobar  "), "bar$").item != nullptr,
          "suffix skips trailing ws");

    // ^equal$
    CHECK(m.match(mk("foo"), "^foo$").item != nullptr, "equal hit");
    CHECK(m.match(mk("foox"), "^foo$").item == nullptr, "equal miss long");
    CHECK(m.match(mk("fo"), "^foo$").item == nullptr, "equal miss short");

    // !negation
    CHECK(m.match(mk("hello"), "!bar").item != nullptr, "neg absent -> match");
    CHECK(m.match(mk("hello bar"), "!bar").item == nullptr,
          "neg present -> no match");
    CHECK(m.match(mk("foo baz"), "foo !bar").item != nullptr,
          "AND with neg ok");
    CHECK(m.match(mk("foo bar"), "foo !bar").item == nullptr,
          "AND with neg fails");
    // !^ anchored negation
    CHECK(m.match(mk("barfoo"), "!^bar").item == nullptr, "neg prefix hit");
    CHECK(m.match(mk("foobar"), "!^bar").item != nullptr, "neg prefix miss");

    // | OR
    CHECK(m.match(mk("has cpp here"), "cpp | rs").item != nullptr, "OR left");
    CHECK(m.match(mk("has rs here"), "cpp | rs").item != nullptr, "OR right");
    CHECK(m.match(mk("has go here"), "cpp | rs").item == nullptr, "OR none");
    // OR binds within the AND clause
    CHECK(m.match(mk("main.go x"), "x go | rb").item != nullptr,
          "AND then OR");

    // escaped space
    CHECK(m.match(mk("foo bar"), "foo\\ bar").item != nullptr,
          "escaped space literal hit");
    CHECK(m.match(mk("fooxbar"), "foo\\ bar").item == nullptr,
          "escaped space literal miss");

    // 'boundary' quoted-both-ends
    CHECK(m.match(mk("x foo y"), "'foo'").item != nullptr, "boundary hit");
    CHECK(m.match(mk("xfooy"), "'foo'").item == nullptr, "boundary miss");
    CHECK(m.match(mk("x/foo/y"), "'foo'").item != nullptr,
          "boundary at delimiters");

    // smart-case is per term
    CHECK(m.match(mk("FOO bar"), "foo bar").item != nullptr,
          "lc term matches uc text");
    CHECK(m.match(mk("foo bar"), "FOO bar").item == nullptr,
          "uc term is case-sensitive");
    CHECK(m.match(mk("FOO bar"), "FOO bar").item != nullptr,
          "uc term matches uc text");

    // whitespace-only query matches everything with score 0
    CHECK(m.match(mk("anything"), "   ").item != nullptr, "ws-only matches");

    // bare syntax chars degrade gracefully
    CHECK(m.match(mk("a$b"), "$").item != nullptr, "lone $ literal");
    CHECK(m.match(mk("anything"), "!").item != nullptr,
          "lone ! empty -> matches all");

    // ---- --exact mode flips
    {
        TestMatcher me(CaseMode::Smart, AlgoType::FuzzyV2, true);
        CHECK(me.match(mk("foobar"), "oba").item != nullptr,
              "exact-mode substring hit");
        CHECK(me.match(mk("obxa"), "oba").item == nullptr,
              "exact-mode no fuzzy");
        CHECK(me.match(mk("obxa"), "'oba").item != nullptr,
              "exact-mode 'term flips to fuzzy");
    }

    // ---- inverse-only query keeps score 0; ranking by tiebreak length
    {
        TestMatcher mm;
        auto items = std::vector<std::shared_ptr<TItem>>{
            mk("looooooooong abc", 0), mk("abc", 1), mk("zabck", 2)};
        auto rs = mm.match_items(items, "abc");
        CHECK(rs.size() == 3, "all three match");
        CHECK(rs.size() == 3 && rs[0].item->index() == 1,
              "shortest equal-score item first (len tiebreak)");
    }

    // ---- unsortable queries keep input order (fzf's sortable flag)
    {
        TestMatcher mm;
        auto items = std::vector<std::shared_ptr<TItem>>{
            mk("zzzzzzzz", 0), mk("a", 1), mk("mmmm", 2)};
        auto r1 = mm.match_items(items, "!q");
        CHECK(r1.size() == 3 && r1[0].item->index() == 0 &&
                  r1[1].item->index() == 1 && r1[2].item->index() == 2,
              "inverse-only query keeps input order");
        auto r2 = mm.match_items(items, "   ");
        CHECK(r2.size() == 3 && r2[0].item->index() == 0 &&
                  r2[1].item->index() == 1 && r2[2].item->index() == 2,
              "whitespace query keeps input order");
        auto r3 = mm.match_items(items, "");
        CHECK(r3.size() == 3 && r3[0].item->index() == 0 &&
                  r3[2].item->index() == 2,
              "empty query keeps input order");
        // A positive term alongside a negation IS sortable.
        auto items2 = std::vector<std::shared_ptr<TItem>>{
            mk("xxxxx m xxxxx", 0), mk("m", 1)};
        auto r4 = mm.match_items(items2, "m !q");
        CHECK(r4.size() == 2 && r4[0].item->index() == 1,
              "positive+negative query still sorts");
    }

    // ---- multibyte safety through the whole pipeline
    {
        TestMatcher mm;
        auto r = mm.match(mk("日本語 fzf 対応"), "fzf");
        CHECK(r.item != nullptr, "multibyte text fuzzy hit");
        auto r2 = mm.match(mk("日本語対応"), "日本");
        CHECK(r2.item != nullptr && r2.positions.size() == 2 &&
                  r2.positions[0].start == 0,
              "cjk pattern positions are codepoint indices");
        CHECK(mm.match(mk("caf\xC3\xA9 latte"), "caf\xC3\xA9").item != nullptr,
              "utf8 pattern matches utf8 text");
    }

    // ---- A6 latin normalization: accented text matches plain-ASCII query
    // and vice versa, in both V1 and V2, independent of case sensitivity.
    {
        TestMatcher mm;
        CHECK(mm.match(mk("caf\xC3\xA9"), "cafe").item != nullptr,
              "cafe matches caf\xC3\xA9 (V2, case-insensitive)");
        CHECK(mm.match(mk("\xC3\x84rzte"), "arzte").item != nullptr,
              "arzte matches \xC3\x84rzte (leading capital-accent)");
        CHECK(mm.match(mk("resume"), "r\xC3\xA9sum\xC3\xA9").item != nullptr,
              "accented pattern matches plain text");
        CHECK(mm.match(mk("na\xC3\xAFve"), "naive").item != nullptr,
              "naive matches na\xC3\xAFve");

        // Accent stripping is independent of case sensitivity: an uppercase
        // (case-sensitive, per smart-case) query still folds the accent.
        CHECK(mm.match(mk("CAF\xC3\x89"), "CAFE").item != nullptr,
              "smart-case-sensitive query still strips accents");
        CHECK(mm.match(mk("CAF\xC3\x89"), "cafe").item != nullptr,
              "lowercase query still matches accented+cased text");

        TestMatcher v1(CaseMode::Smart, AlgoType::FuzzyV1);
        CHECK(v1.match(mk("caf\xC3\xA9"), "cafe").item != nullptr,
              "cafe matches caf\xC3\xA9 (V1)");

        // Non-letter / unrelated codepoints outside the table pass through.
        CHECK(mm.match(mk("日本語"), "日本").item != nullptr,
              "non-latin text unaffected by normalize table");
    }

    // ---- V1 basic + same extended plumbing
    {
        TestMatcher v1(CaseMode::Smart, AlgoType::FuzzyV1);
        auto r = v1.match(mk("foo-bar"), "bar");
        CHECK(r.item && r.score == 80, "V1 foo-bar/bar want 80 got %d",
              r.item ? r.score : -1);
        CHECK(v1.match(mk("a" + std::string(60, 'x') + "b"), "ab").item !=
                  nullptr,
              "V1 long-gap still matches");
        CHECK(v1.match(mk("foo baz bar"), "foo bar").item != nullptr,
              "V1 AND");
    }

    stress(AlgoType::FuzzyV2, "V2");
    stress(AlgoType::FuzzyV1, "V1");

    if (failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
