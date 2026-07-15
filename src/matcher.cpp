#include "matcher.hpp"
#include <utf8.h>
#include <algorithm>
#include <limits>

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

bool Matcher::is_lowercase_pattern(const std::vector<CodePoint>& pattern) const {
    for (CodePoint c : pattern) {
        if (c >= 'A' && c <= 'Z') {
            return false;
        }
    }
    return true;
}

CodePoint Matcher::normalize_char(CodePoint c) const {
    // ASCII-only lowercasing; full Unicode case folding would need ICU.
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

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
        return CharClass::CharNonWord;
    }

    // Non-ASCII code points are treated as generic letters.
    return CharClass::CharLetter;
}

// Calculate bonus for character position
int32_t Matcher::bonus_for(CharClass prev, CharClass curr) const {
    if (curr != CharClass::CharNonWord && curr != CharClass::CharWhite) {
        if (prev == CharClass::CharWhite) {
            return BONUS_BOUNDARY_WHITE;
        }
        if (prev == CharClass::CharNonWord) {
            return BONUS_BOUNDARY;
        }
    }
    if (prev == CharClass::CharLower && curr == CharClass::CharUpper) {
        return BONUS_CAMEL123;
    }
    if (prev != CharClass::CharNumber && curr == CharClass::CharNumber) {
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

    // Greedy left-to-right matching: take the first occurrence of each pattern
    // character. Because character presence is monotonic, this always finds a
    // match when one exists (unlike a best-bonus lookahead, which can pick a
    // later position and strand the remaining pattern characters).
    std::vector<MatchPos> positions;
    positions.reserve(pattern_len);

    int32_t score = 0;
    size_t text_idx = 0;
    int32_t prev_match_pos = -1;

    for (size_t pat_idx = 0; pat_idx < pattern_len; ++pat_idx) {
        bool found = false;
        size_t match_pos = 0;

        for (size_t j = text_idx; j < text_len; ++j) {
            if (char_equal(text[j], pattern[pat_idx])) {
                found = true;
                match_pos = j;
                break;
            }
        }

        if (!found) {
            return MatchResult();  // No match
        }

        int32_t char_bonus = bonus[match_pos];
        if (prev_match_pos >= 0 && static_cast<int32_t>(match_pos) == prev_match_pos + 1) {
            char_bonus += BONUS_CONSECUTIVE;
        }
        if (match_pos == 0) {
            char_bonus *= BONUS_FIRST_CHAR_MULTIPLIER;
        }

        positions.push_back({static_cast<uint32_t>(match_pos),
                            static_cast<uint32_t>(match_pos + 1)});

        score += SCORE_MATCH + char_bonus;

        text_idx = match_pos + 1;
        prev_match_pos = static_cast<int32_t>(match_pos);
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

    // For each pattern character i and text column j we track:
    //   M[i][j]    : best score for aligning pattern[0..i] with pattern[i]
    //                matched exactly at text[j] (0 if pattern[i] != text[j]).
    //   run[i][j]  : length of the consecutive match run ending at that cell.
    //   from[i][j] : the column of pattern[i-1]'s match on the best path into
    //                this cell (-1 for i == 0). Recorded so backtracking is
    //                exact even across gaps.
    // A match at column j may follow the previous pattern char either
    // immediately (diagonal, j-1) or after a gap. To support gaps we keep, per
    // row, the best "prefix" cell seen so far in row i-1 with the accumulated
    // gap penalty folded in as we sweep columns left to right.
    std::vector<std::vector<int32_t>> M(pattern_len, std::vector<int32_t>(text_len, 0));
    std::vector<std::vector<int32_t>> run(pattern_len, std::vector<int32_t>(text_len, 0));
    std::vector<std::vector<int32_t>> from(pattern_len, std::vector<int32_t>(text_len, -1));

    int32_t max_score = 0;
    size_t max_score_row = 0;
    size_t max_score_pos = 0;

    for (size_t i = 0; i < pattern_len; ++i) {
        CodePoint pattern_char = pattern[i];
        int32_t gap_penalty = (i == pattern_len - 1) ? SCORE_GAP_EXTENSION
                                                     : SCORE_GAP_START;

        // Best score/column for completing pattern[0..i-1] to the left of the
        // current column, gap penalty included. Sourced from the previous row.
        int32_t prefix_best = 0;
        int32_t prefix_col = -1;

        for (size_t j = 0; j < text_len; ++j) {
            if (char_equal(pattern_char, text[j])) {
                if (i == 0) {
                    M[i][j] = SCORE_MATCH + bonus[j] * BONUS_FIRST_CHAR_MULTIPLIER;
                    run[i][j] = 1;
                    from[i][j] = -1;
                } else {
                    int32_t best = 0;
                    // Consecutive with the previous pattern char (diagonal).
                    if (j > 0 && run[i-1][j-1] > 0) {
                        int32_t s = M[i-1][j-1] + SCORE_MATCH +
                                    std::max(bonus[j], BONUS_CONSECUTIVE);
                        if (s > best) {
                            best = s;
                            M[i][j] = s;
                            run[i][j] = run[i-1][j-1] + 1;
                            from[i][j] = static_cast<int32_t>(j) - 1;
                        }
                    }
                    // After a gap from an earlier match of the previous char.
                    if (prefix_col >= 0) {
                        int32_t s = prefix_best + SCORE_MATCH + bonus[j];
                        if (s > best) {
                            best = s;
                            M[i][j] = s;
                            run[i][j] = 1;
                            from[i][j] = prefix_col;
                        }
                    }
                }
            }

            // Advance the prefix (best completion of pattern[0..i-1]) to the
            // next column, folding in one gap step, then admit this column's own
            // completed match as a future prefix source.
            if (prefix_col >= 0) {
                prefix_best += gap_penalty;
            }
            if (i > 0 && run[i-1][j] > 0 &&
                (prefix_col < 0 || M[i-1][j] > prefix_best)) {
                prefix_best = M[i-1][j];
                prefix_col = static_cast<int32_t>(j);
            }

            if (i == pattern_len - 1 && run[i][j] > 0 && M[i][j] > max_score) {
                max_score = M[i][j];
                max_score_row = i;
                max_score_pos = j;
            }
        }
    }

    if (max_score <= 0) {
        return MatchResult();  // No good match
    }

    // Backtrack along the recorded predecessor columns.
    std::vector<MatchPos> positions;
    long i = static_cast<long>(max_score_row);
    long j = static_cast<long>(max_score_pos);
    while (i >= 0 && j >= 0) {
        positions.push_back({static_cast<uint32_t>(j),
                             static_cast<uint32_t>(j + 1)});
        j = from[i][j];
        --i;
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

    // An empty pattern matches every item (score 0), like fzf.
    bool empty_pattern = pattern.empty();

    for (const auto& item : items) {
        auto result = match(item, pattern);
        if (result.item && (empty_pattern || result.score > 0)) {
            results.push_back(std::move(result));
        }
    }

    // Sort by score (descending)
    std::sort(results.begin(), results.end());

    return results;
}

} // namespace fzf
