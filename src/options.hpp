#pragma once

// Command-line options for fzf++.
//
// This is a hand-written port of fzf's option parser (fzf: src/options.go,
// parseOptions / ParseOptions / validateOptions / postProcessOptions).
// Every option name fzf 0.74 accepts is recognized here. Options are either
//   - implemented: they change behaviour exactly as in fzf,
//   - accepted:    parsed and validated like fzf, but with no effect yet
//                  (FZFPP_WARN_UNSUPPORTED=1 prints a warning for each one
//                  that is actually used),
// and anything else is an error: fzf's message on stderr and exit code 2.
// See docs/DESIGN.md section 3 for the policy.

#include "item.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fzf {

// ---------------------------------------------------------------------------
// Value types (fzf: src/options.go, src/tui/tui.go)
// ---------------------------------------------------------------------------

enum class LayoutType { Default, Reverse, ReverseList };

enum class InfoStyle { Default, Right, Inline, InlineRight, Hidden };

// fzf: tui.BorderShape
enum class BorderShape {
    Undefined, None, Line, Inline, Rounded, Sharp, Bold, Block, ThinBlock,
    Double, Dashed, Horizontal, Vertical, Top, Bottom, Left, Right, Phantom
};

// fzf: windowPosition
enum class WindowPosition { Up, Down, Left, Right, Next, Center };

// fzf: criterion. byScore is always first and implicit; byIndex is the
// implicit last tiebreak.
enum class Criterion { Score, Chunk, Length, Begin, End, Pathname };

enum class TrackOption { Disabled, Enabled, Current };

// A size that is either an absolute number of lines/columns or a percentage.
struct SizeSpec {
    double size = 0;
    bool percent = false;
};

// fzf: heightSpec. `auto_` is the `~` prefix (adaptive height), `inverse`
// the `-` prefix (terminal height minus size). `index` is the position of
// the option among all parsed arguments (fzf uses it to order --height
// against --tmux).
struct HeightSpec {
    double size = 0;
    bool percent = false;
    bool auto_ = false;
    bool inverse = false;
    int index = 0;
    bool is_set() const { return size > 0 || auto_; }
};

struct LabelOpts {
    std::string label;
    int column = 0;      // 0 = center, positive from left, negative from right
    bool bottom = false;
};

// fzf: Range (nth expression). 0 stands for the ellipsis (open end), exactly
// like fzf's rangeEllipsis, so {0,0} is ".." (everything), {2,0} is "2..",
// {0,3} is "..3", {-1,-1} is the last field.
struct Range {
    int begin = 0;
    int end = 0;
    bool operator==(const Range& o) const { return begin == o.begin && end == o.end; }
    bool is_full() const { return begin == 0 && end == 0; }
};
constexpr int kRangeEllipsis = 0;

// fzf: Delimiter. `awk` means no delimiter was given (whitespace runs).
// `pattern` holds the literal string, or the regular expression source when
// `is_regex` is set (compiled by the tokenizer, T1.3).
struct Delimiter {
    bool awk = true;
    bool is_regex = false;
    std::string pattern;
};

// fzf: previewOpts
struct PreviewOpts {
    std::string command;
    WindowPosition position = WindowPosition::Right;
    SizeSpec size{50, true};
    std::string scroll;            // +SCROLL[OFFSETS][/DENOM] expression
    bool hidden = false;
    bool wrap = false;
    bool wrap_word = false;
    bool cycle = false;
    bool follow = false;
    bool info = true;
    BorderShape border = BorderShape::Rounded;
    int header_lines = 0;
    int threshold = 0;             // columns for the <N(...) alternative
    std::shared_ptr<PreviewOpts> alternative;
    bool visible() const { return !hidden; }
};

struct WalkerOpts {
    bool file = true;
    bool dir = false;
    bool hidden = true;
    bool follow = true;
};

struct TmuxOptions {
    WindowPosition position = WindowPosition::Center;
    SizeSpec width{50, true};
    SizeSpec height{50, true};
    bool border = false;
    int index = 0;
};

// ---------------------------------------------------------------------------
// Colors (fzf: src/tui/tui.go Color / Attr / ColorTheme, options.go parseTheme)
// ---------------------------------------------------------------------------

// Color encoding: kColorUndefined, kColorDefault, 0..255 for ANSI/256
// colors, or (1 << 24) | 0xRRGGBB for 24-bit colors.
using ColorCode = int32_t;
constexpr ColorCode kColorUndefined = -2;
constexpr ColorCode kColorDefault = -1;
inline bool color_is_24bit(ColorCode c) { return c >= (1 << 24); }

using Attr = uint32_t;
constexpr Attr kAttrBold = 1;
constexpr Attr kAttrDim = 2;
constexpr Attr kAttrItalic = 4;
constexpr Attr kAttrUnderline = 8;
constexpr Attr kAttrBlink = 16;
constexpr Attr kAttrBlink2 = 32;
constexpr Attr kAttrReverse = 64;
constexpr Attr kAttrStrikeThrough = 128;
constexpr Attr kAttrRegular = 1u << 8;      // "regular": reset attributes
constexpr Attr kAttrClear = 1u << 9;
constexpr Attr kAttrBoldForce = 1u << 10;
constexpr Attr kAttrFullBg = 1u << 11;
constexpr Attr kAttrStrip = 1u << 12;
constexpr int kUnderlineStyleShift = 13;
constexpr Attr kUnderlineStyleMask = 0x7u << kUnderlineStyleShift;
constexpr Attr kUlStyleDouble = 0x1u << kUnderlineStyleShift;
constexpr Attr kUlStyleCurly = 0x2u << kUnderlineStyleShift;
constexpr Attr kUlStyleDotted = 0x3u << kUnderlineStyleShift;
constexpr Attr kUlStyleDashed = 0x4u << kUnderlineStyleShift;

struct ColorAttr {
    ColorCode color = kColorUndefined;
    Attr attr = 0;
    bool is_color_defined() const { return color != kColorUndefined; }
    // fzf: Attr.Merge -- `other` layered on top of this.
    static Attr merge_attr(Attr a, Attr b) {
        if (b & kAttrRegular) {
            return (b & ~kAttrRegular) | (a & kAttrBoldForce);
        }
        Attr merged = (a & ~kAttrRegular) | b;
        if (b & kAttrUnderline) {
            merged = (merged & ~kUnderlineStyleMask) | (b & kUnderlineStyleMask);
        }
        return merged;
    }
    // fzf: ColorAttr.Merge
    ColorAttr merge(const ColorAttr& other) const {
        ColorAttr out = *this;
        if (other.color != kColorUndefined) out.color = other.color;
        out.attr = merge_attr(attr, other.attr);
        return out;
    }
};

// Every color slot fzf's --color accepts (fzf: ColorTheme fields).
enum class ThemeSlot {
    Input, Ghost, Disabled, Fg, Bg, ListFg, ListBg, AltBg, Nth, Nomatch,
    SelectedFg, SelectedBg, SelectedMatch, PreviewFg, PreviewBg, DarkBg,
    Gutter, AltGutter, Prompt, InputBg, InputBorder, InputLabel, Match,
    Current, CurrentMatch, Spinner, Info, Pointer, Marker, Header, HeaderBg,
    HeaderBorder, HeaderLabel, Footer, FooterBg, FooterBorder, FooterLabel,
    Separator, Scrollbar, Border, PreviewBorder, PreviewLabel,
    PreviewScrollbar, BorderLabel, ListLabel, ListBorder, GapLine,
    Count_
};

// fzf keeps a materialized ColorTheme; we keep the base theme name plus the
// per-slot overrides that --color layered on top of it, in fzf's own merge
// semantics. The renderer (T2.4) materializes base + overrides.
//   base:  which built-in theme the overrides apply to. A base token in a
//          --color spec ("dark", "light", "16", "bw") resets the overrides,
//          exactly like fzf duplicating the base theme afresh.
enum class ThemeBase { Empty, NoColor, Default16, Dark256, Light256 };

struct Theme {
    ThemeBase base = ThemeBase::Empty;
    // Base selected by an explicit token; fzf's opts.BaseTheme. Empty means
    // "auto-detect from the terminal".
    std::optional<ThemeBase> explicit_base;
    std::array<ColorAttr, static_cast<size_t>(ThemeSlot::Count_)> overrides{};

    ColorAttr& slot(ThemeSlot s) { return overrides[static_cast<size_t>(s)]; }
    const ColorAttr& slot(ThemeSlot s) const { return overrides[static_cast<size_t>(s)]; }
    void reset(ThemeBase b) {
        base = b;
        for (auto& o : overrides) o = ColorAttr{};
    }
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    // Exiting modes (last one wins, fzf: clearExitingOpts)
    bool help = false;
    bool version = false;
    bool man = false;
    bool bash = false;
    bool zsh = false;
    bool fish = false;
    bool nushell = false;

    // Search
    bool fuzzy = true;                 // -e / --exact clears it
    AlgoType algo = AlgoType::FuzzyV2;
    std::string scheme;                // "" = unknown (decided after parsing)
    bool extended = true;
    bool phony = false;                // --disabled / --phony
    bool inputless = false;            // --no-input
    CaseMode case_mode = CaseMode::Smart;
    bool normalize = true;             // --literal clears it
    std::vector<Range> nth;            // --nth
    std::string with_nth_expr;         // --with-nth (ranges or template)
    std::string accept_nth_expr;       // --accept-nth (ranges or template)
    Delimiter delimiter;
    int sort = 1000;                   // 0 = --no-sort; N = sort when <= N items
    bool raw = false;
    TrackOption track = TrackOption::Disabled;
    std::vector<Range> id_nth;
    bool tac = false;
    int tail = 0;
    std::vector<Criterion> criteria;   // empty = decided by scheme
    int multi = 0;                     // 0 = single select, N = limit
    bool ansi = false;
    bool mouse = true;
    Theme theme;
    bool black = false;
    bool bold = true;

    // Layout
    HeightSpec height;
    int min_height = -10;              // negative = automatic (fzf semantics)
    LayoutType layout = LayoutType::Default;
    bool cycle = false;
    bool wrap = false;
    bool wrap_word = false;
    std::optional<std::string> wrap_sign;
    std::optional<std::string> preview_wrap_sign;
    bool multi_line = true;
    bool cursor_line = false;          // --highlight-line
    bool keep_right = false;
    bool hscroll = true;
    int hscroll_off = 10;
    int scroll_off = 3;
    bool file_word = false;
    InfoStyle info_style = InfoStyle::Default;
    std::string info_prefix;
    std::string info_command;
    std::string ghost;
    std::optional<std::string> separator;   // nullopt = default line
    std::string jump_labels = "asdfghjklqwertyuiopzxcvbnm1234567890";
    std::string prompt = "> ";
    std::optional<std::string> gutter;
    std::optional<std::string> gutter_raw;
    std::optional<std::string> pointer;     // nullopt = default (set in post-processing)
    std::optional<std::string> marker;
    std::optional<std::array<std::string, 3>> marker_multi;
    std::optional<std::string> ellipsis;
    std::optional<std::string> scrollbar;   // nullopt = default; "" = none
    int gap = 0;
    std::optional<std::string> gap_line;
    std::array<SizeSpec, 4> margin{};       // top, right, bottom, left
    std::array<SizeSpec, 4> padding{};
    BorderShape border_shape = BorderShape::Undefined;
    BorderShape list_border = BorderShape::Undefined;
    BorderShape input_border = BorderShape::Undefined;
    BorderShape header_border = BorderShape::Undefined;
    BorderShape header_lines_border = BorderShape::Undefined;
    BorderShape footer_border = BorderShape::Undefined;
    LabelOpts border_label;
    LabelOpts list_label;
    LabelOpts input_label;
    LabelOpts header_label;
    LabelOpts footer_label;
    LabelOpts preview_label;
    bool unicode = true;
    bool ambidouble = false;
    int tabstop = 8;

    // Input / output
    std::string query;
    bool select_1 = false;
    bool exit_0 = false;
    std::optional<std::string> filter;      // -f / --filter
    bool print_query = false;
    bool read_zero = false;                 // --read0
    bool print0 = false;
    bool sync = false;
    std::vector<std::string> header;        // --header, split on newlines
    int header_lines = 0;
    bool header_first = false;
    std::vector<std::string> footer;
    PreviewOpts preview;
    std::string with_shell;
    bool clear_on_exit = true;

    // Key bindings. The raw specs are kept in order; keymap.cpp (T1.2)
    // turns them into the structured Keymap.
    std::vector<std::string> bind_specs;        // each --bind argument
    std::vector<std::string> expect_specs;      // each --expect argument
    bool no_expect = false;                     // --no-expect seen after expects
    std::vector<std::string> toggle_sort_specs; // --toggle-sort KEY
    bool toggle_sort = false;

    // Accepted, not implemented (see docs/DESIGN.md non-goals)
    std::optional<std::string> history;
    int history_size = 1000;
    std::optional<std::string> listen_addr;
    bool unsafe = false;
    std::optional<TmuxOptions> tmux;
    std::string tty_default = "/dev/tty";
    bool force_tty_in = false;
    std::string proxy_script;
    bool no_winpty = false;
    int freeze_left = 0;
    int freeze_right = 0;
    int threads = 0;
    WalkerOpts walker;
    std::vector<std::string> walker_root{"."};
    std::vector<std::string> walker_skip{".git", "node_modules"};

    // ------------------------------------------------------------------
    // LEGACY VIEW -- derived from the fields above by derive_legacy_fields()
    // so that the pre-rewrite terminal.cpp / main.cpp / reader keep working
    // unchanged. Every field in this block is removed in T1.7 when the
    // executor and event loop switch to the canonical fields.
    // ------------------------------------------------------------------
    bool disabled = false;             // = phony
    int legacy_height = 0;             // --height as an int (0 = fullscreen)
    bool height_is_percent = false;
    std::string legacy_header;         // --header lines joined (first line only)
    bool border = false;               // border_shape != None
    bool no_mouse = false;
    std::string preview_command;
    bool info_hidden = false;
    std::string preview_position = "right";
    int preview_size_percent = 50;
    bool preview_size_is_percent = true;
    bool preview_wrap = false;
    bool preview_hidden = false;
    bool preview_follow = false;
    std::string legacy_delimiter;      // literal delimiter (pre-tokenizer consumers)
    std::map<std::string, std::string> bindings;   // key -> action string
    std::vector<std::string> expect_keys;
};

// Error thrown by the parser; the message is exactly what fzf prints.
struct OptionError {
    std::string message;
};

// Parse the given arguments (without argv[0]) on top of the defaults,
// optionally prefixed by $FZF_DEFAULT_OPTS_FILE and $FZF_DEFAULT_OPTS as fzf
// does. Throws OptionError. Used directly by the tests.
Options parse_option_args(const std::vector<std::string>& args, bool use_defaults);

// Entry point for main(): parses, and on error prints fzf's message to
// stderr and exits with code 2. Also handles --help/--version.
Options parse_options(int argc, char* argv[]);

// Helpers exposed for tests and for other modules.
std::vector<std::string> shell_split_words(const std::string& s);   // fzf: parseShellWords
std::vector<Range> parse_nth(const std::string& spec);              // fzf: splitNth (throws)
Delimiter parse_delimiter(const std::string& spec);                 // fzf: delimiterRegexp
std::vector<std::string> str_lines(const std::string& s);           // fzf: strLines
const char* fzf_compat_version();                                   // "0.74"
const char* fzfpp_version();                                        // "0.2.1"

} // namespace fzf
