#include "matcher.hpp"
#include <utf8.h>
#include <algorithm>
#include <climits>

namespace fzf {

// Convert UTF-8 pattern to UTF-32 code points
std::vector<CodePoint> Matcher::prepare_pattern(const std::string& pattern) {
    std::vector<CodePoint> result;
    try {
        utf8::utf8to32(pattern.begin(), pattern.end(), std::back_inserter(result));
    } catch (...) {
        // Invalid UTF-8, treat as Latin-1
        for (unsigned char c : pattern) {
            result.push_back(static_cast<CodePoint>(c));
        }
    }

    // Determine case sensitivity for this pattern
    if (case_mode_ == CaseMode::Smart) {
        case_sensitive_ = !is_lowercase_pattern(result);
    } else {
        case_sensitive_ = (case_mode_ == CaseMode::Respect);
    }

    // Normalize pattern if case-insensitive
    if (!case_sensitive_) {
        for (auto& c : result) {
            c = normalize_char(c);
        }
    }

    return result;
}

// Check if pattern is all lowercase
bool Matcher::is_lowercase_pattern(const std::vector<CodePoint>& pattern) const {
    for (CodePoint c : pattern) {
        if (c >= 'A' && c <= 'Z') {
            return false;
        }
        // For Unicode, could add more sophisticated checks
    }
    return true;
}

// Normalize character (to lowercase)
CodePoint Matcher::normalize_char(CodePoint c) const {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    // For full Unicode support, would use ICU or similar
    return c;
}

// Character equality check
bool Matcher::char_equal(CodePoint a, CodePoint b) const {
    if (case_sensitive_) {
        return a == b;
    }
    return normalize_char(a) == normalize_char(b);
}

// Determine character class for bonus calculation
CharClass Matcher::char_class_of(CodePoint c) const {
    if (c <= 0x7F) {  // ASCII fast path
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            return CharClass::CharWhite;
        }
        if ((c >= 'a' && c <= 'z')) {
            return CharClass::CharLower;
        }
        if ((c >= 'A' && c <= 'Z')) {
            return CharClass::CharUpper;
        }
        if ((c >= '0' && c <= '9')) {
            return CharClass::CharNumber;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            return CharClass::CharLetter;
        }
        return CharClass::CharNonWord;
    }

    // Unicode: simplified classification
    // Full implementation would use Unicode properties
    return CharClass::CharLetter;
}

// Calculate bonus for character position
int32_t Matcher::bonus_for(CharClass prev, CharClass curr) const {
    if (prev == CharClass::CharWhite && curr != CharClass::CharWhite) {
        // Whitespace boundary
        return BONUS_BOUNDARY_WHITE;
    }
    if ((prev == CharClass::CharNonWord && curr != CharClass::CharNonWord) ||
        (prev != CharClass::CharNonWord && curr == CharClass::CharNonWord)) {
        // Word boundary with delimiter
        return BONUS_BOUNDARY_DELIMITER;
    }
    if (prev == CharClass::CharLower && curr == CharClass::CharUpper) {
        // camelCase
        return BONUS_CAMEL123;
    }
    if (prev != CharClass::CharNumber && curr == CharClass::CharNumber) {
        // Number after letter
        return BONUS_CAMEL123;
    }
    return 0;
}

// Greedy fuzzy matching algorithm (V1) - faster but less optimal than V2
MatchResult Matcher::fuzzy_match_v1(
    const std::shared_ptr<Item>& item,
    const std::vector<CodePoint>& pattern)
{
    const auto& text = item->code_points();
    size_t pattern_len = pattern.size();
    size_t text_len = text.size();

    if (pattern_len == 0) {
        return MatchResult(item, 0);
    }

    if (text_len == 0 || pattern_len > text_len) {
        return MatchResult();  // No match
    }

    // Prepare bonus array for each text position
    std::vector<int32_t> bonus(text_len);
    CharClass prev_class = CharClass::CharWhite;
    for (size_t i = 0; i < text_len; ++i) {
        CharClass curr_class = char_class_of(text[i]);
        bonus[i] = bonus_for(prev_class, curr_class);
        prev_class = curr_class;
    }

    // Greedy left-to-right matching
    std::vector<MatchPos> positions;
    positions.reserve(pattern_len);

    int32_t score = 0;
    size_t text_idx = 0;
    int32_t prev_match_pos = -1;

    for (size_t pat_idx = 0; pat_idx < pattern_len; ++pat_idx) {
        // Find next occurrence of pattern character
        bool found = false;
        int32_t best_pos = -1;
        int32_t best_bonus = 0;

        // Look ahead a few positions to find best bonus position (limited lookahead)
        const size_t lookahead = 10;
        size_t search_end = std::min(text_idx + lookahead, text_len);

        for (size_t j = text_idx; j < search_end; ++j) {
            if (char_equal(text[j], pattern[pat_idx])) {
                int32_t current_bonus = bonus[j];

                // Prefer consecutive matches
                if (prev_match_pos >= 0 && static_cast<int32_t>(j) == prev_match_pos + 1) {
                    current_bonus += BONUS_CONSECUTIVE;
                }

                // First character bonus
                if (j == 0) {
                    current_bonus += BONUS_FIRST_CHAR_MULTIPLIER * 2;
                }

                if (!found || current_bonus > best_bonus) {
                    found = true;
                    best_pos = static_cast<int32_t>(j);
                    best_bonus = current_bonus;

                    // If we found a good boundary match, take it immediately
                    if (current_bonus >= BONUS_BOUNDARY_WHITE) {
                        break;
                    }
                }
            }
        }

        // If not found in lookahead, scan the rest
        if (!found) {
            for (size_t j = search_end; j < text_len; ++j) {
                if (char_equal(text[j], pattern[pat_idx])) {
                    found = true;
                    best_pos = static_cast<int32_t>(j);
                    best_bonus = bonus[j];
                    break;
                }
            }
        }

        if (!found) {
            return MatchResult();  // No match
        }

        // Record the match
        positions.push_back({static_cast<uint32_t>(best_pos),
                            static_cast<uint32_t>(best_pos + 1)});

        // Calculate score
        score += SCORE_MATCH + best_bonus;

        // Move past this match
        text_idx = static_cast<size_t>(best_pos) + 1;
        prev_match_pos = best_pos;
    }

    return MatchResult(item, score, std::move(positions));
}

// Smith-Waterman fuzzy matching algorithm (V2)
MatchResult Matcher::fuzzy_match_v2(
    const std::shared_ptr<Item>& item,
    const std::vector<CodePoint>& pattern)
{
    const auto& text = item->code_points();
    size_t pattern_len = pattern.size();
    size_t text_len = text.size();

    if (pattern_len == 0) {
        return MatchResult(item, 0);
    }

    if (text_len == 0 || pattern_len > text_len) {
        return MatchResult();  // No match
    }

    // Quick check: all pattern characters must exist in text
    size_t text_idx = 0;
    for (size_t pat_idx = 0; pat_idx < pattern_len; ++pat_idx) {
        bool found = false;
        while (text_idx < text_len) {
            if (char_equal(text[text_idx], pattern[pat_idx])) {
                found = true;
                text_idx++;
                break;
            }
            text_idx++;
        }
        if (!found) {
            return MatchResult();  // No match
        }
    }

    // Prepare bonus array for each text position
    std::vector<int32_t> bonus(text_len);
    CharClass prev_class = CharClass::CharWhite;
    for (size_t i = 0; i < text_len; ++i) {
        CharClass curr_class = char_class_of(text[i]);
        bonus[i] = bonus_for(prev_class, curr_class);
        prev_class = curr_class;
    }

    // DP matrices: H (score) and E (gap)
    // Using 2 rows for space efficiency (current and previous)
    std::vector<int32_t> H0(text_len + 1, 0);
    std::vector<int32_t> H1(text_len + 1, 0);
    std::vector<int32_t> E0(text_len + 1, 0);
    std::vector<int32_t> E1(text_len + 1, 0);

    // Backtracking: store best position per pattern character
    std::vector<std::vector<int32_t>> best_pos(pattern_len);
    for (size_t i = 0; i < pattern_len; ++i) {
        best_pos[i].resize(text_len + 1, -1);
    }

    int32_t max_score = 0;
    int32_t max_score_pos = 0;

    // Fill DP table
    for (size_t i = 0; i < pattern_len; ++i) {
        CodePoint pattern_char = pattern[i];
        int32_t gap_score = (i == pattern_len - 1) ? SCORE_GAP_EXTENSION : SCORE_GAP_START;

        std::fill(H1.begin(), H1.end(), 0);
        std::fill(E1.begin(), E1.end(), 0);

        for (size_t j = 0; j < text_len; ++j) {
            // Calculate match score
            int32_t match_score = 0;
            if (char_equal(pattern_char, text[j])) {
                match_score = SCORE_MATCH + bonus[j];

                // Consecutive bonus
                if (i > 0 && best_pos[i-1][j] == static_cast<int32_t>(j) - 1) {
                    match_score += BONUS_CONSECUTIVE;
                }

                // First character bonus
                if (j == 0) {
                    match_score *= BONUS_FIRST_CHAR_MULTIPLIER;
                }

                match_score += H0[j];
            }

            // Calculate gap score
            E1[j+1] = std::max(E0[j+1] + gap_score, H0[j+1] + SCORE_GAP_START);

            // Take maximum
            H1[j+1] = std::max({0, match_score, E1[j+1]});

            // Track best position for backtracking
            if (H1[j+1] > 0) {
                if (match_score >= E1[j+1]) {
                    best_pos[i][j+1] = j;
                } else {
                    best_pos[i][j+1] = best_pos[i][j];
                }
            }

            // Update max score
            if (i == pattern_len - 1 && H1[j+1] > max_score) {
                max_score = H1[j+1];
                max_score_pos = j + 1;
            }
        }

        // Swap rows
        std::swap(H0, H1);
        std::swap(E0, E1);
    }

    if (max_score <= 0) {
        return MatchResult();  // No good match
    }

    // Backtrack to find matched positions
    std::vector<MatchPos> positions;
    int32_t pos = max_score_pos;
    for (int32_t i = pattern_len - 1; i >= 0 && pos > 0; --i) {
        int32_t match_pos = best_pos[i][pos];
        if (match_pos >= 0) {
            positions.push_back({static_cast<uint32_t>(match_pos),
                               static_cast<uint32_t>(match_pos + 1)});
            pos = match_pos;
        }
    }

    std::reverse(positions.begin(), positions.end());

    return MatchResult(item, max_score, std::move(positions));
}

// Main match function
MatchResult Matcher::match(const std::shared_ptr<Item>& item,
                           const std::string& pattern)
{
    if (pattern.empty()) {
        return MatchResult(item, 0);
    }

    auto pattern_cp = prepare_pattern(pattern);

    if (algo_ == AlgoType::FuzzyV2) {
        return fuzzy_match_v2(item, pattern_cp);
    } else if (algo_ == AlgoType::FuzzyV1) {
        return fuzzy_match_v1(item, pattern_cp);
    }

    // Default to V2
    return fuzzy_match_v2(item, pattern_cp);
}

// Match multiple items
std::vector<MatchResult> Matcher::match_items(
    const std::vector<std::shared_ptr<Item>>& items,
    const std::string& pattern)
{
    std::vector<MatchResult> results;
    results.reserve(items.size());

    for (const auto& item : items) {
        auto result = match(item, pattern);
        if (result.item && result.score > 0) {
            results.push_back(std::move(result));
        }
    }

    // Sort by score (descending)
    std::sort(results.begin(), results.end());

    return results;
}

} // namespace fzf
