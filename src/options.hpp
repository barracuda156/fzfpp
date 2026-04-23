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

    // Margins (clifm compatibility)
    int margin_top = 0;
    int margin_right = 0;
    int margin_bottom = 0;
    int margin_left = 0;

    // Interaction options
    bool multi = false;        // Multi-select mode
    bool cycle = false;        // Cycle through results

    // Initial query
    std::string query;

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

    // Field selection for display
    std::vector<int> with_nth;  // Fields to display (1-based, empty = all)

    // Key bindings
    std::map<std::string, std::string> bindings;  // key -> action
    std::string with_shell;  // Shell to use for execute actions (e.g., "bash -c")

    // yt-x compatibility options
    bool info_hidden = false;
    std::vector<std::string> expect_keys;
    std::string preview_position = "right";
    int preview_size_percent = 50;
    bool preview_wrap = false;
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
