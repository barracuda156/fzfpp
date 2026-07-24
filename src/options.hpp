#pragma once

#include "item.hpp"
#include <string>
#include <vector>
#include <map>

namespace fzf {

// Layout type
enum class LayoutType {
    Default,      // Bottom up, full screen
    Reverse,      // Top down
    ReverseList   // Top down with list at bottom
};

// Options structure matching fzf's key options
struct Options {
    // Search options
    CaseMode case_mode = CaseMode::Smart;
    AlgoType algo = AlgoType::FuzzyV2;
    bool extended = true;      // Extended search mode
    bool fuzzy = true;         // Fuzzy matching

    // Display options
    bool ansi = false;         // Parse ANSI color codes
    int height = 0;            // Height in lines (0 = fullscreen) or percentage value
    bool height_is_percent = false;  // True if height is a percentage value
    LayoutType layout = LayoutType::Default;
    std::string prompt = "> ";
    std::string header;        // Header line to display
    bool header_first = false; // Print header before prompt line
    bool border = false;       // Draw border around interface
    bool wrap = false;         // Enable line wrapping
    bool no_mouse = false;
    bool no_unicode = false;   // Disable unicode characters

    // Margins (clifm compatibility). fzf's --margin accepts 1, 2, or 4
    // comma-separated values (1=all sides; 2=vertical,horizontal;
    // 4=top,right,bottom,left) and each value may be a percentage.
    struct Margin {
        int value = 0;
        bool percent = false;
    };
    Margin margin_top;
    Margin margin_right;
    Margin margin_bottom;
    Margin margin_left;

    // Interaction options
    bool multi = false;        // Multi-select mode
    bool cycle = false;        // Cycle through results

    // Initial query
    std::string query;

    // Disabled mode: the query box is shown and typing updates {q} and fires
    // the change: event, but the query does NOT filter the list. Used with
    // reload(...) so an external command does the filtering (viu's pattern).
    bool disabled = false;

    // Filter mode (non-interactive)
    bool filter = false;

    // Selector mode options
    bool select_1 = false;     // Auto-select if only one match
    bool exit_0 = false;       // Exit immediately if no match

    // Print query on accept
    bool print_query = false;

    // Sorting
    bool sort = true;

    // Preview
    std::string preview_command;
    std::string preview_window;  // Preview window options (clifm compatibility)

    // Input options
    bool read_zero = false;    // Read null-delimited input

    // Delimiter for field splitting
    std::string delimiter;

    // Field selection for display (--with-nth) and output (--accept-nth).
    // Each range uses fzf's nth semantics: 1-based, negative counts from the
    // end (-1 = last field), and open-ended ranges (2.. / ..3) are supported.
    // Empty vector = use the whole line.
    std::vector<FieldRange> with_nth;    // Fields shown in the list
    std::vector<FieldRange> accept_nth;  // Fields printed on accept

    // Key bindings
    std::map<std::string, std::string> bindings;  // key -> action
    std::string with_shell;  // Shell to use for execute actions (e.g., "bash -c")

    // yt-x compatibility options
    bool info_hidden = false;
    std::vector<std::string> expect_keys;
    std::string preview_position = "right";
    int preview_size_percent = 50;
    bool preview_size_is_percent = true;   // false = preview_size_percent holds an absolute line/col count
    bool preview_wrap = false;
    bool preview_hidden = false;           // --preview-window=hidden
    bool preview_follow = false;           // --preview-window=follow
    std::map<std::string, std::string> colors;
    std::string border_style;
    std::string border_label;
    std::string marker = ">";
    std::string pointer = ">";
    std::string separator = "-";
    std::string scrollbar = "|";
    int tabstop = 8;
};

// Parse command-line arguments
Options parse_options(int argc, char* argv[]);

} // namespace fzf
