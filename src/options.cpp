#include "options.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
#include <sstream>

namespace fzf {

Options parse_options(int argc, char* argv[]) {
    Options opts;

    // Preprocess argv to handle +i and +m (CLI11 doesn't support + prefix)
    bool case_sensitive_flag = false;
    bool no_multi_flag = false;
    std::vector<std::string> arg_storage;

    arg_storage.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "+i") {
            case_sensitive_flag = true;
        } else if (arg == "+m") {
            no_multi_flag = true;
        } else {
            arg_storage.push_back(arg);
        }
    }

    std::vector<char*> new_argv;
    new_argv.reserve(arg_storage.size());
    for (auto& s : arg_storage) {
        new_argv.push_back(const_cast<char*>(s.c_str()));
    }
    int new_argc = static_cast<int>(new_argv.size());

    CLI::App app{"fzf++ - Command-line fuzzy finder (C++ implementation)"};

    bool version = false;
    app.add_flag("-v,--version", version, "Show version");

    std::string case_str;
    app.add_option("--case", case_str, "Case sensitivity (smart/ignore/respect)")
        ->check(CLI::IsMember({"smart", "ignore", "respect"}));

    bool case_insensitive_flag = false;
    app.add_flag("-i", case_insensitive_flag, "Case insensitive");

    app.add_flag("-e,--exact", [&opts](int64_t) {
        opts.fuzzy = false;
    }, "Enable exact match (disable fuzzy matching)");

    app.add_flag("--fuzzy", [&opts](int64_t) {
        opts.fuzzy = true;
    }, "Enable fuzzy matching (default)");

    app.add_flag("-x,--extended", opts.extended,
                 "Enable extended search mode (default: true)");

    app.add_flag("--ansi", opts.ansi,
                 "Enable processing of ANSI color codes");

    std::string height_str;
    app.add_option("--height", height_str,
                   "Display height (lines or %, 0 for fullscreen)");

    std::string layout_str;
    app.add_option("--layout", layout_str, "Layout type (default/reverse/reverse-list)")
        ->check(CLI::IsMember({"default", "reverse", "reverse-list"}));

    app.add_flag("--reverse", [&opts](int64_t) {
        opts.layout = LayoutType::Reverse;
    }, "Shorthand for --layout=reverse");

    app.add_flag("--no-reverse", [&opts](int64_t) {
        opts.layout = LayoutType::Default;
    }, "Shorthand for --layout=default");

    app.add_option("--prompt", opts.prompt, "Input prompt string");

    app.add_option("--header", opts.header, "Header line to display");

    app.add_flag("--header-first", opts.header_first, "Print header before prompt line");

    app.add_flag("--no-header-first", [&opts](int64_t) {
        opts.header_first = false;
    }, "Print header after prompt line (default)");

    app.add_flag("--border", opts.border, "Draw border around interface");


    app.add_flag("--wrap", opts.wrap, "Enable line wrapping");

    app.add_flag("--no-mouse", opts.no_mouse, "Disable mouse");

    app.add_flag("--no-unicode", opts.no_unicode, "Disable unicode");


    std::string margin_str;
    app.add_option("--margin", margin_str, "Margins (top,right,bottom,left)");

    std::string info_str;
    app.add_option("--info", info_str, "Info display mode (default/inline/hidden)");

    std::vector<std::string> color_specs;
    app.add_option("--color", color_specs, "Color scheme")
        ->allow_extra_args();

    app.add_option("--border-label", opts.border_label, "Border label text");
    app.add_option("--marker", opts.marker, "Multi-select marker");
    app.add_option("--pointer", opts.pointer, "Pointer to current line");
    app.add_option("--separator", opts.separator, "Separator character");
    app.add_option("--scrollbar", opts.scrollbar, "Scrollbar character");
    app.add_option("--tabstop", opts.tabstop, "Tab stop width");

    app.add_flag("-m,--multi", opts.multi, "Enable multi-select mode");

    app.add_flag("--no-multi", [&opts](int64_t) {
        opts.multi = false;
    }, "Disable multi-select mode");

    app.add_flag("--cycle", opts.cycle, "Enable cyclic scrolling");

    app.add_option("-q,--query", opts.query, "Initial query string");

    std::string filter_query;
    app.add_option("-f,--filter", filter_query,
                   "Filter mode (non-interactive, print matches)");

    app.add_flag("-1,--select-1", opts.select_1,
                 "Auto-select if only one match");

    app.add_flag("-0,--exit-0", opts.exit_0,
                 "Exit immediately if no match");

    app.add_flag("--print-query", opts.print_query,
                 "Print query before results");

    app.add_flag("--no-sort", [&opts](int64_t) {
        opts.sort = false;
    }, "Disable sorting");

    app.add_option("-d,--delimiter", opts.delimiter,
                   "Field delimiter (regex)");

    std::string with_nth_str;
    app.add_option("--with-nth", with_nth_str,
                   "Display only specified fields (comma-separated, 1-based)");

    std::vector<std::string> bind_specs;
    app.add_option("--bind", bind_specs,
                   "Custom key bindings (key:action)")
        ->allow_extra_args();

    std::string expect_str;
    app.add_option("--expect", expect_str,
                   "Comma-separated list of keys that trigger exit with key name");

    app.add_option("--with-shell", opts.with_shell,
                   "Shell to use for execute actions");

    app.add_flag("--read0", opts.read_zero,
                 "Read null-delimited input");

    app.add_option("--preview", opts.preview_command,
                   "Preview command");

    app.add_option("--preview-window", opts.preview_window,
                   "Preview window options");

    try {
        app.parse(new_argc, new_argv.data());
    } catch (const CLI::ParseError &e) {
        std::exit(app.exit(e));
    }

    // Parse --with-nth
    if (!with_nth_str.empty()) {
        std::stringstream ss(with_nth_str);
        std::string field;
        while (std::getline(ss, field, ',')) {
            try {
                opts.with_nth.push_back(std::stoi(field));
            } catch (...) {
                std::cerr << "Invalid field number: " << field << std::endl;
            }
        }
    }

    // Parse --bind specifications
    for (const auto& spec : bind_specs) {
        size_t colon = spec.find(':');
        if (colon != std::string::npos) {
            std::string key = spec.substr(0, colon);
            std::string action = spec.substr(colon + 1);
            opts.bindings[key] = action;
        }
    }


    if (version) {
        std::cout << "fzf++ version 0.1.1 (C++20 implementation)" << std::endl;
        std::cout << "Compatible with fzf" << std::endl;
        std::exit(0);
    }

    if (!filter_query.empty()) {
        opts.filter = true;
        opts.query = filter_query;
    }

    if (!margin_str.empty()) {
        std::stringstream ss(margin_str);
        std::string value;
        std::vector<int> margins;
        while (std::getline(ss, value, ',')) {
            try {
                margins.push_back(std::stoi(value));
            } catch (...) {
                std::cerr << "Invalid margin value: " << value << std::endl;
            }
        }
        if (margins.size() == 4) {
            opts.margin_top = margins[0];
            opts.margin_right = margins[1];
            opts.margin_bottom = margins[2];
            opts.margin_left = margins[3];
        }
    }

    if (case_insensitive_flag) {
        opts.case_mode = CaseMode::Ignore;
    }
    if (case_sensitive_flag) {
        opts.case_mode = CaseMode::Respect;
    }

    if (no_multi_flag) {
        opts.multi = false;
    }

    if (!case_str.empty()) {
        if (case_str == "smart") {
            opts.case_mode = CaseMode::Smart;
        } else if (case_str == "ignore") {
            opts.case_mode = CaseMode::Ignore;
        } else if (case_str == "respect") {
            opts.case_mode = CaseMode::Respect;
        }
    }

    if (!layout_str.empty()) {
        if (layout_str == "default") {
            opts.layout = LayoutType::Default;
        } else if (layout_str == "reverse") {
            opts.layout = LayoutType::Reverse;
        } else if (layout_str == "reverse-list") {
            opts.layout = LayoutType::Reverse;
        }
    }

    if (!height_str.empty()) {
        if (height_str.back() == '%') {
            std::string num_str = height_str.substr(0, height_str.size() - 1);
            try {
                opts.height = std::stoi(num_str);
                opts.height_is_percent = true;
                if (opts.height < 0 || opts.height > 100) {
                    std::cerr << "Warning: height percentage should be 0-100, got " << opts.height << "%" << std::endl;
                    opts.height = std::clamp(opts.height, 0, 100);
                }
            } catch (...) {
                std::cerr << "Invalid height value: " << height_str << std::endl;
                opts.height = 0;
                opts.height_is_percent = false;
            }
        } else {
            try {
                opts.height = std::stoi(height_str);
                opts.height_is_percent = false;
                if (opts.height < 0) {
                    std::cerr << "Warning: height must be non-negative, got " << opts.height << std::endl;
                    opts.height = 0;
                }
            } catch (...) {
                std::cerr << "Invalid height value: " << height_str << std::endl;
                opts.height = 0;
                opts.height_is_percent = false;
            }
        }
    }

    if (!info_str.empty()) {
        if (info_str == "hidden") {
            opts.info_hidden = true;
        }
    }

    // Parse --expect (comma-separated keys)
    if (!expect_str.empty()) {
        std::stringstream ss(expect_str);
        std::string key;
        while (std::getline(ss, key, ',')) {
            opts.expect_keys.push_back(key);
        }
    }

    // Process --preview-window
    if (!opts.preview_window.empty()) {
        std::stringstream ss(opts.preview_window);
        std::string part;
        while (std::getline(ss, part, ',')) {
            if (part == "left" || part == "right" || part == "up" || part == "down") {
                opts.preview_position = part;
            }
            else if (!part.empty() && part.back() == '%') {
                std::string num_str = part.substr(0, part.size() - 1);
                try {
                    opts.preview_size_percent = std::stoi(num_str);
                    opts.preview_size_percent = std::clamp(opts.preview_size_percent, 0, 100);
                } catch (...) {
                    std::cerr << "Invalid preview size: " << part << std::endl;
                }
            }
            else if (part == "wrap") {
                opts.preview_wrap = true;
            }
        }
    }

    for (const auto& spec : color_specs) {
        size_t colon = spec.find(':');
        if (colon != std::string::npos) {
            std::string key = spec.substr(0, colon);
            std::string value = spec.substr(colon + 1);
            opts.colors[key] = value;
        }
    }

    return opts;
}

} // namespace fzf
