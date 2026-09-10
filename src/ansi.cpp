// fzf: src/ansi.go -- the subset of escape handling fzf applies to input
// lines: SGR (CSI ... m) is interpreted, every other CSI / OSC / two-byte
// escape is removed, and the remaining text is returned with color runs.

#include "ansi.hpp"
#include "options.hpp"

#include <cstdlib>

namespace fzf {

namespace {

// Applies one SGR parameter list to the state (fzf: ansi.go applyAnsiCode).
void apply_sgr(AnsiState& st, const char* p, size_t n) {
    // Parse ';'-separated integers; empty list means reset.
    int codes[64];
    int count = 0;
    {
        size_t i = 0;
        while (i <= n && count < 64) {
            int v = 0;
            bool any = false;
            while (i < n && p[i] >= '0' && p[i] <= '9') { v = v * 10 + (p[i] - '0'); ++i; any = true; }
            codes[count++] = any ? v : 0;
            if (i < n && (p[i] == ';' || p[i] == ':')) { ++i; continue; }
            break;
        }
    }
    if (count == 0) { st = AnsiState{}; return; }
    for (int i = 0; i < count; ++i) {
        int c = codes[i];
        switch (c) {
            case 0: st = AnsiState{}; break;
            case 1: st.attr |= kAttrBold; break;
            case 2: st.attr |= kAttrDim; break;
            case 3: st.attr |= kAttrItalic; break;
            case 4: st.attr |= kAttrUnderline; break;
            case 5: st.attr |= kAttrBlink; break;
            case 6: st.attr |= kAttrBlink2; break;
            case 7: st.attr |= kAttrReverse; break;
            case 9: st.attr |= kAttrStrikeThrough; break;
            case 22: st.attr &= ~(kAttrBold | kAttrDim); break;
            case 23: st.attr &= ~kAttrItalic; break;
            case 24: st.attr &= ~kAttrUnderline; break;
            case 25: st.attr &= ~(kAttrBlink | kAttrBlink2); break;
            case 27: st.attr &= ~kAttrReverse; break;
            case 29: st.attr &= ~kAttrStrikeThrough; break;
            case 39: st.fg = -1; break;
            case 49: st.bg = -1; break;
            case 38: case 48: {
                int32_t* target = (c == 38) ? &st.fg : &st.bg;
                if (i + 1 < count && codes[i + 1] == 5 && i + 2 < count) {
                    *target = codes[i + 2];
                    i += 2;
                } else if (i + 1 < count && codes[i + 1] == 2 && i + 4 < count) {
                    int r = codes[i + 2] & 0xff, g = codes[i + 3] & 0xff, b = codes[i + 4] & 0xff;
                    *target = (1 << 24) + (r << 16) + (g << 8) + b;
                    i += 4;
                }
                break;
            }
            default:
                if (c >= 30 && c <= 37) st.fg = c - 30;
                else if (c >= 40 && c <= 47) st.bg = c - 40;
                else if (c >= 90 && c <= 97) st.fg = c - 90 + 8;
                else if (c >= 100 && c <= 107) st.bg = c - 100 + 8;
                break;
        }
    }
}

// Length of the escape sequence starting at s[i] (s[i] == ESC), or 1 for a
// lone/unknown ESC. Sets `sgr` to the parameter bytes when it is CSI ... m.
size_t escape_length(std::string_view s, size_t i, std::string_view& sgr) {
    sgr = {};
    size_t n = s.size();
    if (i + 1 >= n) return 1;
    unsigned char next = static_cast<unsigned char>(s[i + 1]);
    if (next == '[') {
        size_t j = i + 2;
        while (j < n) {
            unsigned char b = static_cast<unsigned char>(s[j]);
            if (b >= 0x40 && b <= 0x7e) {
                if (b == 'm') sgr = s.substr(i + 2, j - (i + 2));
                return j - i + 1;
            }
            ++j;
        }
        return n - i;   // unterminated: drop the rest
    }
    if (next == ']') {   // OSC ... BEL or ESC-backslash
        size_t j = i + 2;
        while (j < n) {
            unsigned char b = static_cast<unsigned char>(s[j]);
            if (b == 0x07) return j - i + 1;
            if (b == 0x1b && j + 1 < n && s[j + 1] == '\\') return j - i + 2;
            ++j;
        }
        return n - i;
    }
    if (next == 'P' || next == '_' || next == '^' || next == 'X') {   // DCS/APC/PM/SOS ... ST
        size_t j = i + 2;
        while (j < n) {
            if (static_cast<unsigned char>(s[j]) == 0x1b && j + 1 < n && s[j + 1] == '\\') return j - i + 2;
            ++j;
        }
        return n - i;
    }
    if (next == '(' || next == ')' || next == '*' || next == '+') return (i + 2 < n) ? 3 : n - i;
    return 2;   // two-byte escape (ESC c, ESC M, ...)
}

} // namespace

uint32_t extract_color(std::string_view input, std::string& out,
                       std::vector<ColorRun>& runs, bool& ascii) {
    out.clear();
    runs.clear();
    ascii = true;
    AnsiState st;
    uint32_t runes = 0;
    uint32_t run_start = 0;
    AnsiState run_state;
    bool in_run = false;

    auto close_run = [&]() {
        if (in_run && runes > run_start) {
            runs.push_back(ColorRun{run_start, runes, run_state.fg, run_state.bg, run_state.attr});
        }
        in_run = false;
    };

    size_t i = 0, n = input.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == 0x1b) {
            std::string_view sgr;
            size_t len = escape_length(input, i, sgr);
            if (sgr.data() != nullptr) {
                AnsiState before = st;
                apply_sgr(st, sgr.data(), sgr.size());
                if (!(before.fg == st.fg && before.bg == st.bg && before.attr == st.attr)) {
                    close_run();
                    if (st.active()) { in_run = true; run_start = runes; run_state = st; }
                }
            }
            i += len;
            continue;
        }
        // Copy one UTF-8 sequence.
        size_t clen = 1;
        if (c >= 0x80) {
            ascii = false;
            if ((c & 0xe0) == 0xc0) clen = 2;
            else if ((c & 0xf0) == 0xe0) clen = 3;
            else if ((c & 0xf8) == 0xf0) clen = 4;
            if (i + clen > n) clen = n - i;
        }
        out.append(input.data() + i, clen);
        i += clen;
        ++runes;
    }
    close_run();
    return runes;
}

std::string strip_ansi(std::string_view input) {
    std::string out;
    std::vector<ColorRun> runs;
    bool ascii;
    extract_color(input, out, runs, ascii);
    return out;
}

} // namespace fzf
