#pragma once

#include <cstdint>
#include <string>

namespace fzf {

// Byte length of the UTF-8 sequence introduced by `first_byte` (1 for
// ASCII and for invalid lead bytes).
inline size_t utf8_char_length(char first_byte) {
    unsigned char c = static_cast<unsigned char>(first_byte);
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // Invalid, treat as single byte
}

// Strip ANSI escape codes from a string. Only ESC (0x1b) introduces a
// sequence -- this is a UTF-8 world, and the 8-bit C1 control range (0x80-
// 0x9f), including 0x9b (the 8-bit CSI introducer some legacy code treats as
// equivalent to "ESC ["), overlaps UTF-8 continuation bytes. 0x9b is exactly
// the trailing byte of e.g. 国 (E5 9B BD); treating it as CSI mid-codepoint
// swallowed the rest of the character plus everything up to the next ASCII
// letter, corrupting CJK text. So C1 handling is dropped entirely: any byte
// that isn't ESC is passed through untouched (multibyte UTF-8 sequences
// included), and only real ESC-introduced sequences are parsed/dropped:
//   - CSI: ESC [ <params/intermediates> <final 0x40-0x7E> (matches
//     classify_escape_seq/sanitize_preview_line in render.cpp -- finals are
//     the whole 0x40-0x7E range, not just ASCII letters, e.g. '@' and '~').
//   - OSC: ESC ] ... up to ST (ESC \) or BEL (0x07).
//   - Other two-byte ESC sequences (ESC c, ESC D, etc.): drop the ESC and
//     the one byte after it.
// An unterminated sequence at the end of the string is dropped to the end
// (nothing to keep, no terminator was seen).
inline std::string strip_ansi_codes(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    const size_t n = str.size();
    size_t i = 0;
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        if (c != 0x1b) {
            result += str[i];
            ++i;
            continue;
        }

        unsigned char next = (i + 1 < n) ? static_cast<unsigned char>(str[i + 1]) : 0;

        if (next == '[') {
            // CSI: scan to the final byte, 0x40-0x7E (fzf/vt100 range covers
            // '@'-'~', not just letters -- e.g. SGR 'm' but also '@' ICH, '`'
            // HPA, etc.).
            size_t j = i + 2;
            while (j < n) {
                unsigned char b = static_cast<unsigned char>(str[j]);
                if (b >= 0x40 && b <= 0x7e) { ++j; break; }
                ++j;
            }
            i = j;
            continue;
        }

        if (next == ']') {
            // OSC: scan to ST (ESC \) or BEL.
            size_t j = i + 2;
            while (j < n) {
                unsigned char b = static_cast<unsigned char>(str[j]);
                if (b == 0x07) { ++j; break; }
                if (b == 0x1b && j + 1 < n && static_cast<unsigned char>(str[j + 1]) == '\\') {
                    j += 2;
                    break;
                }
                ++j;
            }
            i = j;
            continue;
        }

        if (i + 1 >= n) {
            // Lone trailing ESC: nothing to keep.
            i = n;
            continue;
        }

        // Other two-byte escape sequence (ESC c, ESC D, ESC M, ...).
        i += 2;
    }

    return result;
}

} // namespace fzf
