#include "matcher.hpp"
#include <utf8.h>
#include <algorithm>
#include <limits>

namespace fzf {

namespace {

// ASCII-only lowercase of a UTF-8 string (bytes >= 0x80 untouched). Full
// Unicode case folding would need ICU; fzf's smart-case decision and
// case-insensitive comparison are ported at the same ASCII fidelity the rest
// of this file uses.
std::string ascii_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

std::vector<CodePoint> to_code_points(const std::string& s) {
    std::vector<CodePoint> result;
    try {
        utf8::utf8to32(s.begin(), s.end(), std::back_inserter(result));
    } catch (...) {
        // Invalid UTF-8, treat as Latin-1. Clear first: utf8to32 may have
        // appended converted codepoints before throwing, and keeping that
        // prefix would duplicate part of the pattern.
        result.clear();
        for (unsigned char c : s) {
            result.push_back(static_cast<CodePoint>(c));
        }
    }
    return result;
}

bool is_space_cp(CodePoint c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

size_t leading_whitespaces(const std::vector<CodePoint>& text) {
    size_t n = 0;
    while (n < text.size() && is_space_cp(text[n])) {
        ++n;
    }
    return n;
}

size_t trailing_whitespaces(const std::vector<CodePoint>& text) {
    size_t n = 0;
    while (n < text.size() && is_space_cp(text[text.size() - 1 - n])) {
        ++n;
    }
    return n;
}

// Latin-script diacritic table, ported verbatim from fzf's
// src/algo/normalize.go (`normalized` map). Sorted by key for binary search.
// Maps precomposed/combining Latin letters and fullwidth/halfwidth forms to
// their plain ASCII base letter, so e.g. "cafe" matches "café".
constexpr std::pair<CodePoint, CodePoint> kNormalizeTable[] = {
#include "normalize_table.inc"
};

CodePoint strip_accent(CodePoint c) {
    // fzf's fast-path range check before the table lookup.
    if (c < 0x00C0 || c > 0xFF61) {
        return c;
    }
    auto it = std::lower_bound(
        std::begin(kNormalizeTable), std::end(kNormalizeTable), c,
        [](const std::pair<CodePoint, CodePoint>& kv, CodePoint key) {
            return kv.first < key;
        });
    if (it != std::end(kNormalizeTable) && it->first == c) {
        return it->second;
    }
    return c;
}

} // namespace

CodePoint Matcher::normalize_char(CodePoint c) const {
    return strip_accent(c);
}

bool Matcher::char_equal(CodePoint a, CodePoint b, bool case_sensitive) const {
    // Accent stripping (fzf's `normalize`) is independent of case
    // sensitivity: `--case-sensitive cafe` still matches "café".
    a = strip_accent(a);
    b = strip_accent(b);
    if (case_sensitive) {
        return a == b;
    }
    if (a >= 'A' && a <= 'Z') a += ('a' - 'A');
    if (b >= 'A' && b <= 'Z') b += ('a' - 'A');
    return a == b;
}

// Determine character class for bonus calculation (fzf's charClassOf; the
// delimiter set is fzf's default-scheme delimiterChars "/,:;|").
CharClass Matcher::char_class_of(CodePoint c) const {
    if (c <= 0x7F) {  // ASCII fast path
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '\v' || c == '\f') {
            return CharClass::CharWhite;
        }
        if (c >= 'a' && c <= 'z') {
            return CharClass::CharLower;
        }
        if (c >= 'A' && c <= 'Z') {
            return CharClass::CharUpper;
        }
        if (c >= '0' && c <= '9') {
            return CharClass::CharNumber;
        }
        if (c == '/' || c == ',' || c == ':' || c == ';' || c == '|') {
            return CharClass::CharDelimiter;
        }
        return CharClass::CharNonWord;
    }

    // Non-ASCII code points are treated as generic letters.
    return CharClass::CharLetter;
}

// Calculate bonus for a character position (fzf's bonusFor, default scheme).
int32_t Matcher::bonus_for(CharClass prev, CharClass curr) const {
    if (curr != CharClass::CharWhite) {
        if (prev == CharClass::CharWhite) {
            return BONUS_BOUNDARY_WHITE;  // word boundary after whitespace
        }
        if (prev == CharClass::CharDelimiter) {
            return BONUS_BOUNDARY_DELIMITER;  // boundary after / , : ; |
        }
        if (prev == CharClass::CharNonWord) {
            return BONUS_BOUNDARY;  // word boundary
        }
    }
    if ((prev == CharClass::CharLower && curr == CharClass::CharUpper) ||
        (prev != CharClass::CharNumber && curr == CharClass::CharNumber)) {
        return BONUS_CAMEL123;  // camelCase letter123
    }
    if (curr == CharClass::CharNonWord || curr == CharClass::CharDelimiter) {
        return BONUS_NON_WORD;
    }
    if (curr == CharClass::CharWhite) {
        return BONUS_BOUNDARY_WHITE;
    }
    return 0;
}

int32_t Matcher::bonus_at(const std::vector<CodePoint>& text, size_t idx) const {
    if (idx == 0) {
        return BONUS_BOUNDARY_WHITE;
    }
    return bonus_for(char_class_of(text[idx - 1]), char_class_of(text[idx]));
}

// fzf's calculateScore: walk a known match region and accumulate the same
// bonuses/penalties V2 would assign, so exact/prefix/suffix results rank
// comparably against fuzzy ones.
int32_t Matcher::calculate_score(const std::vector<CodePoint>& text,
                                 const std::vector<CodePoint>& pattern,
                                 size_t sidx, size_t eidx, bool case_sensitive,
                                 std::vector<MatchPos>* positions) const {
    size_t pidx = 0;
    int32_t score = 0;
    bool in_gap = false;
    int32_t consecutive = 0;
    int32_t first_bonus = 0;
    CharClass prev_class = CharClass::CharWhite;
    if (sidx > 0) {
        prev_class = char_class_of(text[sidx - 1]);
    }
    for (size_t idx = sidx; idx < eidx; ++idx) {
        CodePoint c = text[idx];
        CharClass klass = char_class_of(c);
        // Accent stripping applies regardless of case sensitivity; the
        // pattern is compared through the same transform (char_equal does
        // this for every other match path -- mirrored here since this loop
        // compares codepoints directly for scoring/positions).
        c = strip_accent(c);
        CodePoint p = pidx < pattern.size() ? strip_accent(pattern[pidx]) : 0;
        if (!case_sensitive) {
            if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
            if (p >= 'A' && p <= 'Z') p += ('a' - 'A');
        }
        if (pidx < pattern.size() && c == p) {
            if (positions) {
                positions->push_back({static_cast<uint32_t>(idx),
                                      static_cast<uint32_t>(idx + 1)});
            }
            score += SCORE_MATCH;
            int32_t bonus = bonus_for(prev_class, klass);
            if (consecutive == 0) {
                first_bonus = bonus;
            } else {
                // Break consecutive chunk
                if (bonus >= BONUS_BOUNDARY && bonus > first_bonus) {
                    first_bonus = bonus;
                }
                bonus = std::max({bonus, first_bonus, BONUS_CONSECUTIVE});
            }
            if (pidx == 0) {
                score += bonus * BONUS_FIRST_CHAR_MULTIPLIER;
            } else {
                score += bonus;
            }
            in_gap = false;
            ++consecutive;
            ++pidx;
        } else {
            score += in_gap ? SCORE_GAP_EXTENSION : SCORE_GAP_START;
            in_gap = true;
            consecutive = 0;
            first_bonus = 0;
        }
        prev_class = klass;
    }
    return score;
}

// Greedy fuzzy matching algorithm (V1) - fzf's FuzzyMatchV1: forward scan to
// find the first subsequence, backward scan to shrink its window, then
// calculateScore over the window.
MatchResult Matcher::fuzzy_match_v1(
    const std::shared_ptr<Item>& item,
    const std::vector<CodePoint>& pattern, bool case_sensitive)
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

    // Forward: first subsequence occurrence.
    size_t pidx = 0;
    long sidx = -1, eidx = -1;
    for (size_t idx = 0; idx < text_len; ++idx) {
        if (char_equal(text[idx], pattern[pidx], case_sensitive)) {
            if (sidx < 0) {
                sidx = static_cast<long>(idx);
            }
            if (++pidx == pattern_len) {
                eidx = static_cast<long>(idx) + 1;
                break;
            }
        }
    }
    if (eidx < 0) {
        return MatchResult();  // No match
    }

    // Backward: shrink the window from the right so the match is as tight as
    // possible (fzf does this to improve scoring locality).
    pidx = pattern_len - 1;
    for (long idx = eidx - 1; idx >= sidx; --idx) {
        if (char_equal(text[static_cast<size_t>(idx)], pattern[pidx],
                       case_sensitive)) {
            if (pidx == 0) {
                sidx = idx;
                break;
            }
            --pidx;
        }
    }

    std::vector<MatchPos> positions;
    int32_t score = calculate_score(text, pattern, static_cast<size_t>(sidx),
                                    static_cast<size_t>(eidx), case_sensitive,
                                    &positions);
    return MatchResult(item, score, std::move(positions));
}

// Optimal fuzzy matching (V2) - a port of fzf's FuzzyMatchV2 recurrence.
//
// Whether an item MATCHES is decided solely by the forward feasibility scan
// (does the pattern appear as a subsequence?) -- scoring only ranks. fzf's H
// values floor at 0 (max(s1, s2, 0)), so gap decay can never turn a real
// subsequence into a non-match; every match scores >= SCORE_MATCH.
//
// Cell semantics (full-matrix form of fzf's rolling rows, kept so that
// backtracking can follow explicit predecessor links -- see the from[]
// invariants in the project memory):
//   M[i][j]    fzf's s1: score of aligning pattern[0..i] with pattern[i]
//              matched exactly at text[j] (0 if infeasible / chars differ).
//   run[i][j]  fzf's C: consecutive-run length at that cell (0 when the gap
//              path dominated, matching fzf's consecutive-reset rule).
//   from[i][j] column where pattern[i-1] matched on the best path into this
//              cell (-1 for row 0). Backtracking follows these exactly.
//
// The previous row's H (best score for pattern[0..i-1] ending at-or-before a
// column, with fzf's gap decay: -3 for the first gap column, -1 for each
// further one, floored at 0) is reconstructed as a running carry while
// sweeping row i, so no H matrix is stored.
MatchResult Matcher::fuzzy_match_v2(
    const std::shared_ptr<Item>& item,
    const std::vector<CodePoint>& pattern, bool case_sensitive)
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

    // Feasibility: the pattern must appear as a subsequence. This alone
    // decides match/no-match, like fzf's phase-2 pidx check.
    {
        size_t text_idx = 0;
        for (size_t pat_idx = 0; pat_idx < pattern_len; ++pat_idx) {
            bool found = false;
            while (text_idx < text_len) {
                if (char_equal(text[text_idx], pattern[pat_idx], case_sensitive)) {
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
    }

    // Per-position boundary bonuses.
    std::vector<int32_t> bonus(text_len);
    CharClass prev_class = CharClass::CharWhite;
    for (size_t i = 0; i < text_len; ++i) {
        CharClass curr_class = char_class_of(text[i]);
        bonus[i] = bonus_for(prev_class, curr_class);
        prev_class = curr_class;
    }

    // Flat matrices; row i starts at i * text_len. M/run only ever need the
    // previous row during the sweep, but run's diagonal read makes a simple
    // two-row scheme error-prone; from[] genuinely needs full history for
    // backtracking. Keep all three full -- clarity over the last few bytes.
    std::vector<int32_t> M(pattern_len * text_len, 0);
    std::vector<int32_t> run(pattern_len * text_len, 0);
    std::vector<int32_t> from(pattern_len * text_len, -1);
    auto at = [text_len](size_t i, size_t j) { return i * text_len + j; };

    int32_t max_score = 0;
    size_t max_score_pos = 0;

    // Row 0: every match of pattern[0] seeds a run; first-char bonus doubled.
    for (size_t j = 0; j < text_len; ++j) {
        if (char_equal(pattern[0], text[j], case_sensitive)) {
            M[at(0, j)] = SCORE_MATCH + bonus[j] * BONUS_FIRST_CHAR_MULTIPLIER;
            run[at(0, j)] = 1;
            if (pattern_len == 1 && M[at(0, j)] > max_score) {
                max_score = M[at(0, j)];
                max_score_pos = j;
            }
        }
    }

    for (size_t i = 1; i < pattern_len; ++i) {
        CodePoint pattern_char = pattern[i];

        // hprev reconstructs H[i-1][j-1] (the diagonal source) as j sweeps:
        // the best completion of pattern[0..i-1] at-or-before the previous
        // column, gap decay applied, floored at 0. hprev_col is the column of
        // the actual match cell that value descends from -- the backtracking
        // anchor. prev_in_gap tracks fzf's per-row inGap flag for row i-1.
        int32_t hprev = 0;
        int32_t hprev_col = -1;
        bool prev_in_gap = false;

        // hcur / cur_in_gap: the same carry for the CURRENT row (H[i][j-1]),
        // fzf's s2 source.
        int32_t hcur = 0;
        bool cur_in_gap = false;

        for (size_t j = 0; j < text_len; ++j) {
            int32_t s2 = hcur + (cur_in_gap ? SCORE_GAP_EXTENSION : SCORE_GAP_START);
            int32_t s1 = 0;
            int32_t consecutive = 0;

            // A cell is only valid once some completion of pattern[0..i-1]
            // exists strictly to the left (hprev_col >= 0) -- the full-sweep
            // equivalent of fzf starting row i at F[i].
            if (hprev_col >= 0 &&
                char_equal(pattern_char, text[j], case_sensitive)) {
                s1 = hprev + SCORE_MATCH;
                int32_t b = bonus[j];
                consecutive = (j > 0 ? run[at(i - 1, j - 1)] : 0) + 1;
                if (consecutive > 1) {
                    // Bonus of the run's first character.
                    int32_t fb = bonus[j - static_cast<size_t>(consecutive - 1)];
                    if (b >= BONUS_BOUNDARY && b > fb) {
                        // This char starts at a stronger boundary than the
                        // run did: break the chunk and restart here.
                        consecutive = 1;
                    } else {
                        b = std::max({b, BONUS_CONSECUTIVE, fb});
                    }
                }
                if (s1 + b < s2) {
                    // Gap path dominates; keep the raw positional bonus and
                    // drop the run (fzf's consecutive reset).
                    s1 += bonus[j];
                    consecutive = 0;
                } else {
                    s1 += b;
                }
                M[at(i, j)] = s1;
                run[at(i, j)] = consecutive;
                from[at(i, j)] = hprev_col;

                if (i == pattern_len - 1 && s1 > max_score) {
                    max_score = s1;
                    max_score_pos = j;
                }
            }

            cur_in_gap = s1 < s2;
            hcur = std::max({s1, s2, 0});

            // Advance hprev to H[i-1][j] for the next column: the previous
            // row's own match cell competes against the decayed carry.
            int32_t carried =
                hprev + (prev_in_gap ? SCORE_GAP_EXTENSION : SCORE_GAP_START);
            int32_t prev_cell = run[at(i - 1, j)] > 0 ? M[at(i - 1, j)] : 0;
            if (run[at(i - 1, j)] > 0 && prev_cell >= carried) {
                hprev = std::max(prev_cell, 0);
                hprev_col = static_cast<int32_t>(j);
                prev_in_gap = false;
            } else {
                hprev = std::max(carried, 0);
                prev_in_gap = true;
            }
        }
    }

    // The feasibility scan passed, so the last row has at least one valid
    // cell and max_score >= SCORE_MATCH: a found subsequence is ALWAYS a
    // match (scores only rank, they never disqualify).

    // Backtrack along the recorded predecessor columns.
    std::vector<MatchPos> positions;
    long i = static_cast<long>(pattern_len) - 1;
    long j = static_cast<long>(max_score_pos);
    while (i >= 0 && j >= 0) {
        positions.push_back({static_cast<uint32_t>(j),
                             static_cast<uint32_t>(j + 1)});
        j = from[at(static_cast<size_t>(i), static_cast<size_t>(j))];
        --i;
    }

    std::reverse(positions.begin(), positions.end());

    return MatchResult(item, max_score, std::move(positions));
}

// fzf's ExactMatchNaive / ExactMatchBoundary: the whole pattern as one
// contiguous run; among all occurrences keep the one whose FIRST character
// has the highest boundary bonus (earliest such occurrence wins; the scan
// stops early once a boundary-quality occurrence is found).
MatchResult Matcher::exact_match_naive(
    const std::shared_ptr<Item>& item,
    const std::vector<CodePoint>& pattern, bool case_sensitive,
    bool boundary_check)
{
    const auto& text = item->code_points();
    size_t pattern_len = pattern.size();
    size_t text_len = text.size();

    if (pattern_len == 0) {
        return MatchResult(item, 0);
    }
    if (text_len < pattern_len) {
        return MatchResult();  // No match
    }

    long best_start = -1;
    int32_t best_bonus = -1;
    for (size_t start = 0; start + pattern_len <= text_len; ++start) {
        bool matched = true;
        for (size_t k = 0; k < pattern_len; ++k) {
            if (!char_equal(text[start + k], pattern[k], case_sensitive)) {
                matched = false;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        int32_t first_bonus = bonus_at(text, start);
        if (boundary_check) {
            // Both ends must sit on boundaries: the start's bonus must be
            // boundary-grade and the characters just outside the match (if
            // any) must be whitespace/non-word/delimiter.
            if (first_bonus < BONUS_BOUNDARY) {
                continue;
            }
            if (start > 0) {
                CharClass before = char_class_of(text[start - 1]);
                if (before != CharClass::CharWhite &&
                    before != CharClass::CharNonWord &&
                    before != CharClass::CharDelimiter) {
                    continue;
                }
            }
            size_t end = start + pattern_len;
            if (end < text_len) {
                CharClass after = char_class_of(text[end]);
                if (after != CharClass::CharWhite &&
                    after != CharClass::CharNonWord &&
                    after != CharClass::CharDelimiter) {
                    continue;
                }
            }
        }
        if (first_bonus > best_bonus) {
            best_start = static_cast<long>(start);
            best_bonus = first_bonus;
        }
        if (first_bonus >= BONUS_BOUNDARY) {
            break;  // can't do better in a way fzf distinguishes
        }
    }

    if (best_start < 0) {
        return MatchResult();  // No match
    }

    size_t sidx = static_cast<size_t>(best_start);
    size_t eidx = sidx + pattern_len;
    int32_t score;
    if (boundary_check) {
        // fzf ranks underscore-adjacent boundaries slightly lower.
        score = best_bonus;
        int32_t deduct = (best_bonus - BONUS_BOUNDARY) + 1;
        if (sidx > 0 && text[sidx - 1] == '_') {
            score -= deduct + 1;
            deduct = 1;
        }
        if (eidx < text_len && text[eidx] == '_') {
            score -= deduct;
        }
        score += SCORE_MATCH * static_cast<int32_t>(pattern_len) +
                 BONUS_BOUNDARY_WHITE * (static_cast<int32_t>(pattern_len) + 1);
    } else {
        score = calculate_score(text, pattern, sidx, eidx, case_sensitive,
                                nullptr);
    }

    std::vector<MatchPos> positions;
    positions.reserve(pattern_len);
    for (size_t k = sidx; k < eidx; ++k) {
        positions.push_back({static_cast<uint32_t>(k),
                             static_cast<uint32_t>(k + 1)});
    }
    return MatchResult(item, score, std::move(positions));
}

MatchResult Matcher::prefix_match(const std::shared_ptr<Item>& item,
                                  const std::vector<CodePoint>& pattern,
                                  bool case_sensitive)
{
    const auto& text = item->code_points();
    if (pattern.empty()) {
        return MatchResult(item, 0);
    }
    // fzf skips the item's leading whitespace unless the pattern itself
    // starts with whitespace.
    size_t trimmed = is_space_cp(pattern[0]) ? 0 : leading_whitespaces(text);
    if (text.size() - trimmed < pattern.size()) {
        return MatchResult();
    }
    for (size_t k = 0; k < pattern.size(); ++k) {
        if (!char_equal(text[trimmed + k], pattern[k], case_sensitive)) {
            return MatchResult();
        }
    }
    size_t sidx = trimmed, eidx = trimmed + pattern.size();
    std::vector<MatchPos> positions;
    positions.reserve(pattern.size());
    for (size_t k = sidx; k < eidx; ++k) {
        positions.push_back({static_cast<uint32_t>(k),
                             static_cast<uint32_t>(k + 1)});
    }
    int32_t score = calculate_score(text, pattern, sidx, eidx, case_sensitive,
                                    nullptr);
    return MatchResult(item, score, std::move(positions));
}

MatchResult Matcher::suffix_match(const std::shared_ptr<Item>& item,
                                  const std::vector<CodePoint>& pattern,
                                  bool case_sensitive)
{
    const auto& text = item->code_points();
    if (pattern.empty()) {
        return MatchResult(item, 0);
    }
    // fzf ignores the item's trailing whitespace unless the pattern itself
    // ends with whitespace.
    size_t trimmed_len = text.size();
    if (!is_space_cp(pattern.back())) {
        trimmed_len -= trailing_whitespaces(text);
    }
    if (trimmed_len < pattern.size()) {
        return MatchResult();
    }
    size_t diff = trimmed_len - pattern.size();
    for (size_t k = 0; k < pattern.size(); ++k) {
        if (!char_equal(text[diff + k], pattern[k], case_sensitive)) {
            return MatchResult();
        }
    }
    size_t sidx = diff, eidx = trimmed_len;
    std::vector<MatchPos> positions;
    positions.reserve(pattern.size());
    for (size_t k = sidx; k < eidx; ++k) {
        positions.push_back({static_cast<uint32_t>(k),
                             static_cast<uint32_t>(k + 1)});
    }
    int32_t score = calculate_score(text, pattern, sidx, eidx, case_sensitive,
                                    nullptr);
    return MatchResult(item, score, std::move(positions));
}

MatchResult Matcher::equal_match(const std::shared_ptr<Item>& item,
                                 const std::vector<CodePoint>& pattern,
                                 bool case_sensitive)
{
    const auto& text = item->code_points();
    if (pattern.empty()) {
        return MatchResult();
    }
    size_t lead = is_space_cp(pattern.front()) ? 0 : leading_whitespaces(text);
    size_t trail = is_space_cp(pattern.back()) ? 0 : trailing_whitespaces(text);
    if (lead + trail > text.size() ||
        text.size() - lead - trail != pattern.size()) {
        return MatchResult();
    }
    for (size_t k = 0; k < pattern.size(); ++k) {
        if (!char_equal(text[lead + k], pattern[k], case_sensitive)) {
            return MatchResult();
        }
    }
    // fzf's fixed equal-match score.
    int32_t n = static_cast<int32_t>(pattern.size());
    int32_t score = (SCORE_MATCH + BONUS_BOUNDARY_WHITE) * n +
                    (BONUS_FIRST_CHAR_MULTIPLIER - 1) * BONUS_BOUNDARY_WHITE;
    std::vector<MatchPos> positions;
    positions.reserve(pattern.size());
    for (size_t k = lead; k < lead + pattern.size(); ++k) {
        positions.push_back({static_cast<uint32_t>(k),
                             static_cast<uint32_t>(k + 1)});
    }
    return MatchResult(item, score, std::move(positions));
}

// Parse a raw query into extended-search term sets: fzf's parseTerms.
// Space-separated terms AND; "|" between terms OR; per-term prefixes and
// suffixes select the match type (see PatternTerm). "\ " escapes a literal
// space inside a term.
std::vector<TermSet> Matcher::parse_terms(const std::string& pattern) const {
    // Trim leading spaces; trim trailing spaces unless escaped ("\\ ").
    std::string trimmed = pattern;
    size_t begin = trimmed.find_first_not_of(' ');
    trimmed.erase(0, begin == std::string::npos ? trimmed.size() : begin);
    while (!trimmed.empty() && trimmed.back() == ' ' &&
           !(trimmed.size() >= 2 && trimmed[trimmed.size() - 2] == '\\')) {
        trimmed.pop_back();
    }

    // Protect escaped spaces, then split on runs of spaces.
    std::string protected_str;
    protected_str.reserve(trimmed.size());
    for (size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i] == '\\' && i + 1 < trimmed.size() &&
            trimmed[i + 1] == ' ') {
            protected_str += '\t';  // placeholder, restored per token below
            ++i;
        } else {
            protected_str += trimmed[i];
        }
    }

    std::vector<std::string> tokens;
    {
        size_t i = 0;
        while (i < protected_str.size()) {
            size_t sp = protected_str.find(' ', i);
            if (sp == std::string::npos) {
                tokens.push_back(protected_str.substr(i));
                break;
            }
            tokens.push_back(protected_str.substr(i, sp - i));
            i = sp;
            while (i < protected_str.size() && protected_str[i] == ' ') {
                ++i;
            }
        }
    }

    const bool fuzzy = !exact_;
    std::vector<TermSet> sets;
    TermSet set;
    bool switch_set = false;
    bool after_bar = false;

    for (auto& token : tokens) {
        std::string text = token;
        for (char& c : text) {
            if (c == '\t') c = ' ';  // restore escaped spaces
        }

        // Smart case is decided per term, on the token as typed (before
        // stripping the syntax characters -- they're symbols, so this
        // matches deciding on the stripped text like fzf does).
        std::string lower = ascii_lower(text);
        bool term_case_sensitive =
            case_mode_ == CaseMode::Respect ||
            (case_mode_ == CaseMode::Smart && text != lower);
        if (!term_case_sensitive) {
            text = lower;
        }

        PatternTerm::Type typ =
            fuzzy ? PatternTerm::Type::Fuzzy : PatternTerm::Type::Exact;
        bool inv = false;

        if (!set.empty() && !after_bar && text == "|") {
            switch_set = false;
            after_bar = true;
            continue;
        }
        after_bar = false;

        if (!text.empty() && text[0] == '!') {
            inv = true;
            typ = PatternTerm::Type::Exact;
            text.erase(0, 1);
        }

        if (text != "$" && !text.empty() && text.back() == '$') {
            typ = PatternTerm::Type::Suffix;
            text.pop_back();
        }

        if (text.size() > 2 && text.front() == '\'' && text.back() == '\'') {
            typ = PatternTerm::Type::ExactBoundary;
            text = text.substr(1, text.size() - 2);
        } else if (!text.empty() && text[0] == '\'') {
            // Flip exactness
            if (fuzzy && !inv) {
                typ = PatternTerm::Type::Exact;
            } else {
                typ = PatternTerm::Type::Fuzzy;
            }
            text.erase(0, 1);
        } else if (!text.empty() && text[0] == '^') {
            typ = (typ == PatternTerm::Type::Suffix) ? PatternTerm::Type::Equal
                                                     : PatternTerm::Type::Prefix;
            text.erase(0, 1);
        }

        if (!text.empty()) {
            if (switch_set) {
                sets.push_back(std::move(set));
                set = TermSet{};
            }
            PatternTerm term;
            term.type = typ;
            term.inverse = inv;
            term.case_sensitive = term_case_sensitive;
            term.text = to_code_points(text);
            set.push_back(std::move(term));
            switch_set = true;
        }
    }
    if (!set.empty()) {
        sets.push_back(std::move(set));
    }
    return sets;
}

MatchResult Matcher::match_term(const std::shared_ptr<Item>& item,
                                const PatternTerm& term) {
    switch (term.type) {
        case PatternTerm::Type::Fuzzy:
            return algo_ == AlgoType::FuzzyV1
                       ? fuzzy_match_v1(item, term.text, term.case_sensitive)
                       : fuzzy_match_v2(item, term.text, term.case_sensitive);
        case PatternTerm::Type::Exact:
            return exact_match_naive(item, term.text, term.case_sensitive, false);
        case PatternTerm::Type::ExactBoundary:
            return exact_match_naive(item, term.text, term.case_sensitive, true);
        case PatternTerm::Type::Prefix:
            return prefix_match(item, term.text, term.case_sensitive);
        case PatternTerm::Type::Suffix:
            return suffix_match(item, term.text, term.case_sensitive);
        case PatternTerm::Type::Equal:
            return equal_match(item, term.text, term.case_sensitive);
    }
    return MatchResult();
}

// Main match function: extended-search semantics (fzf's extendedMatch).
// Every term set must be satisfied (AND); within a set, alternatives are
// tried in order (OR). An inverse term satisfies its set by NOT matching.
MatchResult Matcher::match(const std::shared_ptr<Item>& item,
                           const std::string& pattern)
{
    if (pattern.empty()) {
        return MatchResult(item, 0);
    }

    if (!cached_valid_ || cached_pattern_ != pattern) {
        cached_sets_ = parse_terms(pattern);
        cached_pattern_ = pattern;
        cached_valid_ = true;
    }
    const auto& sets = cached_sets_;

    if (sets.empty()) {
        return MatchResult(item, 0);  // whitespace-only query matches all
    }

    int32_t total_score = 0;
    std::vector<MatchPos> all_positions;

    for (const auto& set : sets) {
        bool matched = false;
        int32_t current_score = 0;
        std::vector<MatchPos> current_positions;

        for (const auto& term : set) {
            MatchResult r = match_term(item, term);
            if (r.item) {
                if (term.inverse) {
                    // The forbidden text IS present: this alternative fails.
                    continue;
                }
                current_score = r.score;
                current_positions = std::move(r.positions);
                matched = true;
                break;
            } else if (term.inverse) {
                // Absent as required. Keep trying later alternatives -- a
                // positive one can still contribute score/highlights.
                current_score = 0;
                current_positions.clear();
                matched = true;
                continue;
            }
        }

        if (!matched) {
            return MatchResult();  // an AND clause failed
        }
        total_score += current_score;
        all_positions.insert(all_positions.end(), current_positions.begin(),
                             current_positions.end());
    }

    std::sort(all_positions.begin(), all_positions.end(),
              [](const MatchPos& a, const MatchPos& b) { return a.start < b.start; });

    return MatchResult(item, total_score, std::move(all_positions));
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
        // A returned item IS a match -- do not filter on score. Scores only
        // rank (fuzzy matches are always positive, but an inverse-only query
        // like "!foo" legitimately matches with score 0).
        if (result.item) {
            results.push_back(std::move(result));
        }
    }

    // fzf's sortable flag: a query with no positive term (empty, whitespace-
    // only, or all-!negations) keeps input order -- every score is 0 and a
    // score/length sort would scramble the list.
    if (!cached_valid_ || cached_pattern_ != pattern) {
        cached_sets_ = parse_terms(pattern);
        cached_pattern_ = pattern;
        cached_valid_ = true;
    }
    bool sortable = false;
    for (const auto& set : cached_sets_) {
        for (const auto& term : set) {
            if (!term.inverse) {
                sortable = true;
            }
        }
    }

    // Sort by score (descending), then fzf's default tiebreak.
    if (sortable) {
        std::sort(results.begin(), results.end());
    }

    return results;
}

} // namespace fzf
