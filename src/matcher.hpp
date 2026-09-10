#pragma once

// Fuzzy/exact matching algorithms (fzf: src/algo/algo.go) and extended
// search term parsing (fzf: src/pattern.go parseTerms). The algorithms
// are ported with score parity; see tools/matcher_test.cpp.
//
// The matcher works on text views instead of owned items: ASCII text is
// matched directly on its bytes, non-ASCII text is decoded into a
// thread-local scratch buffer per call. All scratch memory (decode buffer,
// bonus table, DP matrices) is thread-local and reused, so matching one
// item allocates nothing in steady state. `match()` is const and safe to
// call from several threads at once on the same Matcher.

#include "item.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fzf {

// Scoring constants (matching fzf's algo.go)
constexpr int32_t SCORE_MATCH = 16;
constexpr int32_t SCORE_GAP_START = -3;
constexpr int32_t SCORE_GAP_EXTENSION = -1;

constexpr int32_t BONUS_BOUNDARY = 8;             // scoreMatch / 2
constexpr int32_t BONUS_NON_WORD = 8;             // scoreMatch / 2
constexpr int32_t BONUS_BOUNDARY_WHITE = 10;      // bonusBoundary + 2
constexpr int32_t BONUS_BOUNDARY_DELIMITER = 9;   // bonusBoundary + 1
constexpr int32_t BONUS_CAMEL123 = 7;             // bonusBoundary + scoreGapExtension
constexpr int32_t BONUS_CONSECUTIVE = 4;          // -(scoreGapStart + scoreGapExtension)
constexpr int32_t BONUS_FIRST_CHAR_MULTIPLIER = 2;

// Character class for bonus calculation (fzf's charClass)
enum class CharClass {
    CharWhite,
    CharNonWord,
    CharDelimiter,
    CharLower,
    CharUpper,
    CharLetter,
    CharNumber
};

// One term of an extended-search query (fzf's term struct). The pattern
// string "foo bar$ !baz | 'qux" parses into AND-ed term sets, each set
// holding OR-ed alternatives:
//   foo        -> Fuzzy
//   'foo       -> Exact ('quoted flips exactness; under --exact it means Fuzzy)
//   'foo'      -> ExactBoundary (both ends on word boundaries)
//   ^foo       -> Prefix
//   foo$       -> Suffix
//   ^foo$      -> Equal
//   !foo       -> Exact, inverse (item must NOT contain it)
struct PatternTerm {
    enum class Type { Fuzzy, Exact, ExactBoundary, Prefix, Suffix, Equal };
    Type type = Type::Fuzzy;
    bool inverse = false;
    bool case_sensitive = false;  // smart-case is decided PER TERM, like fzf
    std::vector<CodePoint> text;
};
using TermSet = std::vector<PatternTerm>;

// Result of matching one text. `begin`/`end` are codepoint offsets of the
// matched region (min begin / max end over all terms), -1 when the query
// had no positive term. `min_end` is the smallest end over the terms
// (fzf's minEnd, used by the `begin` tiebreak).
struct MatchResult {
    bool matched = false;
    int32_t score = 0;
    int32_t begin = -1;
    int32_t end = -1;
    int32_t min_end = -1;
};

// Text views the algorithms are instantiated for.
struct AsciiText {
    const char* p;
    size_t n;
    size_t size() const { return n; }
    CodePoint operator[](size_t i) const { return static_cast<unsigned char>(p[i]); }
};
struct RuneText {
    const CodePoint* p;
    size_t n;
    size_t size() const { return n; }
    CodePoint operator[](size_t i) const { return p[i]; }
};

class Matcher {
public:
    Matcher(CaseMode case_mode = CaseMode::Smart,
            AlgoType algo = AlgoType::FuzzyV2,
            bool exact = false,
            bool normalize = true)
        : case_mode_(case_mode), algo_(algo), exact_(exact), normalize_(normalize) {}

    // Compile a query. Must be called before match(); not thread-safe
    // against concurrent match() calls.
    void set_pattern(const std::string& pattern);
    // Install pre-built term sets (fzf's non-extended mode builds a single
    // term from the whole query; see search.cpp).
    void set_terms(std::vector<TermSet> sets);
    const std::string& pattern() const { return pattern_; }
    const std::vector<TermSet>& terms() const { return sets_; }
    // True when the query has no term at all (everything matches, score 0).
    bool empty() const { return sets_.empty(); }
    // fzf's sortable flag: false when no positive term exists (all `!`).
    bool sortable() const { return sortable_; }

    // Match UTF-8 text. `ascii` skips decoding. `nth`/`nth_count` restrict
    // matching to those codepoint ranges (fzf: --nth; the first matching
    // range wins). `positions`, if given, receives the matched codepoint
    // indices (sorted, may contain duplicates across terms).
    MatchResult match(std::string_view text, bool ascii,
                      const RuneRange* nth = nullptr, size_t nth_count = 0,
                      std::vector<uint32_t>* positions = nullptr) const;

    // Convenience: detects ASCII itself.
    MatchResult match(std::string_view text, std::vector<uint32_t>* positions = nullptr) const;

    // Settings
    void set_case_mode(CaseMode mode) { case_mode_ = mode; }
    void set_algo(AlgoType algo) { algo_ = algo; }
    void set_exact(bool exact) { exact_ = exact; }
    void set_normalize(bool normalize) { normalize_ = normalize; }
    CaseMode case_mode() const { return case_mode_; }

    // Parse a raw query into extended-search term sets (exposed for tests).
    std::vector<TermSet> parse_terms(const std::string& pattern) const;

    // Character class detection / bonus (exposed for tiebreak computation).
    static CharClass char_class_of(CodePoint c);
    static int32_t bonus_for(CharClass prev, CharClass curr);

    // The thread-local decode buffer the last non-ASCII match() on this
    // thread used. Valid until the next match() call on the same thread;
    // lets the caller compute rune-based tiebreaks without decoding twice.
    static const std::vector<CodePoint>& scratch_runes();

    // fzf: parseTerms' per-term case/normalize decision, exposed so the
    // non-extended pattern builder applies the same rules.
    static bool term_case_sensitive(CaseMode mode, const std::string& text);

private:
    template <class Text>
    MatchResult match_text(const Text& text, uint32_t base,
                           std::vector<uint32_t>* positions) const;

    template <class Text>
    MatchResult match_term(const Text& text, const PatternTerm& term,
                           std::vector<uint32_t>* positions) const;

    template <class Text>
    MatchResult fuzzy_match_v1(const Text& text, const std::vector<CodePoint>& pattern,
                               bool case_sensitive, std::vector<uint32_t>* positions) const;
    template <class Text>
    MatchResult fuzzy_match_v2(const Text& text, const std::vector<CodePoint>& pattern,
                               bool case_sensitive, std::vector<uint32_t>* positions) const;
    template <class Text>
    MatchResult exact_match_naive(const Text& text, const std::vector<CodePoint>& pattern,
                                  bool case_sensitive, bool boundary_check,
                                  std::vector<uint32_t>* positions) const;
    template <class Text>
    MatchResult prefix_match(const Text& text, const std::vector<CodePoint>& pattern,
                             bool case_sensitive, std::vector<uint32_t>* positions) const;
    template <class Text>
    MatchResult suffix_match(const Text& text, const std::vector<CodePoint>& pattern,
                             bool case_sensitive, std::vector<uint32_t>* positions) const;
    template <class Text>
    MatchResult equal_match(const Text& text, const std::vector<CodePoint>& pattern,
                            bool case_sensitive, std::vector<uint32_t>* positions) const;
    template <class Text>
    int32_t calculate_score(const Text& text, const std::vector<CodePoint>& pattern,
                            size_t sidx, size_t eidx, bool case_sensitive,
                            std::vector<uint32_t>* positions) const;

    bool char_equal(CodePoint a, CodePoint b, bool case_sensitive) const;
    CodePoint normalize_char(CodePoint c) const;
    template <class Text>
    int32_t bonus_at(const Text& text, size_t idx) const;

    CaseMode case_mode_;
    AlgoType algo_;
    bool exact_;
    bool normalize_;

    std::string pattern_;
    std::vector<TermSet> sets_;
    bool sortable_ = false;
};

} // namespace fzf
