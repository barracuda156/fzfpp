#pragma once

// ANSI escape handling for input items (fzf: src/ansi.go extractColor).
// With --ansi, every input line is stripped of escape sequences at read
// time and its SGR colors are recorded as ColorRun entries in codepoint
// offsets, so the matcher sees plain text and the renderer can recolor the
// visible rows. Without --ansi nothing is parsed (fzf keeps the raw line).

#include "chunklist.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace fzf {

struct AnsiState {
    int32_t fg = -1;     // kColorDefault
    int32_t bg = -1;
    uint32_t attr = 0;
    bool active() const { return fg != -1 || bg != -1 || attr != 0; }
};

// Strips escape sequences from `input`, appending the plain text to `out`
// (which is cleared first) and the color runs to `runs` (cleared first).
// Returns the number of codepoints in `out` and sets `ascii` when the
// stripped text is pure ASCII. Runs are coalesced and empty runs dropped.
uint32_t extract_color(std::string_view input, std::string& out,
                       std::vector<ColorRun>& runs, bool& ascii);

// True if `s` contains an ESC byte (cheap pre-check).
inline bool has_escape(std::string_view s) { return s.find('\x1b') != std::string_view::npos; }

// Plain strip without color bookkeeping (for --accept output with --ansi).
std::string strip_ansi(std::string_view input);

} // namespace fzf
