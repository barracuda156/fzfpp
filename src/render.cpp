#include "render.hpp"
#include "util.hpp"

#include <algorithm>
#include <unistd.h>
#include <utf8.h>

namespace fzf {

namespace {

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

        int available = budget - written;
        size_t take = std::min(cps.size(), static_cast<size_t>(available));
        if (take == 0) {
            continue;
        }

        std::string seg_utf8;
        utf8::utf32to8(cps.begin(), cps.begin() + static_cast<long>(take),
                        std::back_inserter(seg_utf8));

        append_style(span.style);
        buffer_ += seg_utf8;
        append_reset();

        written += static_cast<int>(take);
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
                size_t j = i + 2;
                while (j < n) {
                    unsigned char b = static_cast<unsigned char>(line[j]);
                    if (b == 0x07 || b == 0x9c) {  // BEL or 8-bit ST
                        j++;
                        break;
                    }
                    if (b == '\x1b' && j + 1 < n &&
                        static_cast<unsigned char>(line[j + 1]) == '\\') {
                        j += 2;  // 7-bit ST: ESC \.
                        break;
                    }
                    j++;
                }
                out.append(line, i, j - i);
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
// A line that carries a DCS/APC/OSC/PM/SOS string sequence (sixel, kitty
// graphics, terminal queries) can't be measured or truncated by visible
// column — its payload bytes aren't display columns and cutting mid-payload
// would corrupt it. Detect these and pass such lines through unclipped.
bool has_string_sequence(const std::string& s) {
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) == 0x1b) {
            char n = s[i + 1];
            if (n == ']' || n == 'P' || n == '_' || n == '^' || n == 'X') {
                return true;
            }
        }
    }
    return false;
}
} // namespace

void write_preview_lines(int fd, int top, int left,
                         const std::vector<std::string>& lines,
                         int max_lines, int max_cols) {
    std::string out;
    out += "\x1b" "7";  // DECSC save cursor

    int drawn = 0;
    for (const auto& line : lines) {
        if (drawn >= max_lines) break;
        // Position this line at the pane's left column on its own row —
        // never rely on CR/LF, which would return to column 0 and let the
        // line bleed into the results pane.
        out += "\x1b[" + std::to_string(top + drawn + 1) + ";" +
               std::to_string(left + 1) + "H";

        std::string clean = sanitize_preview_line(line);
        if (max_cols > 0 && !has_string_sequence(clean) &&
            visible_width(clean) > static_cast<size_t>(max_cols)) {
            clean = truncate_ansi_text(clean, static_cast<size_t>(max_cols));
        }
        out += clean;
        drawn++;
    }

    out += "\x1b" "8";  // DECRC restore cursor
    write_all(fd, out);
}

size_t visible_width(const std::string& utf8_text) {
    std::string stripped = strip_ansi_codes(utf8_text);
    try {
        return utf8::distance(stripped.begin(), stripped.end());
    } catch (...) {
        return stripped.size();
    }
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
        result.append(text, i, char_len);
        i += char_len;
        visible_count++;
    }

    if (any_sgr) {
        result += "\x1b[0m";
    }
    return result;
}

} // namespace fzf
