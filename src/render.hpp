#pragma once

#include <string>
#include <vector>

namespace fzf {

// Foreground/background color a styled span can carry. Mirrors the small
// subset of SGR color space fzf's own chrome (match highlighting, cursor
// row, header) actually uses — this is NOT a general ANSI parser (that job
// moved to raw passthrough for preview content, see write_raw_passthrough).
enum class Color {
    Default,
    Black, Red, Green, Yellow, Blue, Magenta, Cyan, White,
};

struct Style {
    Color fg = Color::Default;
    bool bold = false;
    bool inverted = false;
};

// One piece of styled text within a row, e.g. one match-highlighted run.
struct Span {
    std::string text;  // UTF-8 bytes, no embedded ANSI
    Style style;
};

using Row = std::vector<Span>;

// Direct-terminal-output layer replacing FTXUI's DOM/Elements. Not a
// retained-mode tree: callers build up one frame's worth of "chrome" rows
// (info/header/results/prompt/border) via the FrameRenderer, which
// accumulates plain bytes and writes them in one buffered write() call.
// The preview pane is handled separately (write_raw_passthrough) since its
// content is opaque bytes from an external command, not styled Spans.
class FrameRenderer {
public:
    // rows/cols: the terminal dimensions this frame is being drawn for.
    FrameRenderer(int rows, int cols);

    // Write one row of styled spans at screen row `row` (0-based), starting
    // at column `col` (0-based). Clears to end of line after the content so
    // stale characters from a previous, longer frame don't linger. Content
    // is truncated (not wrapped) to fit within `max_cols` if given (0 means
    // "to the end of the terminal width").
    void draw_row(int row, int col, const Row& spans, int max_cols = 0);

    // Convenience for a single-style full-width row.
    void draw_text(int row, int col, const std::string& text, Style style = {}, int max_cols = 0);

    // Draws a horizontal line of box-drawing characters (or '-' if
    // unsupported) spanning the full terminal width at `row`.
    void draw_separator(int row);

    // Draws a single-line box border around the full frame (rows/cols given
    // to the constructor). Must be called after all interior content is
    // drawn, since callers are expected to have reserved a 1-cell margin on
    // all sides when border is enabled (matching the existing
    // calculate_preview_position/layout math in terminal.cpp).
    void draw_border();

    // Clears the region [row, row+height) x [col, col+width) — used before
    // repainting the preview pane at a new size, so a smaller image doesn't
    // leave stale content around its edges.
    void clear_region(int row, int col, int height, int width);

    // Move the real terminal cursor (as opposed to drawing into the frame
    // buffer) to (row, col), 0-based. Used for the final query-line cursor
    // position after a repaint.
    void move_cursor(int row, int col);

    // Finish the frame: returns the accumulated bytes for one buffered
    // write() to the terminal. Does NOT include a cursor-hide/show toggle;
    // callers wrap the whole repaint in hide/show if desired to avoid
    // cursor flicker.
    const std::string& bytes() const { return buffer_; }

private:
    void move_to(int row, int col);
    void append_style(const Style& style);
    void append_reset();

    int rows_;
    int cols_;
    std::string buffer_;
};

// Write an external command's raw captured output verbatim at (row, col),
// wrapped in cursor save/restore (\x1b7...\x1b8) so the terminal's own
// cursor-after-image behavior (inconsistent across terminals for sixel/
// kitty-graphics) never needs to be predicted or corrected — matches fzf's
// own LightRenderer.PassThrough. This is a separate direct write() (not part
// of a FrameRenderer's buffered frame) since the preview region is treated
// as content the chrome repaint must never clear or overwrite.
void write_raw_passthrough(int fd, int row, int col, const std::string& raw_bytes);

// Visible column width of a UTF-8 string with ANSI SGR codes stripped for
// measurement purposes (1 column per codepoint — no wide-char/grapheme
// clustering, matching this codebase's existing behavior). Used to truncate
// preview lines to a pane's column budget without corrupting an open SGR
// sequence mid-line.
size_t visible_width(const std::string& utf8_text);

// Truncate a UTF-8 string (which may contain ANSI SGR codes) to at most
// `max_cols` visible columns, preserving any SGR codes that fall within the
// kept prefix and appending a reset code if any SGR was open at the cut
// point, so a clipped line doesn't leak color into whatever comes after it.
std::string truncate_ansi_text(const std::string& text, size_t max_cols);

} // namespace fzf
