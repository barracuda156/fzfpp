#pragma once

// Shared small types for items and matching. The item storage itself is
// in chunklist.hpp (fixed-capacity chunks); this header only carries the
// enums and aliases that options.hpp / matcher.hpp / terminal.hpp share.

#include <cstdint>

namespace fzf {

// UTF-32 code point representation for matching
using CodePoint = char32_t;

// Case sensitivity mode
enum class CaseMode {
    Smart,      // Case-insensitive if pattern is lowercase, otherwise sensitive
    Ignore,     // Always case-insensitive
    Respect     // Always case-sensitive
};

// A codepoint range of an item's text (used by --nth restriction and by
// match-region bookkeeping).
struct RuneRange {
    uint32_t start;
    uint32_t len;
};

// Go's unicode.IsSpace, which fzf uses for trimming and for the chunk/
// begin/end tiebreaks (fzf: src/result.go, util/chars.go TrimLength).
inline bool is_unicode_space(CodePoint r) {
    switch (r) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r': case U' ':
        case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
        case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return r >= 0x2000 && r <= 0x200A;
    }
}

// Matching algorithm selection
enum class AlgoType {
    FuzzyV1,    // Fast greedy algorithm
    FuzzyV2     // Smith-Waterman optimal algorithm
};

} // namespace fzf
