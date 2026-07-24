#pragma once

#include "item.hpp"
#include "util.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cctype>

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

// Fuzzy matcher class
class Matcher {
public:
    Matcher(CaseMode case_mode = CaseMode::Smart,
            AlgoType algo = AlgoType::FuzzyV2,
            bool exact = false)
        : case_mode_(case_mode), algo_(algo), exact_(exact) {}

    // Match a single item against a raw query string (extended-search syntax).
    MatchResult match(const std::shared_ptr<Item>& item, const std::string& pattern);

    // Match multiple items (for multi-threading later)
    std::vector<MatchResult> match_items(
        const std::vector<std::shared_ptr<Item>>& items,
        const std::string& pattern);

    // Set case sensitivity mode
    void set_case_mode(CaseMode mode) { case_mode_ = mode; invalidate_cache(); }

    // Set algorithm
    void set_algo(AlgoType algo) { algo_ = algo; }

    // Exact (substring) matching by default instead of fuzzy (--exact / -e).
    // A 'quoted term then flips back to fuzzy, matching fzf.
    void set_exact(bool exact) { exact_ = exact; invalidate_cache(); }

    // Parse a raw query into extended-search term sets (exposed for tests).
    std::vector<TermSet> parse_terms(const std::string& pattern) const;

private:
    // Term dispatch: run the right algorithm for one term. On success the
    // result's item is set; on failure it is null.
    MatchResult match_term(const std::shared_ptr<Item>& item,
                           const PatternTerm& term);

    // Fuzzy match V1 (greedy algorithm - faster)
    MatchResult fuzzy_match_v1(
        const std::shared_ptr<Item>& item,
        const std::vector<CodePoint>& pattern, bool case_sensitive);

    // Fuzzy match V2 (Smith-Waterman-style optimal algorithm)
    MatchResult fuzzy_match_v2(
        const std::shared_ptr<Item>& item,
        const std::vector<CodePoint>& pattern, bool case_sensitive);

    // Exact substring occurrence with the best first-char bonus (fzf's
    // ExactMatchNaive / ExactMatchBoundary when boundary_check is set).
    MatchResult exact_match_naive(
        const std::shared_ptr<Item>& item,
        const std::vector<CodePoint>& pattern, bool case_sensitive,
        bool boundary_check);

    MatchResult prefix_match(const std::shared_ptr<Item>& item,
                             const std::vector<CodePoint>& pattern,
                             bool case_sensitive);
    MatchResult suffix_match(const std::shared_ptr<Item>& item,
                             const std::vector<CodePoint>& pattern,
                             bool case_sensitive);
    MatchResult equal_match(const std::shared_ptr<Item>& item,
                            const std::vector<CodePoint>& pattern,
                            bool case_sensitive);

    // fzf's calculateScore: score a known match region [sidx, eidx) of the
    // text against the pattern, optionally collecting per-char positions.
    int32_t calculate_score(const std::vector<CodePoint>& text,
                            const std::vector<CodePoint>& pattern,
                            size_t sidx, size_t eidx, bool case_sensitive,
                            std::vector<MatchPos>* positions) const;

    // Character class detection
    CharClass char_class_of(CodePoint c) const;

    // Bonus score calculation
    int32_t bonus_for(CharClass prev, CharClass curr) const;

    // Bonus at a text position (start-of-string counts as a white boundary).
    int32_t bonus_at(const std::vector<CodePoint>& text, size_t idx) const;

    // Character comparison (case-sensitive or insensitive)
    bool char_equal(CodePoint a, CodePoint b, bool case_sensitive) const;

    // Normalize character for comparison
    CodePoint normalize_char(CodePoint c) const;

    void invalidate_cache() { cached_valid_ = false; }

    CaseMode case_mode_;
    AlgoType algo_;
    bool exact_;  // Exact (substring) matching instead of fuzzy

    // Parsed-pattern cache: match() is called once per item per keystroke
    // with the same query string; re-parsing per item would be pure waste.
    mutable std::string cached_pattern_;
    mutable std::vector<TermSet> cached_sets_;
    mutable bool cached_valid_ = false;
};

} // namespace fzf
