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

template <class Text>
size_t leading_whitespaces(const Text& text) {
    size_t n = 0;
    while (n < text.size() && is_space_cp(text[n])) {
        ++n;
    }
    return n;
}

template <class Text>
size_t trailing_whitespaces(const Text& text) {
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

// Per-thread scratch memory: nothing is allocated per match in steady
// state (fzf uses a per-thread slab for the same reason).
struct Scratch {
    std::vector<CodePoint> runes;     // decoded non-ASCII text
    std::vector<int32_t> bonus;
    std::vector<int32_t> M;
    std::vector<int32_t> run;
    std::vector<int32_t> from;
    std::vector<uint32_t> term_positions;
};

Scratch& scratch() {
    thread_local Scratch s;
    return s;
}

// Decode UTF-8 into the scratch rune buffer (invalid bytes as Latin-1).
void decode_runes(std::string_view text, std::vector<CodePoint>& out) {
    out.clear();
    out.reserve(text.size());
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x80) { out.push_back(c); ++p; continue; }
        try {
            const char* q = p;
            CodePoint cp = utf8::next(q, end);
            out.push_back(cp);
            p = q;
        } catch (...) {
            out.push_back(c);
            ++p;
        }
    }
}

// A sub-range view used for --nth restriction.
template <class Text>
struct SubText {
    Text base;
    size_t offset;
    size_t n;
    size_t size() const { return n; }
    CodePoint operator[](size_t i) const { return base[offset + i]; }
};

} // namespace

CodePoint Matcher::normalize_char(CodePoint c) const {
    return normalize_ ? strip_accent(c) : c;
}

bool Matcher::char_equal(CodePoint a, CodePoint b, bool case_sensitive) const {
    // Accent stripping (fzf's `normalize`) is independent of case
    // sensitivity: `--case-sensitive cafe` still matches "café".
    if (normalize_) {
        a = strip_accent(a);
        b = strip_accent(b);
    }
    if (case_sensitive) {
        return a == b;
    }
    if (a >= 'A' && a <= 'Z') a += ('a' - 'A');
    if (b >= 'A' && b <= 'Z') b += ('a' - 'A');
    return a == b;
}

// Determine character class for bonus calculation (fzf's charClassOf; the
// delimiter set is fzf's default-scheme delimiterChars "/,:;|").
CharClass Matcher::char_class_of(CodePoint c) {
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
int32_t Matcher::bonus_for(CharClass prev, CharClass curr) {
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

template <class Text>
int32_t Matcher::bonus_at(const Text& text, size_t idx) const {
    if (idx == 0) {
        return BONUS_BOUNDARY_WHITE;
    }
    return bonus_for(char_class_of(text[idx - 1]), char_class_of(text[idx]));
}

// fzf's calculateScore: walk a known match region and accumulate the same
// bonuses/penalties V2 would assign, so exact/prefix/suffix results rank
// comparably against fuzzy ones.
template <class Text>
int32_t Matcher::calculate_score(const Text& text,
                                 const std::vector<CodePoint>& pattern,
                                 size_t sidx, size_t eidx, bool case_sensitive,
                                 std::vector<uint32_t>* positions) const {
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
        c = normalize_char(c);
        CodePoint p = pidx < pattern.size() ? normalize_char(pattern[pidx]) : 0;
        if (!case_sensitive) {
            if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
            if (p >= 'A' && p <= 'Z') p += ('a' - 'A');
        }
        if (pidx < pattern.size() && c == p) {
            if (positions) {
                positions->push_back(static_cast<uint32_t>(idx));
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

namespace {
MatchResult found(int32_t score, size_t sidx, size_t eidx) {
    MatchResult r;
    r.matched = true;
    r.score = score;
    r.begin = static_cast<int32_t>(sidx);
    r.end = static_cast<int32_t>(eidx);
    return r;
}
MatchResult empty_match() {
    MatchResult r;
    r.matched = true;
    return r;
}
} // namespace

// Greedy fuzzy matching algorithm (V1) - fzf's FuzzyMatchV1: forward scan to
// find the first subsequence, backward scan to shrink its window, then
// calculateScore over the window.
template <class Text>
MatchResult Matcher::fuzzy_match_v1(const Text& text,
                                    const std::vector<CodePoint>& pattern,
                                    bool case_sensitive,
                                    std::vector<uint32_t>* positions) const
{
    size_t pattern_len = pattern.size();
    size_t text_len = text.size();

    if (pattern_len == 0) {
        return empty_match();
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

    int32_t score = calculate_score(text, pattern, static_cast<size_t>(sidx),
                                    static_cast<size_t>(eidx), case_sensitive,
                                    positions);
    return found(score, static_cast<size_t>(sidx), static_cast<size_t>(eidx));
}

// Optimal fuzzy matching (V2) - a port of fzf's FuzzyMatchV2 recurrence.
//
// Whether an item MATCHES is decided solely by the forward feasibility scan
// (does the pattern appear as a subsequence?) -- scoring only ranks. fzf's H
// values floor at 0 (max(s1, s2, 0)), so gap decay can never turn a real
// subsequence into a non-match; every match scores >= SCORE_MATCH.
//
// Cell semantics (full-matrix form of fzf's rolling rows, kept so that
// backtracking can follow explicit predecessor links):
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
template <class Text>
MatchResult Matcher::fuzzy_match_v2(const Text& text,
                                    const std::vector<CodePoint>& pattern,
                                    bool case_sensitive,
                                    std::vector<uint32_t>* positions) const
{
    size_t pattern_len = pattern.size();
    size_t text_len = text.size();

    if (pattern_len == 0) {
        return empty_match();
    }
    if (text_len == 0 || pattern_len > text_len) {
        return MatchResult();  // No match
    }

    // Feasibility: the pattern must appear as a subsequence. This alone
    // decides match/no-match, like fzf's phase-2 pidx check. It also gives
    // the first possible column of the first pattern char and the last
    // possible column of the last one, which bounds the DP sweep.
    size_t first_col = 0;
    size_t last_col = text_len;
    {
        size_t text_idx = 0;
        for (size_t pat_idx = 0; pat_idx < pattern_len; ++pat_idx) {
            bool found_c = false;
            while (text_idx < text_len) {
                if (char_equal(text[text_idx], pattern[pat_idx], case_sensitive)) {
                    if (pat_idx == 0) first_col = text_idx;
                    found_c = true;
                    text_idx++;
                    break;
                }
                text_idx++;
            }
            if (!found_c) {
                return MatchResult();  // No match
            }
        }
        // Backward scan for the last possible end column.
        size_t pidx = pattern_len;
        for (size_t j = text_len; j > first_col && pidx > 0; --j) {
            if (char_equal(text[j - 1], pattern[pidx - 1], case_sensitive)) {
                if (pidx == pattern_len) last_col = j;
                --pidx;
            }
        }
    }

    // Work on the window [first_col, last_col) only; offsets are added back
    // when reporting positions.
    SubText<Text> win{text, first_col, last_col - first_col};
    size_t wlen = win.size();

    Scratch& sc = scratch();
    // Per-position boundary bonuses.
    sc.bonus.resize(wlen);
    CharClass prev_class = first_col > 0 ? char_class_of(text[first_col - 1]) : CharClass::CharWhite;
    for (size_t i = 0; i < wlen; ++i) {
        CharClass curr_class = char_class_of(win[i]);
        sc.bonus[i] = bonus_for(prev_class, curr_class);
        prev_class = curr_class;
    }

    size_t cells = pattern_len * wlen;
    sc.M.assign(cells, 0);
    sc.run.assign(cells, 0);
    sc.from.assign(cells, -1);
    int32_t* M = sc.M.data();
    int32_t* run = sc.run.data();
    int32_t* from = sc.from.data();
    const int32_t* bonus = sc.bonus.data();
    auto at = [wlen](size_t i, size_t j) { return i * wlen + j; };

    int32_t max_score = 0;
    size_t max_score_pos = 0;

    // Row 0: every match of pattern[0] seeds a run; first-char bonus doubled.
    for (size_t j = 0; j < wlen; ++j) {
        if (char_equal(pattern[0], win[j], case_sensitive)) {
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

        for (size_t j = 0; j < wlen; ++j) {
            int32_t s2 = hcur + (cur_in_gap ? SCORE_GAP_EXTENSION : SCORE_GAP_START);
            int32_t s1 = 0;
            int32_t consecutive = 0;

            // A cell is only valid once some completion of pattern[0..i-1]
            // exists strictly to the left (hprev_col >= 0) -- the full-sweep
            // equivalent of fzf starting row i at F[i].
            if (hprev_col >= 0 &&
                char_equal(pattern_char, win[j], case_sensitive)) {
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

    // Backtrack along the recorded predecessor columns to find the start
    // (and the positions if requested).
    long i = static_cast<long>(pattern_len) - 1;
    long j = static_cast<long>(max_score_pos);
    long start = j;
    size_t pos_begin = positions ? positions->size() : 0;
    while (i >= 0 && j >= 0) {
        if (positions) positions->push_back(static_cast<uint32_t>(j + first_col));
        start = j;
        j = from[at(static_cast<size_t>(i), static_cast<size_t>(j))];
        --i;
    }
    if (positions) {
        std::reverse(positions->begin() + static_cast<long>(pos_begin), positions->end());
    }

    return found(max_score, static_cast<size_t>(start) + first_col,
                 max_score_pos + 1 + first_col);
}

// fzf's ExactMatchNaive / ExactMatchBoundary: the whole pattern as one
// contiguous run; among all occurrences keep the one whose FIRST character
// has the highest boundary bonus (earliest such occurrence wins; the scan
// stops early once a boundary-quality occurrence is found).
template <class Text>
MatchResult Matcher::exact_match_naive(const Text& text,
                                       const std::vector<CodePoint>& pattern,
                                       bool case_sensitive, bool boundary_check,
                                       std::vector<uint32_t>* positions) const
{
    size_t pattern_len = pattern.size();
    size_t text_len = text.size();

    if (pattern_len == 0) {
        return empty_match();
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

    if (positions) {
        for (size_t k = sidx; k < eidx; ++k) positions->push_back(static_cast<uint32_t>(k));
    }
    return found(score, sidx, eidx);
}

template <class Text>
MatchResult Matcher::prefix_match(const Text& text,
                                  const std::vector<CodePoint>& pattern,
                                  bool case_sensitive,
                                  std::vector<uint32_t>* positions) const
{
    if (pattern.empty()) {
        return empty_match();
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
    if (positions) {
        for (size_t k = sidx; k < eidx; ++k) positions->push_back(static_cast<uint32_t>(k));
    }
    int32_t score = calculate_score(text, pattern, sidx, eidx, case_sensitive,
                                    nullptr);
    return found(score, sidx, eidx);
}

template <class Text>
MatchResult Matcher::suffix_match(const Text& text,
                                  const std::vector<CodePoint>& pattern,
                                  bool case_sensitive,
                                  std::vector<uint32_t>* positions) const
{
    if (pattern.empty()) {
        return empty_match();
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
    if (positions) {
        for (size_t k = sidx; k < eidx; ++k) positions->push_back(static_cast<uint32_t>(k));
    }
    int32_t score = calculate_score(text, pattern, sidx, eidx, case_sensitive,
                                    nullptr);
    return found(score, sidx, eidx);
}

template <class Text>
MatchResult Matcher::equal_match(const Text& text,
                                 const std::vector<CodePoint>& pattern,
                                 bool case_sensitive,
                                 std::vector<uint32_t>* positions) const
{
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
    if (positions) {
        for (size_t k = lead; k < lead + pattern.size(); ++k) positions->push_back(static_cast<uint32_t>(k));
    }
    return found(score, lead, lead + pattern.size());
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
        bool term_case_sensitive = Matcher::term_case_sensitive(case_mode_, text);
        if (!term_case_sensitive) {
            text = ascii_lower(text);
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

bool Matcher::term_case_sensitive(CaseMode mode, const std::string& text) {
    return mode == CaseMode::Respect ||
           (mode == CaseMode::Smart && text != ascii_lower(text));
}

void Matcher::set_pattern(const std::string& pattern) {
    pattern_ = pattern;
    set_terms(parse_terms(pattern));
}

void Matcher::set_terms(std::vector<TermSet> sets) {
    sets_ = std::move(sets);
    sortable_ = false;
    for (const auto& set : sets_) {
        for (const auto& term : set) {
            if (!term.inverse) sortable_ = true;
        }
    }
}

const std::vector<CodePoint>& Matcher::scratch_runes() {
    return scratch().runes;
}

template <class Text>
MatchResult Matcher::match_term(const Text& text, const PatternTerm& term,
                                std::vector<uint32_t>* positions) const {
    switch (term.type) {
        case PatternTerm::Type::Fuzzy:
            return algo_ == AlgoType::FuzzyV1
                       ? fuzzy_match_v1(text, term.text, term.case_sensitive, positions)
                       : fuzzy_match_v2(text, term.text, term.case_sensitive, positions);
        case PatternTerm::Type::Exact:
            return exact_match_naive(text, term.text, term.case_sensitive, false, positions);
        case PatternTerm::Type::ExactBoundary:
            return exact_match_naive(text, term.text, term.case_sensitive, true, positions);
        case PatternTerm::Type::Prefix:
            return prefix_match(text, term.text, term.case_sensitive, positions);
        case PatternTerm::Type::Suffix:
            return suffix_match(text, term.text, term.case_sensitive, positions);
        case PatternTerm::Type::Equal:
            return equal_match(text, term.text, term.case_sensitive, positions);
    }
    return MatchResult();
}

// Main match function: extended-search semantics (fzf's extendedMatch).
// Every term set must be satisfied (AND); within a set, alternatives are
// tried in order (OR). An inverse term satisfies its set by NOT matching.
// `base` is added to reported positions/bounds (for --nth sub-ranges).
template <class Text>
MatchResult Matcher::match_text(const Text& text, uint32_t base,
                                std::vector<uint32_t>* positions) const {
    MatchResult total;
    total.matched = true;
    total.score = 0;
    int32_t min_begin = -1, max_end = -1, min_end = -1;

    Scratch& sc = scratch();
    for (const auto& set : sets_) {
        bool matched = false;
        MatchResult chosen;
        size_t pos_mark = positions ? positions->size() : 0;
        for (const auto& term : set) {
            if (positions) positions->resize(pos_mark);
            MatchResult r = match_term(text, term, positions);
            if (r.matched) {
                if (term.inverse) {
                    // The forbidden text IS present: this alternative fails.
                    if (positions) positions->resize(pos_mark);
                    continue;
                }
                chosen = r;
                matched = true;
                break;
            } else if (term.inverse) {
                // Absent as required. Keep trying later alternatives -- a
                // positive one can still contribute score/highlights.
                chosen = MatchResult();
                chosen.matched = true;
                matched = true;
                continue;
            }
        }
        if (!matched) {
            if (positions) positions->resize(pos_mark);
            return MatchResult();  // an AND clause failed
        }
        total.score += chosen.score;
        // fzf: buildResult only counts offsets with begin < end.
        if (chosen.begin >= 0 && chosen.begin < chosen.end) {
            if (min_begin < 0 || chosen.begin < min_begin) min_begin = chosen.begin;
            if (min_end < 0 || chosen.end < min_end) min_end = chosen.end;
            if (chosen.end > max_end) max_end = chosen.end;
        }
    }
    (void)sc;
    if (min_begin >= 0) {
        total.begin = min_begin + static_cast<int32_t>(base);
        total.end = max_end + static_cast<int32_t>(base);
        total.min_end = min_end + static_cast<int32_t>(base);
    }
    if (positions && base > 0) {
        for (auto& p : *positions) p += base;
    }
    return total;
}

MatchResult Matcher::match(std::string_view text, bool ascii,
                           const RuneRange* nth, size_t nth_count,
                           std::vector<uint32_t>* positions) const {
    if (positions) positions->clear();
    if (sets_.empty()) {
        return empty_match();   // empty / whitespace-only query matches all
    }

    // fzf: with --nth, try each transformed part in order; first hit wins.
    if (ascii) {
        AsciiText t{text.data(), text.size()};
        if (nth_count == 0) {
            MatchResult r = match_text(t, 0, positions);
            if (positions) std::sort(positions->begin(), positions->end());
            return r;
        }
        for (size_t k = 0; k < nth_count; ++k) {
            size_t start = std::min<size_t>(nth[k].start, t.size());
            size_t len = std::min<size_t>(nth[k].len, t.size() - start);
            AsciiText sub{t.p + start, len};
            MatchResult r = match_text(sub, static_cast<uint32_t>(start), positions);
            if (r.matched) {
                if (positions) std::sort(positions->begin(), positions->end());
                return r;
            }
        }
        return MatchResult();
    }

    Scratch& sc = scratch();
    decode_runes(text, sc.runes);
    RuneText t{sc.runes.data(), sc.runes.size()};
    if (nth_count == 0) {
        MatchResult r = match_text(t, 0, positions);
        if (positions) std::sort(positions->begin(), positions->end());
        return r;
    }
    for (size_t k = 0; k < nth_count; ++k) {
        size_t start = std::min<size_t>(nth[k].start, t.size());
        size_t len = std::min<size_t>(nth[k].len, t.size() - start);
        RuneText sub{t.p + start, len};
        MatchResult r = match_text(sub, static_cast<uint32_t>(start), positions);
        if (r.matched) {
            if (positions) std::sort(positions->begin(), positions->end());
            return r;
        }
    }
    return MatchResult();
}

MatchResult Matcher::match(std::string_view text, std::vector<uint32_t>* positions) const {
    bool ascii = true;
    for (unsigned char c : text) {
        if (c >= 0x80) { ascii = false; break; }
    }
    return match(text, ascii, nullptr, 0, positions);
}

} // namespace fzf
