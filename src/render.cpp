#include "render.hpp"
#include "util.hpp"

#include <algorithm>
#include <unistd.h>
#include <utf8.h>

namespace fzf {

namespace {

// Display-column width of a single Unicode codepoint, à la wcwidth(3). fzf++
// consumers show CJK/Hangul titles (each 2 columns), Nerd Font glyphs in the
// Private Use Area (2 columns in the fonts these TUIs assume), and combining
// marks (0). Counting every codepoint as 1 column — as the old code did — made
// a wide-char row's true width exceed the results pane, so it auto-wrapped at
// the terminal's right edge and pushed every row below it down (the "jumping"),
// and made per-row padding too short to erase the previous frame (stale text /
// superimposed lists). This table covers the ranges those consumers actually
// hit; it is deliberately compact, not a full Unicode width database.
int codepoint_width(char32_t cp) {
    if (cp == 0) return 0;
    // C0/C1 controls: not printable, treat as zero so they don't shift columns.
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;

    // Zero-width: combining marks, ZWJ/ZWNJ, variation selectors, BOM.
    if ((cp >= 0x0300 && cp <= 0x036f) ||   // combining diacritical marks
        (cp >= 0x200b && cp <= 0x200f) ||   // ZWSP..RLM
        (cp >= 0xfe00 && cp <= 0xfe0f) ||   // variation selectors
        cp == 0xfeff) {                     // BOM / ZWNBSP
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
        (cp >= 0x1f300 && cp <= 0x1faff) || // emoji & pictographs
        (cp >= 0x20000 && cp <= 0x3fffd)) { // CJK Ext B+ (SIP)
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

// Display-column width of a UTF-8 string (no ANSI stripping; caller strips SGR
// first if needed). Falls back to byte count on malformed UTF-8.
size_t utf8_display_width(const std::string& s) {
    size_t w = 0;
    try {
        auto it = s.begin();
        while (it != s.end()) {
            char32_t cp = utf8::next(it, s.end());
            w += static_cast<size_t>(codepoint_width(cp));
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

void write_all(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = write(fd, data.data() + written, data.size() - written);
        if (n <= 0) {
            return;
        }
        written += static_cast<size_t>(n);
    }
}

} // namespace

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
    for (const auto& span : spans) {
        if (written >= budget) {
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
        // measuring each by its display width. A wide char that would straddle
        // the last remaining column is dropped (and the column left blank via
        // padding below) rather than emitted half-off the pane.
        std::u32string seg_cps;
        for (char32_t cp : cps) {
            int cw = codepoint_width(cp);
            if (written + cw > budget) break;
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

void FrameRenderer::draw_separator(int row) {
    if (row < 0 || row >= rows_) {
        return;
    }
    move_to(row, 0);
    for (int i = 0; i < cols_; ++i) {
        // U+2500 BOX DRAWINGS LIGHT HORIZONTAL, encoded as UTF-8.
        buffer_ += "\xE2\x94\x80";
    }
    buffer_ += "\x1b[K";
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

            if (next == ']' || next == 'P' || next == '_' ||
                next == '^' || next == 'X') {
                // String-terminated sequence: OSC / DCS / APC / PM / SOS.
                // These carry sixel and kitty-graphics payloads (the whole
                // point of the direct-terminal backend), so pass the entire
                // sequence — including its data bytes — through verbatim, up
                // to ST (ESC \ or 0x9c) or BEL. Data bytes are not scanned
                // for escapes, so an 'H'/'J' inside a sixel payload is safe.
                size_t body_start = i + 2;
                size_t j = body_start;
                size_t term_start = n;  // index of the terminator (ST/BEL)
                while (j < n) {
                    unsigned char b = static_cast<unsigned char>(line[j]);
                    if (b == 0x07 || b == 0x9c) {  // BEL or 8-bit ST
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
                // still pass through. DCS/APC/PM/SOS (sixel, kitty) are not
                // color queries and pass through unchanged.
                bool is_osc = (next == ']');
                bool is_query = is_osc && term_start > body_start &&
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
    size_t j = start + 2;  // skip ESC + introducer
    const size_t n = content.size();
    while (j < n) {
        unsigned char b = static_cast<unsigned char>(content[j]);
        if (b == 0x07 || b == 0x9c) {  // BEL / 8-bit ST
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
        // sixel garbage). Only a blob-free row is column-clipped; a row that
        // carries an image blob isn't (its width isn't column-measurable and
        // clipping could cut the raster).
        bool has_blob = false;
        for (size_t k = 0; k + 1 < row.size(); ++k) {
            if (row[k] == '\x1b' && is_string_introducer(row[k + 1])) {
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

        size_t char_len = utf8_char_length(text[i]);
        char_len = std::min(char_len, text.size() - i);
        // Advance the visible-column counter by the char's DISPLAY width, and
        // stop if a wide char would exceed max_cols (don't emit a char that
        // straddles the boundary).
        int cw = 1;
        try {
            auto it = text.begin() + static_cast<long>(i);
            char32_t cp = utf8::next(it, text.end());
            cw = codepoint_width(cp);
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
