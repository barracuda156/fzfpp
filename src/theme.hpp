#pragma once

// Color scheme materialization (fzf: src/tui/tui.go InitTheme and the
// Default16 / Dark256 / Light256 / NoColorTheme tables). The option parser
// records the base theme and the --color overrides; this turns them into
// one resolved ColorAttr per slot the renderer uses.

#include "options.hpp"

namespace fzf {

struct ColorScheme {
    bool colored = true;
    ColorAttr input, fg, bg, list_fg, list_bg, dark_bg, prompt, match, current, current_match,
        spinner, info, pointer, marker, header, border, border_label, separator, scrollbar, gutter,
        preview_fg, preview_bg, preview_border, preview_label, disabled, ghost, selected_fg,
        selected_bg, selected_match;
};

// Picks the base theme (explicit --color base, else 256 colors when $TERM
// mentions 256, else 16 colors), layers the --color overrides on top and
// derives the dependent slots like fzf's InitTheme. `--bold` (boldify) and
// `--black` are applied here too.
ColorScheme materialize_theme(const Options& opts);

} // namespace fzf
