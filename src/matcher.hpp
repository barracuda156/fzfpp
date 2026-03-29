#pragma once

#include "item.hpp"
#include "util.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cctype>

namespace fzf {

// Scoring constants (matching fzf's algorithm)
constexpr int32_t SCORE_MATCH = 16;
constexpr int32_t SCORE_GAP_START = -3;
constexpr int32_t SCORE_GAP_EXTENSION = -1;

constexpr int32_t BONUS_BOUNDARY = 8;
constexpr int32_t BONUS_BOUNDARY_WHITE = 10;
constexpr int32_t BONUS_BOUNDARY_DELIMITER = 9;
constexpr int32_t BONUS_CAMEL123 = 7;
constexpr int32_t BONUS_CONSECUTIVE = 4;
constexpr int32_t BONUS_FIRST_CHAR_MULTIPLIER = 2;

// Character class for bonus calculation
enum class CharClass {
    CharWhite,
    CharNonWord,
    CharLower,
    CharUpper,
    CharLetter,
    CharNumber
};

// Fuzzy matcher class
class Matcher {
public:
    Matcher(CaseMode case_mode = CaseMode::Smart,
            AlgoType algo = AlgoType::FuzzyV2)
        : case_mode_(case_mode), algo_(algo) {}

    // Match a single item against a pattern
    MatchResult match(const std::shared_ptr<Item>& item, const std::string& pattern);

    // Match multiple items (for multi-threading later)
    std::vector<MatchResult> match_items(
        const std::vector<std::shared_ptr<Item>>& items,
        const std::string& pattern);

    // Set case sensitivity mode
    void set_case_mode(CaseMode mode) { case_mode_ = mode; }

    // Set algorithm
    void set_algo(AlgoType algo) { algo_ = algo; }

private:
    // Convert pattern to code points
    std::vector<CodePoint> prepare_pattern(const std::string& pattern);

    // Fuzzy match V1 (greedy algorithm - faster)
    MatchResult fuzzy_match_v1(
        const std::shared_ptr<Item>& item,
        const std::vector<CodePoint>& pattern);

    // Fuzzy match V2 (Smith-Waterman algorithm - optimal)
    MatchResult fuzzy_match_v2(
        const std::shared_ptr<Item>& item,
        const std::vector<CodePoint>& pattern);

    // Character class detection
    CharClass char_class_of(CodePoint c) const;

    // Bonus score calculation
    int32_t bonus_for(CharClass prev, CharClass curr) const;

    // Character comparison (case-sensitive or insensitive)
    bool char_equal(CodePoint a, CodePoint b) const;

    // Normalize character for comparison
    CodePoint normalize_char(CodePoint c) const;

    // Check if pattern is lowercase (for smart case)
    bool is_lowercase_pattern(const std::vector<CodePoint>& pattern) const;

    CaseMode case_mode_;
    AlgoType algo_;
    mutable bool case_sensitive_;  // Computed per-pattern for smart case
};

} // namespace fzf
