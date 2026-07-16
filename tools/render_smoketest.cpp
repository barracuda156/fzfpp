// Standalone visual smoke test for src/render.hpp — builds a fake fzf-style
// frame (info line, header, result rows with a highlighted match and an
// inverted cursor row, a multi-select prefix, a border) and prints it to
// the real terminal for eyeballing. Not automated (no pixel comparison —
// "at least as good as FTXUI, not pixel-identical" is the bar); run
// manually and look at it:
//
//   ./render_smoketest

#include "../src/render.hpp"
#include "../src/tty.hpp"

#include <cstdio>
#include <unistd.h>

using namespace fzf;

int main() {
    int rows = 24, cols = 80;
    get_terminal_size(STDOUT_FILENO, rows, cols);

    FrameRenderer frame(rows, cols);

    frame.draw_border();

    int row = 1;
    frame.draw_text(row++, 1, "3", {}, cols - 2);

    frame.draw_text(row++, 1, "-- header line --", Style{Color::Default, true, false}, cols - 2);

    frame.draw_separator(row++);

    // Result rows: one plain, one with a highlighted match, one selected
    // (multi-select prefix), one as the cursor row (inverted).
    frame.draw_text(row++, 1, "  plain_result.txt", {}, cols - 2);

    {
        Row spans;
        spans.push_back(Span{"  hello_", Style{}});
        spans.push_back(Span{"world", Style{Color::Yellow, true, false}});
        spans.push_back(Span{".cpp", Style{}});
        frame.draw_row(row++, 1, spans, cols - 2);
    }

    frame.draw_text(row++, 1, "> selected_item.md", {}, cols - 2);

    frame.draw_text(row++, 1, "  cursor_row_here.log", Style{Color::Default, false, true}, cols - 2);

    frame.draw_separator(row++);
    frame.draw_text(row++, 1, "> query text", {}, cols - 2);

    ssize_t r1 = write(STDOUT_FILENO, frame.bytes().data(), frame.bytes().size());
    frame.move_cursor(row - 1, 1 + 14);  // parked after "> query text"
    std::string cursor_move = "\x1b[" + std::to_string(row) + ";" + std::to_string(1 + 14 + 1) + "H";
    ssize_t r2 = write(STDOUT_FILENO, cursor_move.data(), cursor_move.size());
    (void)r1;
    (void)r2;

    std::printf("\n\n(frame drawn above; press Enter to exit)\n");
    std::fflush(stdout);
    getchar();
    return 0;
}
