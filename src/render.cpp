#include "render.hpp"
#include "util.hpp"
#include "tty.hpp"

#include <algorithm>
#include <unistd.h>
#include <utf8.h>

namespace fzf {

namespace {

// Module-level tab stop (see set_tabstop in render.hpp). Tabs are expanded to
// spaces up to the next multiple of this width, measured from the current
// visible column within the row/pane being drawn -- NOT a fixed per-codepoint
// width, since how many columns a '\t' consumes depends on where it lands.
int g_tabstop = 8;

// Display-column width of a single Unicode codepoint, à la wcwidth(3). fzf++
// consumers show CJK/Hangul titles (each 2 columns), Nerd Font glyphs in the
// Private Use Area (2 columns in the fonts these TUIs assume), and combining
// marks (0). Counting every codepoint as 1 column — as the old code did — made
// a wide-char row's true width exceed the results pane, so it auto-wrapped at
// the terminal's right edge and pushed every row below it down (the "jumping"),
// and made per-row padding too short to erase the previous frame (stale text /
// superimposed lists). This table covers the ranges those consumers actually
// hit; it is deliberately compact, not a full Unicode width database.
// Extended_Pictographic emoji bases (the newer emoji-supplement block plus
// the legacy dingbat/symbol codepoints with default emoji presentation).
// Factored out of codepoint_width so GraphemeWidthScanner can ask "is this
// codepoint a pictograph a ZWJ sequence could be built on" without
// duplicating the range list.
bool is_extended_pictographic(char32_t cp) {
    return (cp >= 0x1f300 && cp <= 0x1faff) ||
           (cp >= 0x2614 && cp <= 0x2615) ||   // umbrella w/ rain..hot beverage
           (cp >= 0x2648 && cp <= 0x2653) ||   // Aries..Pisces
           cp == 0x267f ||                     // wheelchair symbol
           cp == 0x2693 ||                     // anchor
           cp == 0x26a1 ||                     // high voltage
           (cp >= 0x26aa && cp <= 0x26ab) ||   // white circle..black circle
           (cp >= 0x26bd && cp <= 0x26be) ||   // soccer ball..baseball
           (cp >= 0x26c4 && cp <= 0x26c5) ||   // snowman..sun behind cloud
           cp == 0x26ce ||                     // Ophiuchus
           cp == 0x26d4 ||                     // no entry
           cp == 0x26ea ||                     // church
           (cp >= 0x26f2 && cp <= 0x26f3) ||   // fountain..flag in hole
           cp == 0x26f5 ||                     // sailboat
           cp == 0x26fa ||                     // tent
           cp == 0x26fd ||                     // fuel pump
           cp == 0x2705 ||                     // check mark button
           (cp >= 0x270a && cp <= 0x270b) ||   // raised fist..raised hand
           cp == 0x2728 ||                     // sparkles
           cp == 0x274c ||                     // cross mark
           cp == 0x274e ||                     // cross mark button
           (cp >= 0x2753 && cp <= 0x2755) ||   // red question..white excl. mark
           cp == 0x2757 ||                     // red exclamation mark
           (cp >= 0x2795 && cp <= 0x2797) ||   // plus..divide
           cp == 0x27b0 ||                     // curly loop
           cp == 0x27bf ||                     // double curly loop
           (cp >= 0x2b1b && cp <= 0x2b1c) ||   // black/white large square
           cp == 0x2b50 ||                     // star
           cp == 0x2b55;                       // hollow red circle
}

int codepoint_width(char32_t cp) {
    if (cp == 0) return 0;
    // C0/C1 controls: not printable, treat as zero so they don't shift columns.
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;

    // Zero-width: combining marks, ZWJ/ZWNJ, variation selectors, BOM,
    // Fitzpatrick skin-tone modifiers (U+1F3FB-1F3FF only ever appear glued
    // to a preceding emoji base -- they recolor it in place rather than
    // adding a column; see GraphemeWidthScanner for why this alone isn't
    // enough to get ZWJ sequences right).
    if ((cp >= 0x0300 && cp <= 0x036f) ||   // combining diacritical marks
        (cp >= 0x200b && cp <= 0x200f) ||   // ZWSP..RLM
        (cp >= 0xfe00 && cp <= 0xfe0f) ||   // variation selectors
        cp == 0xfeff ||                     // BOM / ZWNBSP
        (cp >= 0x1f3fb && cp <= 0x1f3ff)) { // emoji skin-tone modifiers
        return 0;
    }

    // Wide (2-column) ranges.
    if ((cp >= 0x1100 && cp <= 0x115f) ||   // Hangul Jamo
        (cp >= 0x2e80 && cp <= 0x303e) ||   // CJK radicals, Kangxi, symbols
        (cp >= 0x3041 && cp <= 0x33ff) ||   // Hiragana..CJK compat
        (cp >= 0x3400 && cp <= 0x4dbf) ||   // CJK Ext A
        (cp >= 0x4e00 && cp <= 0x9fff) ||   // CJK Unified
        (cp >= 0xa000 && cp <= 0xa4cf) ||   // Yi
        (cp >= 0xac00 && cp <= 0xd7a3) ||   // Hangul syllables
        (cp >= 0xf900 && cp <= 0xfaff) ||   // CJK compat ideographs
        (cp >= 0xfe30 && cp <= 0xfe4f) ||   // CJK compat forms
        (cp >= 0xff00 && cp <= 0xff60) ||   // fullwidth forms
        (cp >= 0xffe0 && cp <= 0xffe6) ||   // fullwidth signs
        (cp >= 0x1f1e6 && cp <= 0x1f1ff) || // regional indicators (flags)
        (cp >= 0x20000 && cp <= 0x3fffd)) { // CJK Ext B+ (SIP)
        return 2;
    }

    // Emoji & pictographs (0x1f300-0x1faff), plus legacy symbol/dingbat
    // codepoints with DEFAULT emoji presentation (Emoji_Presentation=Yes per
    // Unicode emoji-data.txt) -- the latter render wide even without a
    // trailing VS16, unlike most of their neighbors in the same block which
    // stay text-presentation (narrow) until VS16. Confirmed this exactly
    // matches East_Asian_Width=Wide for those legacy blocks, and matches
    // go-runewidth's default doublewidth table. Real titles (YouTube-style,
    // the ytsurf/yt-x use case) routinely carry stars and hearts from these
    // legacy blocks alongside the newer emoji-supplement range; without
    // this, a title with e.g. U+2B50 (star) measured one column narrower
    // than it renders, desyncing the results/preview separator on that row.
    if (is_extended_pictographic(cp)) {
        return 2;
    }

    // Nerd Font glyphs live in the Private Use Area; the fonts these TUIs
    // assume render them double-width. Treat PUA as wide.
    if ((cp >= 0xe000 && cp <= 0xf8ff) ||       // BMP PUA
        (cp >= 0xf0000 && cp <= 0xffffd) ||     // Plane 15 PUA
        (cp >= 0x100000 && cp <= 0x10fffd)) {   // Plane 16 PUA
        return 2;
    }

    return 1;
}

// Collapses an emoji grapheme cluster to its base's width instead of summing
// each codepoint. Two real-world cases this fixes (found via a live ytsurf
// title: "...Trip🤟🏻 Fukuoka..." and "...ILLIT❤️‍🔥I #Where..."):
//   - Skin-tone modifiers (U+1F3FB-1F3FF) already measure 0 in
//     codepoint_width, so a simple sum gets "🤟🏻" right (2+0=2) -- but a
//     naive per-codepoint sum without this scanner treated the modifier as
//     wide too (see history), so this class is guarded here as well.
//   - ZWJ (U+200D) sequences: "❤️‍🔥" is heart + VS16 + ZWJ + fire, four
//     codepoints. Summing codepoint_width for each gives 1+0+0+2=3, but a
//     terminal renders the whole ZWJ-joined cluster as ONE glyph, 2 columns
//     (the width of its first pictographic member). This scanner tracks
//     "did we just see a ZWJ, and was the codepoint before it a pictograph"
//     and if so suppresses the width of a following pictograph, so the
//     cluster contributes only its base's width regardless of how many
//     ZWJ-joined pictographs follow (family emoji, etc.) -- this is the
//     emoji-adjacent slice of UAX #29's grapheme-cluster rules (GB9/GB9c/
//     GB11), not full grapheme segmentation.
// Regional-indicator flag pairs (e.g. US flag = two RI codepoints) are
// NOT handled here: codepoint_width already counts each RI as 2, and a
// well-formed pair summing to 4 would be wrong (a flag is 2 columns, one
// glyph) -- but that's a pre-existing gap this change doesn't newly
// introduce or worsen, and no real title in the reports so far has hit it.
class GraphemeWidthScanner {
public:
    // Feed one decoded codepoint; returns its incremental contribution to
    // the string's display width (0, or a small delta -- see VS16 below).
    int consume(char32_t cp) {
        if (cp == 0x200d) {  // ZWJ: zero width, arms the "next pictograph
                              // glued to previous" suppression.
            pending_zwj_ = true;
            return 0;
        }

        if (cp == 0xfe0f) {  // VS16: forces EMOJI presentation on the
            // codepoint just emitted, even one that isn't default-emoji
            // (e.g. U+2764 heart is text-presentation, width 1, until VS16
            // follows -- with VS16 it renders as a 2-column emoji glyph).
            // If that base already contributed 2 (already pictographic),
            // there's nothing to add; otherwise emit the +1 delta needed to
            // bring the cluster up to width 2, and mark it pictographic so
            // a following ZWJ join suppresses correctly.
            int delta = 0;
            if (!prev_was_pictographic_ && last_emitted_width_ < 2) {
                delta = 2 - last_emitted_width_;
                last_emitted_width_ = 2;
            }
            prev_was_pictographic_ = true;
            return delta;
        }
        if (cp >= 0xfe00 && cp <= 0xfe0e) {  // other variation selectors: 0.
            return 0;
        }

        bool is_pictographic = is_extended_pictographic(cp);
        int cw = codepoint_width(cp);

        if (pending_zwj_ && prev_was_pictographic_ && is_pictographic) {
            pending_zwj_ = false;
            // Cluster already counted via its first pictographic member.
            return 0;
        }

        pending_zwj_ = false;
        if (cw != 0) {
            prev_was_pictographic_ = is_pictographic;
            last_emitted_width_ = cw;
        }
        return cw;
    }

private:
    bool prev_was_pictographic_ = false;
    bool pending_zwj_ = false;
    int last_emitted_width_ = 0;
};

// Width a '\t' contributes when the visible column immediately before it is
// `col`: spaces up to (but not including) the next tab stop. Matches the
// classic terminal/fzf convention (a tab at the tab stop itself consumes a
// full tabstop's width, not zero).
int tab_width_at(size_t col) {
    int ts = g_tabstop > 0 ? g_tabstop : 8;
    return ts - static_cast<int>(col % static_cast<size_t>(ts));
}

// Display-column width of a UTF-8 string (no ANSI stripping; caller strips SGR
// first if needed), expanding '\t' per the module tab stop measured from
// column 0 (the caller is always a full line/row starting at its own left
// edge -- see visible_width/clip_text_line's usage). Falls back to byte count
// on malformed UTF-8.
size_t utf8_display_width(const std::string& s) {
    size_t w = 0;
    try {
        auto it = s.begin();
        GraphemeWidthScanner scanner;
        while (it != s.end()) {
            char32_t cp = utf8::next(it, s.end());
            if (cp == '\t') {
                w += static_cast<size_t>(tab_width_at(w));
                continue;
            }
            w += static_cast<size_t>(scanner.consume(cp));
        }
    } catch (...) {
        return s.size();
    }
    return w;
}

const char* sgr_fg_code(Color c) {
    switch (c) {
        case Color::Black: return "30";
        case Color::Red: return "31";
        case Color::Green: return "32";
        case Color::Yellow: return "33";
        case Color::Blue: return "34";
        case Color::Magenta: return "35";
        case Color::Cyan: return "36";
        case Color::White: return "37";
        case Color::Default: default: return "";
    }
}

} // namespace

void set_tabstop(int tabstop) {
    g_tabstop = tabstop > 0 ? tabstop : 8;
}

FrameRenderer::FrameRenderer(int rows, int cols) : rows_(rows), cols_(cols) {}

void FrameRenderer::move_to(int row, int col) {
    buffer_ += "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H";
}

void FrameRenderer::move_cursor(int row, int col) {
    move_to(row, col);
}

void FrameRenderer::append_style(const Style& style) {
    buffer_ += "\x1b[0";
    if (style.bold) {
        buffer_ += ";1";
    }
    if (style.inverted) {
        buffer_ += ";7";
    }
    const char* fg = sgr_fg_code(style.fg);
    if (fg[0] != '\0') {
        buffer_ += ";";
        buffer_ += fg;
    }
    buffer_ += "m";
}

void FrameRenderer::append_reset() {
    buffer_ += "\x1b[0m";
}

void FrameRenderer::draw_row(int row, int col, const Row& spans, int max_cols) {
    if (row < 0 || row >= rows_) {
        return;
    }
    int budget = max_cols > 0 ? max_cols : (cols_ - col);
    if (budget <= 0) {
        return;
    }

    move_to(row, col);

    // `written` counts DISPLAY COLUMNS, not codepoints. A CJK/Hangul title or
    // a Nerd Font glyph is two columns wide; counting it as one (the old bug)
    // let a row's true width exceed `budget`, so it auto-wrapped at the screen
    // edge and shoved every row below it down, and left the padding too short
    // to erase the previous, longer frame.
    int written = 0;
    // Shared across spans (not reset per span) so a ZWJ emoji sequence split
    // across a match-highlight span boundary still collapses to one glyph's
    // width instead of being double-counted at the seam.
    GraphemeWidthScanner width_scanner;
    // Set once a span's content doesn't fit the remaining budget (a wide char
    // or an expanded tab straddling the last column). Previously the inner
    // codepoint loop merely `break`'d, which only ended that span -- the outer
    // loop then moved on to the NEXT span and kept drawing, so a row clipped
    // mid-string (e.g. "ab漢|cd" clipped at col 3) deleted the char that didn't
    // fit but then resumed with trailing text ("abc" instead of stopping at
    // "ab"). Once the row is full, no further span may emit anything; the
    // padding below fills the rest exactly as the normal end-of-row case does.
    bool row_done = false;
    for (const auto& span : spans) {
        if (row_done || written >= budget) {
            break;
        }
        std::u32string cps;
        try {
            utf8::utf8to32(span.text.begin(), span.text.end(), std::back_inserter(cps));
        } catch (...) {
            cps.clear();
            for (unsigned char c : span.text) {
                cps.push_back(static_cast<char32_t>(c));
            }
        }

        // Take as many codepoints as fit in the remaining column budget,
        // measuring each by its display width ('\t' expands to spaces up to
        // the next tab stop from the current column). A wide char or tab that
        // would straddle the last remaining column stops emission for the
        // WHOLE row (not just this span) and the rest is left for the padding
        // pass below, rather than dropping one char and letting later spans
        // keep drawing past the clip point.
        std::u32string seg_cps;
        for (char32_t cp : cps) {
            if (cp == '\t') {
                int tw = tab_width_at(static_cast<size_t>(written));
                if (written + tw > budget) { row_done = true; break; }
                seg_cps.append(static_cast<size_t>(tw), U' ');
                written += tw;
                continue;
            }
            int cw = width_scanner.consume(cp);
            if (written + cw > budget) { row_done = true; break; }
            seg_cps.push_back(cp);
            written += cw;
        }
        if (seg_cps.empty()) {
            continue;
        }

        std::string seg_utf8;
        utf8::utf32to8(seg_cps.begin(), seg_cps.end(), std::back_inserter(seg_utf8));

        append_style(span.style);
        buffer_ += seg_utf8;
        append_reset();
    }

    // Pad with spaces to the end of the row's budget so a shorter frame
    // doesn't leave stale characters from a longer previous one. Padding
    // (rather than \x1b[K, which clears to the physical end of line) keeps
    // this safe when a border or preview pane occupies the columns beyond
    // this row's budget.
    if (written < budget) {
        buffer_ += std::string(static_cast<size_t>(budget - written), ' ');
    }
}

void FrameRenderer::draw_text(int row, int col, const std::string& text, Style style, int max_cols) {
    Row spans{Span{text, style}};
    draw_row(row, col, spans, max_cols);
}

void FrameRenderer::draw_separator(int row, int col_start, int width) {
    if (row < 0 || row >= rows_) {
        return;
    }
    // Default (width <= 0, matching draw_row's max_cols convention): span the
    // whole terminal width from column 0, as before.
    int start = col_start > 0 ? col_start : 0;
    int span = width > 0 ? width : (cols_ - start);
    if (span <= 0) {
        return;
    }
    move_to(row, start);
    for (int i = 0; i < span; ++i) {
        // U+2500 BOX DRAWINGS LIGHT HORIZONTAL, encoded as UTF-8.
        buffer_ += "\xE2\x94\x80";
    }
    // EL (erase to end of line) is only safe when the separator legitimately
    // owns the rest of the physical line (the full-width default). With a
    // bounded width -- i.e. the caller is keeping the separator inside
    // --border verticals -- EL would erase the border's right-hand bar (and
    // anything else past `start + span`), so it's skipped; the drawn run of
    // box-drawing chars is the only output for a bounded separator.
    if (col_start <= 0 && width <= 0) {
        buffer_ += "\x1b[K";
    }
}

void FrameRenderer::draw_border() {
    if (rows_ < 2 || cols_ < 2) {
        return;
    }
    // Corners + horizontal edges.
    move_to(0, 0);
    buffer_ += "\xE2\x94\x8C";  // top-left
    for (int i = 1; i < cols_ - 1; ++i) buffer_ += "\xE2\x94\x80";
    buffer_ += "\xE2\x94\x90";  // top-right

    move_to(rows_ - 1, 0);
    buffer_ += "\xE2\x94\x94";  // bottom-left
    for (int i = 1; i < cols_ - 1; ++i) buffer_ += "\xE2\x94\x80";
    buffer_ += "\xE2\x94\x98";  // bottom-right

    // Vertical edges.
    for (int r = 1; r < rows_ - 1; ++r) {
        move_to(r, 0);
        buffer_ += "\xE2\x94\x82";  // vertical bar
        move_to(r, cols_ - 1);
        buffer_ += "\xE2\x94\x82";
    }
}

void FrameRenderer::clear_region(int row, int col, int height, int width) {
    std::string blank(static_cast<size_t>(std::max(0, width)), ' ');
    for (int r = row; r < row + height && r < rows_; ++r) {
        if (r < 0) continue;
        move_to(r, col);
        // Reset SGR before the blank fill: a preview row (e.g. chafa block-mode
        // output) can end mid-color with no trailing reset, so without this the
        // "blank" spaces inherit that background color instead of truly
        // clearing it -- visible as a stray color bleed when scrolling to a
        // new preview clears the old one.
        buffer_ += "\x1b[0m";
        buffer_ += blank;
    }
}

void write_raw_passthrough(int fd, int row, int col, const std::string& raw_bytes) {
    std::string out;
    out += "\x1b" "7";  // DECSC save cursor
    out += "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H";
    out += raw_bytes;
    out += "\x1b" "8";  // DECRC restore cursor
    write_all(fd, out);
}

namespace {

// Classify one escape sequence that begins at `line[start]` (== ESC). Sets
// `*seg_end` to one past the sequence, `*kept` to whether it should be forwarded
// to the terminal (vs dropped), and `*seq_terminated` to whether the sequence
// actually reached its own terminator within [start,n) (false = the buffer was
// cut mid-sequence by a streaming chunk boundary). Never leaves seg_end==start
// (always makes progress) so a caller's scan can't stall.
//
// Used two ways: (1) to drop chafa's terminal PROBES that are SPLICED into a
// sixel raster (query OSCs whose body ends '?', capability CSIs like ESC[18t /
// ESC[0c) while KEEPING a real image OSC (ESC]1337;File=…, no '?'); (2) to tell
// whether a spliced sub-sequence at the tail is complete, so a partially
// streamed image is correctly held back rather than painted truncated.
//
// Is `content[j]` (== 0x9c) a genuine 8-bit ST, or the trailing continuation
// byte of a multibyte UTF-8 codepoint (e.g. Ü = C3 9C)? This is a UTF-8 world:
// 0x9c only ever legitimately terminates a string sequence when the byte
// immediately before it is itself a single-byte ASCII byte (< 0x80) — a
// continuation byte can only follow a UTF-8 lead byte (>= 0xC0) or another
// continuation byte, both of which are >= 0x80. Checking just the immediately
// preceding byte is sufficient (and is what every scan site below already has
// on hand): if it's ASCII, 0x9c cannot be mid-sequence in THIS string, because
// a lead byte would have to sit between them. Applied identically at every
// 0x9c check in this file (classify_escape_seq, dcs_seq_end,
// sanitize_preview_line's DCS/OSC loops, string_seq_len) so the splitter and
// the sanitizer can never disagree about where a sequence ends.
bool is_real_st(const std::string& content, size_t j) {
    if (j == 0) return true;
    return static_cast<unsigned char>(content[j - 1]) < 0x80;
}

void classify_escape_seq(const std::string& line, size_t start, size_t n,
                         size_t* seg_end, bool* kept, bool* seq_terminated) {
    unsigned char intro = start + 1 < n
                              ? static_cast<unsigned char>(line[start + 1])
                              : 0;
    if (intro == '[') {
        // CSI: ESC [ params/intermediates final(0x40-0x7E). A cursor/capability
        // control here (never image data) — drop it.
        size_t j = start + 2;
        bool term = false;
        while (j < n) {
            unsigned char b = static_cast<unsigned char>(line[j]);
            if (b >= 0x40 && b <= 0x7E) { j++; term = true; break; }
            j++;
        }
        *seg_end = j;
        *kept = false;
        *seq_terminated = term;
        return;
    }
    if (intro == ']' || intro == 'P' || intro == '_' || intro == '^' ||
        intro == 'X') {
        // String sequence (OSC / DCS / APC / PM / SOS). Scan to its own ST/BEL.
        size_t body_start = start + 2;
        size_t j = body_start;
        size_t term_start = n;
        bool term = false;
        while (j < n) {
            unsigned char b = static_cast<unsigned char>(line[j]);
            if (b == 0x07 || (b == 0x9c && is_real_st(line, j))) {
                term_start = j; j++; term = true; break;
            }
            if (b == '\x1b' && j + 1 < n &&
                static_cast<unsigned char>(line[j + 1]) == '\\') {
                term_start = j; j += 2; term = true; break;
            }
            j++;
        }
        *seg_end = j;
        *seq_terminated = term;
        // Keep only a real image OSC: an OSC whose body does NOT end in '?'.
        // Query OSCs (…?ST) are probes — drop. Non-OSC string seqs spliced
        // mid-raster aren't trustworthy image data — drop them too.
        bool is_osc = (intro == ']');
        bool is_query = term_start > body_start &&
                        static_cast<unsigned char>(line[term_start - 1]) == '?';
        *kept = is_osc && !is_query;
        return;
    }
    // Lone two-byte escape, or a trailing lone ESC (chunk cut before we even
    // know the introducer). Drop it; treat a bare trailing ESC as unterminated.
    if (start + 1 >= n) {
        *seg_end = n;
        *kept = false;
        *seq_terminated = false;
        return;
    }
    *seg_end = std::min(start + 2, n);
    *kept = false;
    *seq_terminated = true;
}

// Thin wrapper kept for sanitize_preview_line's DCS loop: does this spliced
// sub-sequence get forwarded?
bool spliced_sequence_kept(const std::string& line, size_t start, size_t n,
                           size_t& seg_end) {
    bool kept = false, term = false;
    classify_escape_seq(line, start, n, &seg_end, &kept, &term);
    return kept;
}

// Find the end of a DCS/APC/PM/SOS payload (sixel / kitty graphics) beginning at
// `content[start]` (ESC + introducer P/_/^/X). A real DCS payload's ONLY raw ESC
// is its terminating ST (ESC \); but chafa SPLICES terminal-probe sequences
// (ESC]10;? with its own ESC\, ESC[18t, …) into the raster mid-stream. A spliced
// probe's ESC\ must NOT be mistaken for the DCS terminator — that early stop is
// what let a still-loading (truncated) image get painted as garbage. So we skip
// each spliced sub-sequence whole and keep scanning for the DCS's OWN ST/BEL.
//
// Returns the index one past the DCS terminator, and sets *terminated. If the
// buffer ends before a real terminator — image still streaming, OR a spliced
// sub-sequence at the tail is itself cut — returns the remaining length with
// *terminated=false so the caller holds the blob back.
size_t dcs_seq_end(const std::string& content, size_t start, bool* terminated) {
    const size_t n = content.size();
    size_t j = start + 2;  // skip ESC + introducer
    while (j < n) {
        unsigned char b = static_cast<unsigned char>(content[j]);
        if (b == 0x07 || (b == 0x9c && is_real_st(content, j))) {
            // BEL / genuine 8-bit ST: real DCS terminator. (A 0x9c that's a
            // UTF-8 continuation byte of a multibyte codepoint in the raster
            // falls through and is treated as ordinary payload data below.)
            if (terminated) *terminated = true;
            return j - start + 1;
        }
        if (b == '\x1b') {
            if (j + 1 < n &&
                static_cast<unsigned char>(content[j + 1]) == '\\') {
                if (terminated) *terminated = true;
                return j - start + 2;  // 7-bit ST: ESC \ — real DCS terminator
            }
            // A spliced sub-sequence (or a trailing lone ESC). Skip it whole; if
            // IT is unterminated (tail of a streaming chunk), the whole DCS is
            // unterminated — hold it back.
            size_t seg_end;
            bool kept, sub_term;
            classify_escape_seq(content, j, n, &seg_end, &kept, &sub_term);
            if (!sub_term) {
                if (terminated) *terminated = false;
                return n - start;
            }
            j = seg_end;
            continue;
        }
        j++;
    }
    if (terminated) *terminated = false;
    return n - start;  // unterminated — image still streaming
}

} // namespace

std::string sanitize_preview_line(const std::string& line) {
    std::string out;
    out.reserve(line.size());

    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(line[i]);

        if (c == '\x1b' && i + 1 < n) {
            unsigned char next = static_cast<unsigned char>(line[i + 1]);

            if (next == '[') {
                // CSI: ESC [ <params/intermediates> <final 0x40-0x7E>.
                size_t j = i + 2;
                while (j < n) {
                    unsigned char b = static_cast<unsigned char>(line[j]);
                    if (b >= 0x40 && b <= 0x7E) break;  // final byte
                    j++;
                }
                if (j < n) {
                    unsigned char fin = static_cast<unsigned char>(line[j]);
                    // Keep only SGR (color/attributes); it cannot move the
                    // cursor or erase. Everything else — cursor positioning
                    // (H f d G A-F), erase display/line (J K), save/restore
                    // (s u), scroll (S T) — is dropped so it can't reach
                    // outside the pane.
                    if (fin == 'm') {
                        out.append(line, i, j - i + 1);
                    }
                    i = j + 1;
                    continue;
                }
                // Unterminated CSI: drop the rest of the line.
                break;
            }

            if (next == 'P' || next == '_' || next == '^' || next == 'X') {
                // DCS / APC / PM / SOS — carries sixel and kitty-graphics
                // payloads (the whole point of the direct-terminal backend).
                // Pass the intro + data bytes through verbatim up to ST
                // (ESC \ / 0x9c) or BEL. Data bytes are otherwise opaque: an
                // 'H'/'J' inside a sixel payload is data, not a cursor move.
                //
                // ONE exception. A real sixel/DCS payload never contains a
                // raw ESC except the terminating ST — but chafa splices its
                // color/size PROBES (ESC]10;? ESC]11;? ESC[18t ESC[0c …) into
                // the MIDDLE of the raster it streams. If we treated the first
                // ESC as the DCS terminator we'd (a) stop the sixel early,
                // forwarding the probe's own ESC\ ST as if it closed the DCS,
                // and (b) forward the probe itself — the terminal then replies
                // onto fzf's stdin and mlterm spews garbage (the reported
                // ytsurf bug). So while scanning the DCS payload, when we meet
                // an ESC that ISN'T the ST (ESC\), skip that whole spliced
                // sequence in place — dropping probe OSC/CSI, keeping any real
                // image OSC — and keep the surrounding sixel data contiguous.
                out.push_back('\x1b');
                out.push_back(static_cast<char>(next));
                size_t j = i + 2;
                while (j < n) {
                    unsigned char b = static_cast<unsigned char>(line[j]);
                    if (b == 0x07 || (b == 0x9c && is_real_st(line, j))) {
                        // BEL / genuine 8-bit ST: end DCS. A 0x9c that's a
                        // UTF-8 continuation byte (e.g. Ü = C3 9C inside the
                        // raster/payload) falls through as ordinary data.
                        out.push_back(static_cast<char>(b));
                        j++;
                        break;
                    }
                    if (b == '\x1b') {
                        if (j + 1 < n &&
                            static_cast<unsigned char>(line[j + 1]) == '\\') {
                            out.append("\x1b\\", 2);  // 7-bit ST: end DCS
                            j += 2;
                            break;
                        }
                        // A spliced escape sequence inside the raster. Consume
                        // it without ending the DCS: append it only if it's a
                        // real image OSC (not a terminal-query probe), then
                        // resume streaming sixel data.
                        size_t seg_end;
                        bool keep = spliced_sequence_kept(line, j, n, seg_end);
                        if (keep) out.append(line, j, seg_end - j);
                        j = seg_end;
                        continue;
                    }
                    out.push_back(static_cast<char>(b));
                    j++;
                }
                i = j;
                continue;
            }

            if (next == ']') {
                // OSC. Scan to ST (ESC \ / 0x9c) or BEL.
                size_t body_start = i + 2;
                size_t j = body_start;
                size_t term_start = n;  // index of the terminator (ST/BEL)
                while (j < n) {
                    unsigned char b = static_cast<unsigned char>(line[j]);
                    if (b == 0x07 || (b == 0x9c && is_real_st(line, j))) {
                        // BEL or genuine 8-bit ST (not a UTF-8 continuation
                        // byte, e.g. Ü = C3 9C inside an OSC 8 hyperlink URI).
                        term_start = j;
                        j++;
                        break;
                    }
                    if (b == '\x1b' && j + 1 < n &&
                        static_cast<unsigned char>(line[j + 1]) == '\\') {
                        term_start = j;
                        j += 2;  // 7-bit ST: ESC \.
                        break;
                    }
                    j++;
                }

                // DROP terminal-QUERY OSCs — a preview tool (chafa) probes the
                // terminal for its background/foreground color (ESC]10;? /
                // ESC]11;?) and palette (ESC]4;N;?) to pick an output format.
                // With chafa's stdout captured by our popen pipe, those probes
                // land in the preview text; forwarding them makes the terminal
                // send REPLIES onto fzf's stdin, corrupting the query/keys. A
                // query is an OSC whose body ends in '?' right before the
                // terminator. Image OSCs (ESC]1337;File=…) never do, so they
                // still pass through.
                bool is_query = term_start > body_start &&
                                static_cast<unsigned char>(line[term_start - 1]) == '?';
                if (!is_query) {
                    out.append(line, i, j - i);
                }
                i = j;
                continue;
            }

            // Standalone two-byte escapes: RIS (ESC c), index/next-line
            // (ESC D/E/M), keypad modes, ESC H (home in some terminals), etc.
            // None are needed inside a preview and several move the cursor or
            // reset the terminal — drop the escape and its single trailing
            // byte.
            i += 2;
            continue;
        }

        if (c == '\x1b') {
            // Lone trailing ESC with nothing after it.
            break;
        }

        // Bare carriage return / backspace would reset the column and let
        // subsequent bytes overwrite the results pane; drop them. Tabs are
        // kept (they only advance rightward within the line).
        if (c == '\r' || c == '\b') {
            i++;
            continue;
        }

        out.push_back(line[i]);
        i++;
    }

    return out;
}

namespace {

// Length of a string-terminated escape sequence (OSC/DCS/APC/PM/SOS) starting
// at `content[start]` (which must be ESC). Returns the count of bytes through
// the terminator (BEL, 8-bit ST 0x9c, or 7-bit ST "ESC \"). Sets `*terminated`
// to whether a terminator was actually seen; if not (the sequence runs to the
// end of `content`), returns the remaining length and `*terminated = false`.
// The payload may legitimately contain newlines (iTerm OSC 1337 images, kitty
// APC, sixel) — those are data, not line breaks.
//
// The terminated flag matters because the preview is captured in streaming
// chunks: a partial read can end in the MIDDLE of a multi-kilobyte image blob.
// Emitting that partial (unterminated) sequence to the real terminal is what
// makes iTerm2 pop its "Allow Terminal-Initiated Display?" dialog and makes
// sixel terminals (mlterm) spew the raw payload as garbage. The caller holds
// an unterminated trailing blob back until a later repaint carries its ST/BEL.
size_t string_seq_len(const std::string& content, size_t start, bool* terminated) {
    const size_t n = content.size();
    unsigned char intro = start + 1 < n
                              ? static_cast<unsigned char>(content[start + 1])
                              : 0;
    // DCS/APC/PM/SOS (sixel, kitty graphics) can have chafa's terminal probes
    // SPLICED into the raster; a probe carries its own ESC\ that must not be
    // taken as the DCS terminator, or a still-streaming (truncated) image gets
    // mis-flagged "terminated" and painted as garbage. dcs_seq_end skips spliced
    // sub-sequences and reports termination on the DCS's OWN ST only.
    if (intro == 'P' || intro == '_' || intro == '^' || intro == 'X') {
        return dcs_seq_end(content, start, terminated);
    }
    // OSC (ESC ]) — iTerm2 1337 images and the like. A single ST-terminated
    // sequence; scan straight to its ST/BEL.
    size_t j = start + 2;  // skip ESC + introducer
    while (j < n) {
        unsigned char b = static_cast<unsigned char>(content[j]);
        if (b == 0x07 || (b == 0x9c && is_real_st(content, j))) {
            // BEL / genuine 8-bit ST (see is_real_st: 0x9c preceded by a
            // UTF-8 lead/continuation byte is payload, e.g. Ü = C3 9C in an
            // OSC 8 hyperlink URI, not a terminator).
            if (terminated) *terminated = true;
            return j - start + 1;
        }
        if (b == '\x1b' && j + 1 < n &&
            static_cast<unsigned char>(content[j + 1]) == '\\') {
            if (terminated) *terminated = true;
            return j - start + 2;  // 7-bit ST: ESC \.
        }
        j++;
    }
    if (terminated) *terminated = false;
    return n - start;  // unterminated
}

bool is_string_introducer(char c) {
    return c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X';
}

// Is the string sequence starting at `row[k]` (== ESC) a genuine graphics
// blob that must bypass column clipping -- DCS (sixel, ESC P), APC (kitty
// graphics, ESC _), or specifically an OSC 1337 (iTerm2 inline image, "ESC ]
// 1337 ; File=..."), as opposed to a plain OSC that happens to share the same
// introducer (OSC 8 hyperlinks, OSC 0/1/2 window/tab title)? Those plain OSCs
// carry ordinary text around them and must still go through the normal
// ANSI-aware clip (escape sequences preserved, visible columns budgeted) --
// only real image data has "width that isn't column-measurable". Exempting
// every OSC (the previous behavior) let e.g. `ls --hyperlink` preview output
// skip clipping entirely and wrap into the results pane.
bool is_graphics_blob_start(const std::string& row, size_t k) {
    if (k + 1 >= row.size() || row[k] != '\x1b') return false;
    char intro = row[k + 1];
    if (intro == 'P' || intro == '_') return true;
    if (intro != ']') return false;
    // OSC: only 1337 (iTerm2 file/image protocol) counts as a graphics blob.
    static const std::string kMarker = "1337;";
    size_t body = k + 2;
    if (body > row.size()) return false;  // compare() throws if pos > size()
    return row.compare(body, kMarker.size(), kMarker) == 0;
}

// Truncate an already-sanitized text line (SGR only, no cursor/erase escapes,
// no string sequences) to `max_cols` visible columns.
std::string clip_text_line(const std::string& line, int max_cols) {
    if (max_cols <= 0) return line;
    if (visible_width(line) <= static_cast<size_t>(max_cols)) return line;
    return truncate_ansi_text(line, static_cast<size_t>(max_cols));
}

} // namespace

void write_preview_content(int fd, int top, int left,
                           const std::string& raw_content,
                           size_t scroll, int max_lines, int max_cols,
                           size_t& out_total_lines) {
    // First pass: split into "logical lines" for scroll/line-count bookkeeping,
    // but treat a string sequence (image blob) as belonging to the line it
    // starts on so an embedded newline never inflates the line count or gets
    // treated as a row break. Each element is the raw bytes of one screen row's
    // worth of content (text + any SGR + at most the image blob that begins on
    // it). We DON'T sanitize here — sanitation happens per-row below.
    std::vector<std::string> rows;
    {
        std::string cur;
        size_t i = 0;
        const size_t n = raw_content.size();
        while (i < n) {
            char c = raw_content[i];
            if (c == '\x1b' && i + 1 < n &&
                is_string_introducer(raw_content[i + 1])) {
                bool terminated = true;
                size_t len = string_seq_len(raw_content, i, &terminated);
                if (!terminated) {
                    // A string sequence with no terminator can only be a blob
                    // truncated by a mid-stream chunk read (see string_seq_len).
                    // Painting it now would send an incomplete OSC/DCS/APC to
                    // the terminal — iTerm2's "Allow Display?" dialog, sixel
                    // garbage in mlterm. Drop everything from here to end of
                    // buffer; the next repaint (with more bytes, or the final
                    // complete capture) will carry the terminator and paint it
                    // whole. Anything already in `cur` stays as its own row.
                    break;
                }
                cur.append(raw_content, i, len);  // whole blob, newlines and all
                i += len;
                continue;
            }
            if (c == '\x1b' && i + 1 == n) {
                // Lone ESC as the last byte: a chunk boundary landed mid-escape
                // (before we even know the introducer). Drop it so it can't
                // swallow the first byte of the next repaint; the next capture
                // carries the full sequence.
                break;
            }
            if (c == '\n') {
                rows.push_back(cur);
                cur.clear();
                i++;
                continue;
            }
            cur.push_back(c);
            i++;
        }
        if (!cur.empty()) rows.push_back(cur);
    }

    out_total_lines = rows.size();
    if (!rows.empty() && scroll >= rows.size()) scroll = rows.size() - 1;

    std::string out;
    out += "\x1b" "7";  // DECSC save cursor

    int drawn = 0;
    for (size_t r = scroll; r < rows.size() && drawn < max_lines; ++r, ++drawn) {
        // Position at this pane row's left column — never a bare CR/LF, which
        // would return to physical column 0 and bleed into the results pane.
        out += "\x1b[" + std::to_string(top + drawn + 1) + ";" +
               std::to_string(left + 1) + "H";

        const std::string& row = rows[r];
        // Every row goes through sanitize_preview_line: it passes real image
        // blobs (sixel DCS, kitty APC, iTerm2 OSC 1337) through verbatim so the
        // raster reaches the terminal intact, but strips the junk a preview
        // tool interleaves around them — CSI cursor-moves/erases, and OSC
        // COLOR-QUERY probes (ESC]10;? / ESC]11;?) that chafa emits to detect
        // the terminal. Forwarding those probes made the terminal reply onto
        // fzf's stdin, corrupting the query/keys and the display (the ytsurf
        // sixel garbage). Only a row with a genuine graphics blob (DCS/APC/OSC
        // 1337) skips column clipping (its width isn't column-measurable and
        // clipping could cut the raster); a plain OSC (hyperlink, title) still
        // carries ordinary column-measurable text and goes through the normal
        // clip like any other row -- exempting every OSC let hyperlinked
        // preview text (`ls --hyperlink`) skip clipping and wrap into the
        // results pane.
        bool has_blob = false;
        for (size_t k = 0; k + 1 < row.size(); ++k) {
            if (is_graphics_blob_start(row, k)) {
                has_blob = true;
                break;
            }
        }
        std::string clean = sanitize_preview_line(row);
        out += has_blob ? clean : clip_text_line(clean, max_cols);
    }

    out += "\x1b" "8";  // DECRC restore cursor
    write_all(fd, out);
}

size_t visible_width(const std::string& utf8_text) {
    // Display columns, not codepoints: a CJK/Hangul char or Nerd Font glyph is
    // two columns. Used for prompt-cursor placement and overflow/clip checks.
    return utf8_display_width(strip_ansi_codes(utf8_text));
}

std::string truncate_ansi_text(const std::string& text, size_t max_cols) {
    if (max_cols == 0) {
        return "";
    }

    std::string result;
    result.reserve(text.size());
    size_t visible_count = 0;
    bool any_sgr = false;
    size_t i = 0;
    GraphemeWidthScanner width_scanner;

    while (i < text.size() && visible_count < max_cols) {
        if (text[i] == '\x1b') {
            size_t start = i;
            size_t j = i + 1;
            while (j < text.size() &&
                   !((text[j] >= 'A' && text[j] <= 'Z') || (text[j] >= 'a' && text[j] <= 'z'))) {
                j++;
            }
            if (j < text.size()) {
                j++;  // include the terminating letter
            }
            result.append(text, start, j - start);
            any_sgr = true;
            i = j;
            continue;
        }

        if (text[i] == '\t') {
            // Expand to spaces up to the next tab stop from the current
            // visible column; stop (without emitting a partial tab) if even
            // the first expanded space would straddle max_cols.
            int tw = tab_width_at(visible_count);
            if (visible_count + static_cast<size_t>(tw) > max_cols) {
                break;
            }
            result.append(static_cast<size_t>(tw), ' ');
            i += 1;
            visible_count += static_cast<size_t>(tw);
            continue;
        }

        size_t char_len = utf8_char_length(text[i]);
        char_len = std::min(char_len, text.size() - i);
        // Advance the visible-column counter by the char's DISPLAY width, and
        // stop if a wide char would exceed max_cols (don't emit a char that
        // straddles the boundary).
        int cw = 1;
        try {
            auto it = text.begin() + static_cast<long>(i);
            char32_t cp = utf8::next(it, text.end());
            cw = width_scanner.consume(cp);
        } catch (...) {
            cw = 1;
        }
        if (visible_count + static_cast<size_t>(cw) > max_cols) {
            break;
        }
        result.append(text, i, char_len);
        i += char_len;
        visible_count += static_cast<size_t>(cw);
    }

    if (any_sgr) {
        result += "\x1b[0m";
    }
    return result;
}

} // namespace fzf
