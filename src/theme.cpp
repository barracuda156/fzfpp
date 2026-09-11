#include "theme.hpp"

#include <cstdlib>
#include <cstring>

namespace fzf {

namespace {

// fzf: tui.go color constants
constexpr ColorCode colRed = 1, colGreen = 2, colYellow = 3, colBlue = 4,
    colMagenta = 5, colCyan = 6, colGrey = 8, colBrightGreen = 10, colBrightWhite = 15;

ColorAttr ca(ColorCode c) { return ColorAttr{c, 0}; }
constexpr ColorAttr kUndefined{};                     // colUndefined, no attr
const ColorAttr kDefault = ColorAttr{kColorDefault, 0};

struct BaseTheme {
    bool colored;
    ColorAttr input, fg, bg, dark_bg, prompt, match, current, current_match, spinner, info,
        pointer, marker, header, border, border_label;
};

// fzf: Default16
const BaseTheme kDefault16{true, kDefault, kDefault, kDefault, ca(colGrey), ca(colBlue), ca(colGreen),
                           ca(colBrightWhite), ca(colBrightGreen), ca(colGreen), ca(colYellow),
                           ca(colRed), ca(colMagenta), ca(colCyan), kUndefined, kDefault};
// fzf: Dark256
const BaseTheme kDark256{true, kDefault, kDefault, kDefault, ca(236), ca(110), ca(108), ca(254), ca(151),
                         ca(148), ca(144), ca(161), ca(168), ca(109), ca(59), ca(145)};
// fzf: Light256
const BaseTheme kLight256{true, kDefault, kDefault, kDefault, ca(251), ca(25), ca(66), ca(237), ca(23),
                          ca(65), ca(101), ca(161), ca(168), ca(31), ca(145), ca(59)};
// fzf: NoColorTheme (root colors default, derived ones undefined)
const BaseTheme kNoColor{false, kDefault, kDefault, kDefault, kDefault, kDefault, kDefault, kUndefined,
                         kUndefined, kDefault, kDefault, kDefault, kDefault, kDefault, kUndefined, kDefault};

// fzf: InitTheme's `o` -- b's color and attributes override a's when set.
// An override without attributes keeps the base attributes (our parser
// stores "no attribute" as 0, which stands for fzf's AttrUndefined here).
ColorAttr over(ColorAttr a, ColorAttr b) {
    ColorAttr c = a;
    if (b.color != kColorUndefined) c.color = b.color;
    if (b.attr != 0) c.attr = b.attr;
    return c;
}

bool is_undefined(const ColorAttr& c) { return c.color == kColorUndefined && c.attr == 0; }

} // namespace

ColorScheme materialize_theme(const Options& opts) {
    const BaseTheme* base = nullptr;
    ThemeBase which = opts.theme.base;
    if (opts.theme.explicit_base) which = *opts.theme.explicit_base;
    if (which == ThemeBase::Empty) {
        const char* term = std::getenv("TERM");
        which = (term && std::strstr(term, "256")) ? ThemeBase::Dark256 : ThemeBase::Default16;
    }
    switch (which) {
        case ThemeBase::NoColor: base = &kNoColor; break;
        case ThemeBase::Default16: base = &kDefault16; break;
        case ThemeBase::Light256: base = &kLight256; break;
        case ThemeBase::Dark256: default: base = &kDark256; break;
    }
    const Theme& t = opts.theme;
    auto ov = [&](ThemeSlot s) { return t.slot(s); };

    ColorScheme s;
    s.colored = base->colored;
    ColorAttr bg_override = ov(ThemeSlot::Bg);
    if (opts.black) bg_override = ca(0);

    // fzf: boldify
    auto boldify = [&](ColorAttr c) {
        if (opts.bold && !(c.attr & kAttrRegular)) c.attr |= kAttrBoldForce;
        return c;
    };
    ColorAttr current_ov = boldify(ov(ThemeSlot::Current));
    ColorAttr current_match_ov = boldify(ov(ThemeSlot::CurrentMatch));
    ColorAttr prompt_ov = boldify(ov(ThemeSlot::Prompt));
    ColorAttr input_ov = boldify(ov(ThemeSlot::Input));
    ColorAttr pointer_ov = boldify(ov(ThemeSlot::Pointer));
    ColorAttr spinner_ov = boldify(ov(ThemeSlot::Spinner));

    s.input = over(base->input, input_ov);
    s.fg = over(base->fg, ov(ThemeSlot::Fg));
    s.bg = over(base->bg, bg_override);
    s.dark_bg = over(base->dark_bg, ov(ThemeSlot::DarkBg));
    s.prompt = over(base->prompt, prompt_ov);
    ColorAttr match = ov(ThemeSlot::Match);
    if (!base->colored && is_undefined(match)) match.attr = kAttrUnderline;
    s.match = over(base->match, match);
    s.list_fg = over(s.fg, ov(ThemeSlot::ListFg));
    s.list_bg = over(s.bg, ov(ThemeSlot::ListBg));
    ColorAttr current = current_ov;
    if (!base->colored && is_undefined(current)) current.attr |= kAttrReverse;
    ColorAttr resolved_current = over(base->current, current);
    s.current = s.list_fg.merge(resolved_current);
    ColorAttr current_match = current_match_ov;
    if (!base->colored && is_undefined(current_match)) current_match.attr |= kAttrReverse | kAttrUnderline;
    s.current_match = over(base->current_match, current_match);
    s.spinner = over(base->spinner, spinner_ov);
    s.info = over(base->info, ov(ThemeSlot::Info));
    s.pointer = over(base->pointer, pointer_ov);
    s.marker = over(base->marker, ov(ThemeSlot::Marker));
    s.header = over(base->header, ov(ThemeSlot::Header));
    ColorAttr border = ov(ThemeSlot::Border);
    if (is_undefined(base->border) && is_undefined(border)) border.attr = kAttrDim;
    s.border = over(base->border, border);
    s.border_label = over(base->border_label, ov(ThemeSlot::BorderLabel));

    s.selected_fg = s.list_fg.merge(ov(ThemeSlot::SelectedFg));
    s.selected_bg = over(s.list_bg, ov(ThemeSlot::SelectedBg));
    s.selected_match = over(s.match, ov(ThemeSlot::SelectedMatch));

    ColorAttr ghost = ov(ThemeSlot::Ghost);
    if (is_undefined(ghost)) ghost.attr = kAttrDim;
    else if (ghost.color != kColorUndefined && ghost.attr == 0) ghost.attr = kAttrRegular;
    s.ghost = over(s.input, ghost);
    s.disabled = over(s.input, ov(ThemeSlot::Disabled));

    ColorAttr gutter = ov(ThemeSlot::Gutter);
    if (!base->colored && is_undefined(gutter)) gutter.attr = kAttrDim;
    s.gutter = over(s.dark_bg, gutter);
    s.preview_fg = over(s.fg, ov(ThemeSlot::PreviewFg));
    s.preview_bg = over(s.bg, ov(ThemeSlot::PreviewBg));
    s.preview_label = over(s.border_label, ov(ThemeSlot::PreviewLabel));
    s.preview_border = over(s.border, ov(ThemeSlot::PreviewBorder));
    ColorAttr list_border = over(s.border, ov(ThemeSlot::ListBorder));
    s.separator = over(list_border, ov(ThemeSlot::Separator));
    s.scrollbar = over(list_border, ov(ThemeSlot::Scrollbar));
    return s;
}

} // namespace fzf
