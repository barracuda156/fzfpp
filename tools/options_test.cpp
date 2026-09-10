// Unit tests for src/options.cpp (T1.1). Cases mirror fzf's semantics as
// documented in src/options.go and its tests; each block names the rule.
//
// Build: part of the default CMake build; run via ctest or ./options_test.

#include "options.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>

using namespace fzf;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Options parse(std::vector<std::string> args) {
    return parse_option_args(args, false);
}

// Returns the error message, or "" if parsing succeeded.
static std::string parse_error(std::vector<std::string> args) {
    try {
        parse_option_args(args, false);
    } catch (const OptionError& e) {
        return e.message;
    }
    return "";
}

static void test_defaults() {
    Options o = parse({});
    CHECK(o.fuzzy && o.extended && o.case_mode == CaseMode::Smart);
    CHECK(o.algo == AlgoType::FuzzyV2);
    CHECK(o.sort == 1000 && !o.tac && o.multi == 0 && !o.ansi && o.mouse);
    CHECK(o.prompt == "> " && o.tabstop == 8 && o.hscroll_off == 10 && o.scroll_off == 3);
    CHECK(o.layout == LayoutType::Default && o.info_style == InfoStyle::Default);
    CHECK(o.preview.position == WindowPosition::Right && o.preview.size.percent && o.preview.size.size == 50);
    CHECK(o.delimiter.awk);
    // scheme defaults to "path" when stdin is a tty, "default" otherwise
    CHECK(o.criteria.size() == (isatty(STDIN_FILENO) ? 3u : 2u) && o.criteria.back() == Criterion::Length);
    CHECK(*o.pointer == "\xE2\x96\x8C" && *o.marker == "\xE2\x94\x83");
    CHECK(o.border_shape == BorderShape::None && !o.border);
    CHECK(o.bindings.at("ctrl-c") == "abort" && o.bindings.at("tab") == "toggle+down");
}

static void test_every_option_name_is_known() {
    // Every name in fzf 0.74's parseOptions switch. Value-taking options get
    // a plausible value; the parse must not report "unknown option".
    struct Case { const char* name; const char* value; };
    const Case cases[] = {
        {"--no-winpty", nullptr}, {"--tmux", nullptr}, {"--popup", nullptr}, {"--no-tmux", nullptr}, {"--no-popup", nullptr},
        {"--tty-default", "/dev/tty"}, {"--no-tty-default", nullptr}, {"--force-tty-in", nullptr}, {"--no-force-tty-in", nullptr},
        {"--proxy-script", "x"}, {"-x", nullptr}, {"--extended", nullptr}, {"-e", nullptr}, {"--exact", nullptr},
        {"--extended-exact", nullptr}, {"+x", nullptr}, {"--no-extended", nullptr}, {"+e", nullptr}, {"--no-exact", nullptr},
        {"-q", "q"}, {"--query", "q"}, {"-f", "f"}, {"--filter", "f"}, {"--literal", nullptr}, {"--no-literal", nullptr},
        {"--algo", "v1"}, {"--scheme", "path"}, {"--expect", "ctrl-a"}, {"--no-expect", nullptr},
        {"--enabled", nullptr}, {"--no-phony", nullptr}, {"--disabled", nullptr}, {"--phony", nullptr}, {"--no-input", nullptr},
        {"--tiebreak", "index"}, {"--bind", "ctrl-a:accept"}, {"--color", "fg:1"}, {"--toggle-sort", "ctrl-r"},
        {"-d", ":"}, {"--delimiter", ":"}, {"-n", "1"}, {"--nth", "1"}, {"--freeze-left", "1"}, {"--freeze-right", "1"},
        {"--with-nth", "2.."}, {"--accept-nth", "1"}, {"-s", nullptr}, {"--sort", nullptr}, {"+s", nullptr}, {"--no-sort", nullptr},
        {"--raw", nullptr}, {"--no-raw", nullptr}, {"--track", nullptr}, {"--no-track", nullptr}, {"--id-nth", "1"}, {"--no-id-nth", nullptr},
        {"--tac", nullptr}, {"--no-tac", nullptr}, {"--tail", "5"}, {"--no-tail", nullptr}, {"--smart-case", nullptr},
        {"-i", nullptr}, {"--ignore-case", nullptr}, {"+i", nullptr}, {"--no-ignore-case", nullptr},
        {"-m", nullptr}, {"--multi", nullptr}, {"+m", nullptr}, {"--no-multi", nullptr}, {"--ansi", nullptr}, {"--no-ansi", nullptr},
        {"--no-mouse", nullptr}, {"+c", nullptr}, {"--no-color", nullptr}, {"+2", nullptr}, {"--no-256", nullptr},
        {"--black", nullptr}, {"--no-black", nullptr}, {"--bold", nullptr}, {"--no-bold", nullptr},
        {"--layout", "reverse"}, {"--reverse", nullptr}, {"--no-reverse", nullptr}, {"--cycle", nullptr},
        {"--highlight-line", nullptr}, {"--no-highlight-line", nullptr}, {"--no-cycle", nullptr},
        {"--wrap", nullptr}, {"--no-wrap", nullptr}, {"--wrap-word", nullptr}, {"--no-wrap-word", nullptr}, {"--wrap-sign", "x"},
        {"--multi-line", nullptr}, {"--no-multi-line", nullptr}, {"--keep-right", nullptr}, {"--no-keep-right", nullptr},
        {"--hscroll", nullptr}, {"--no-hscroll", nullptr}, {"--hscroll-off", "5"}, {"--scroll-off", "2"},
        {"--filepath-word", nullptr}, {"--no-filepath-word", nullptr}, {"--info", "inline"}, {"--info-command", "echo"},
        {"--no-info-command", nullptr}, {"--no-info", nullptr}, {"--inline-info", nullptr}, {"--no-inline-info", nullptr},
        {"--separator", "-"}, {"--no-separator", nullptr}, {"--ghost", "g"}, {"--scrollbar", nullptr}, {"--no-scrollbar", nullptr},
        {"--jump-labels", "abc"}, {"-1", nullptr}, {"--select-1", nullptr}, {"+1", nullptr}, {"--no-select-1", nullptr},
        {"-0", nullptr}, {"--exit-0", nullptr}, {"+0", nullptr}, {"--no-exit-0", nullptr}, {"--read0", nullptr}, {"--no-read0", nullptr},
        {"--print0", nullptr}, {"--no-print0", nullptr}, {"--print-query", nullptr}, {"--no-print-query", nullptr},
        {"--prompt", "p"}, {"--gutter", "x"}, {"--gutter-raw", "x"}, {"--pointer", "x"}, {"--marker", "x"}, {"--marker-multi-line", "abc"},
        {"--sync", nullptr}, {"--no-sync", nullptr}, {"--async", nullptr}, {"--no-history", nullptr}, {"--history", "/tmp/h"},
        {"--history-size", "10"}, {"--no-header", nullptr}, {"--no-header-lines", nullptr}, {"--header", "h"}, {"--header-lines", "1"},
        {"--no-footer", nullptr}, {"--footer", "f"}, {"--header-first", nullptr}, {"--no-header-first", nullptr},
        {"--gap", nullptr}, {"--no-gap", nullptr}, {"--gap-line", nullptr}, {"--no-gap-line", nullptr}, {"--ellipsis", ".."},
        {"--preview", "cat {}"}, {"--no-preview", nullptr}, {"--preview-window", "up,40%"}, {"--no-preview-border", nullptr},
        {"--preview-border", nullptr}, {"--preview-wrap-sign", "x"}, {"--height", "40%"}, {"--min-height", "5"}, {"--no-height", nullptr},
        {"--no-margin", nullptr}, {"--no-padding", nullptr}, {"--no-border", nullptr}, {"--border", nullptr},
        {"--list-border", nullptr}, {"--no-list-border", nullptr}, {"--no-list-label", nullptr}, {"--list-label", "l"}, {"--list-label-pos", "2"},
        {"--no-header-border", nullptr}, {"--header-border", nullptr}, {"--no-header-lines-border", nullptr}, {"--header-lines-border", nullptr},
        {"--no-header-label", nullptr}, {"--header-label", "l"}, {"--header-label-pos", "center"},
        {"--no-footer-border", nullptr}, {"--footer-border", nullptr}, {"--no-footer-label", nullptr}, {"--footer-label", "l"}, {"--footer-label-pos", "-1"},
        {"--no-input-border", nullptr}, {"--input-border", nullptr}, {"--no-input-label", nullptr}, {"--input-label", "l"}, {"--input-label-pos", "bottom"},
        {"--no-border-label", nullptr}, {"--border-label", "l"}, {"--border-label-pos", "3:bottom"},
        {"--no-preview-label", nullptr}, {"--preview-label", "l"}, {"--preview-label-pos", "top"},
        {"--style", "minimal"}, {"--no-unicode", nullptr}, {"--unicode", nullptr}, {"--ambidouble", nullptr}, {"--no-ambidouble", nullptr},
        {"--margin", "1,2"}, {"--padding", "1"}, {"--tabstop", "4"}, {"--with-shell", "bash -c"},
        {"--listen", nullptr}, {"--listen-unsafe", nullptr}, {"--no-listen", nullptr}, {"--no-listen-unsafe", nullptr},
        {"--clear", nullptr}, {"--no-clear", nullptr}, {"--walker", "file,dir"}, {"--walker-root", "."}, {"--walker-skip", ".git"},
        {"--threads", "2"}, {"--bench", "3s"}, {"--profile-cpu", "/tmp/c"}, {"--profile-mem", "/tmp/m"},
        {"--profile-block", "/tmp/b"}, {"--profile-mutex", "/tmp/x"}, {"--", nullptr},
    };
    for (const Case& c : cases) {
        std::vector<std::string> args{c.name};
        if (c.value) args.push_back(c.value);
        std::string err = parse_error(args);
        if (!err.empty()) {
            ++failures;
            std::fprintf(stderr, "FAIL option %s: %s\n", c.name, err.c_str());
        }
        ++checks;
    }
}

static void test_unknown_and_positional() {
    CHECK(parse_error({"--bogus"}) == "unknown option: --bogus");
    CHECK(parse_error({"stray"}) == "unknown option: stray");
    CHECK(parse_error({"--query"}) == "query string required");
    CHECK(parse_error({"--height", "x"}) == "not a valid integer: x");
    CHECK(parse_error({"--ansi=1"}) == "unexpected value for --ansi: 1");
    CHECK(parse_error({"--layout", "sideways"}).rfind("invalid layout", 0) == 0);
    CHECK(parse_error({"--tiebreak", "index,length"}) == "index should be the last criterion");
    CHECK(parse_error({"--tiebreak", "length,length"}) == "duplicate sort criteria: length");
    CHECK(parse_error({"--scheme", "x"}).rfind("invalid scoring scheme", 0) == 0);
    CHECK(parse_error({"--algo", "v3"}) == "invalid algorithm (expected: v1 or v2)");
    CHECK(parse_error({"--nth", "a"}) == "invalid format: a");
    CHECK(parse_error({"--nth", "0"}) == "invalid format: 0");
    CHECK(parse_error({"--margin", "1,2,3,4,5"}) == "invalid margin: 1,2,3,4,5");
    CHECK(parse_error({"--margin", "50%"}) == "margin too large (max: 49%)");
    CHECK(parse_error({"--tabstop", "0"}) == "tab stop must be a positive integer");
    CHECK(parse_error({"--header-lines", "-1"}) == "header lines must be a non-negative integer");
    CHECK(parse_error({"--pointer", "abc"}) == "pointer display width should be up to 2");
    CHECK(parse_error({"--border", "fancy"}).rfind("invalid border style", 0) == 0);
    CHECK(parse_error({"--preview-window", "sideways"}) == "invalid preview window option: sideways");
    CHECK(parse_error({"--color", "fg:nonsense"}) == "invalid color specification: fg:nonsense");
    CHECK(parse_error({"--color", "nth:red"}).rfind("only ANSI attributes are allowed for 'nth'", 0) == 0);
    CHECK(parse_error({"--height", "~40%", "--margin", "10%"}) == "adaptive height is not compatible with top/bottom percent margin");
    CHECK(parse_error({"--wrap", "sideways"}) == "invalid wrap mode: sideways (expected: char or word)");
    CHECK(parse_error({"--min-height", "-3"}) == "minimum height must be a non-negative integer");
    CHECK(parse_error({"--walker", "x"}) == "invalid walker option: x");
    CHECK(parse_error({"--walker", "hidden"}) == "at least one of 'file' or 'dir' should be specified");
    CHECK(parse_error({"--style", "fancy"}) == "unsupported style preset: fancy");
    CHECK(parse_error({"--tail", "0"}) == "number of items to keep must be a positive integer");
    CHECK(parse_error({"--history-size", "0"}) == "history max must be a positive integer");
    CHECK(parse_error({"--list-border", "line"}) == "list border cannot be 'line'");
}

static void test_value_forms() {
    CHECK(parse({"--query=foo"}).query == "foo");
    CHECK(parse({"--query", "foo"}).query == "foo");
    CHECK(parse({"-q", "foo"}).query == "foo");
    CHECK(parse({"-qfoo"}).query == "foo");
    CHECK(parse({"-ffoo"}).filter && *parse({"-ffoo"}).filter == "foo");
    CHECK(parse({"-f", ""}).filter && parse({"-f", ""}).filter->empty());
    CHECK(parse({"--filter="}).filter && parse({"--filter="}).filter->empty());
    CHECK(parse({"-m3"}).multi == 3);
    CHECK(parse({"-m", "3"}).multi == 3);
    CHECK(parse({"--multi=3"}).multi == 3);
    CHECK(parse({"--multi"}).multi > 1000000);
    CHECK(parse({"-m", "-q", "x"}).multi > 1000000);            // -q is not a count
    CHECK(parse({"-s"}).sort == 1 && parse({"--sort"}).sort == 1);
    CHECK(parse({"+s"}).sort == 0 && parse({"--no-sort"}).sort == 0);
    CHECK(parse({"--sort=500"}).sort == 500);
    CHECK(parse({"-d", ":"}).delimiter.pattern == ":" && !parse({"-d", ":"}).delimiter.awk);
    CHECK(parse({"-d:"}).delimiter.pattern == ":");
    CHECK((parse({"-n2"}).nth.size() == 1 && parse({"-n2"}).nth[0] == Range{2, 2}));
    CHECK(parse({"--border-label="}).border_label.label.empty());
    CHECK(parse({"--border-label=", "-f", "query"}).filter.has_value());   // empty value must not swallow -f
}

static void test_optional_values() {
    Options o = parse({"--border"});
    CHECK(o.border_shape == BorderShape::Rounded && o.border);
    CHECK(parse({"--border=sharp"}).border_shape == BorderShape::Sharp);
    CHECK(parse({"--border", "sharp"}).border_shape == BorderShape::Sharp);
    CHECK(parse({"--border", "--reverse"}).border_shape == BorderShape::Rounded);   // next arg is an option
    CHECK(parse({"--border", "--border=rounded"}).border_shape == BorderShape::Rounded);
    CHECK(parse({"--border", "+m"}).border_shape == BorderShape::Rounded && parse({"--border", "+m"}).multi == 0);
    CHECK(parse({"--no-border"}).border_shape == BorderShape::None);
    CHECK(parse({"--border=none"}).border_shape == BorderShape::None && !parse({"--border=none"}).border);
    CHECK(parse({"--scrollbar"}).scrollbar == std::nullopt);
    CHECK(*parse({"--scrollbar", "#"}).scrollbar == "#");
    CHECK(*parse({"--no-scrollbar"}).scrollbar == "");
    CHECK(parse({"--wrap"}).wrap && !parse({"--wrap"}).wrap_word);
    CHECK(parse({"--wrap", "word"}).wrap_word);
    CHECK(parse({"--color"}).theme.base == ThemeBase::Empty);
    CHECK(parse({"--gap"}).gap == 1 && parse({"--gap", "2"}).gap == 2);
    CHECK(parse({"--tmux"}).tmux.has_value() && parse({"--tmux", "center,80%"}).tmux->width.size == 80);
    CHECK(parse({"--listen"}).listen_addr.has_value() && *parse({"--listen", "6266"}).listen_addr == "6266");
}

static void test_precedence() {
    CHECK(parse({"-i", "+i"}).case_mode == CaseMode::Respect);
    CHECK(parse({"+i", "-i"}).case_mode == CaseMode::Ignore);
    CHECK(parse_error({"--case", "smart"}) == "unknown option: --case");
    CHECK(parse({"--smart-case", "-i", "+i", "--smart-case"}).case_mode == CaseMode::Smart);
    CHECK(parse({"--prompt", "a", "--prompt", "b"}).prompt == "b");
    CHECK(parse({"--reverse", "--layout=default"}).layout == LayoutType::Default);
    CHECK(parse({"--layout=reverse-list", "--reverse"}).layout == LayoutType::Reverse);
    CHECK(parse({"--header", "a", "--header", "b"}).header == std::vector<std::string>{"b"});   // last wins
    CHECK(parse({"--header", "a\nb\n"}).header == (std::vector<std::string>{"a", "b"}));
    CHECK(parse({"-m", "+m"}).multi == 0);
    CHECK(parse({"--exact", "+e"}).fuzzy);
    CHECK(parse({"-e"}).fuzzy == false);
    CHECK(parse({"--disabled", "--enabled"}).phony == false);
    CHECK(parse({"--phony"}).phony && parse({"--phony"}).disabled);
    CHECK(parse({"--info=hidden", "--info=inline"}).info_style == InfoStyle::Inline);
    CHECK(parse({"--info=inline"}).info_prefix == " < ");
    CHECK(parse({"--info=inline:>> "}).info_prefix == ">> ");
    CHECK(parse({"--no-info"}).info_hidden);
}

static void test_accumulating_lists() {
    Options o = parse({"--bind", "ctrl-a:accept", "--bind", "ctrl-b:abort"});
    CHECK(o.bind_specs.size() == 2);
    CHECK(o.bindings.at("ctrl-a") == "accept" && o.bindings.at("ctrl-b") == "abort");
    o = parse({"--expect", "ctrl-a,ctrl-b", "--expect", "f1"});
    CHECK(o.expect_specs.size() == 2 && o.expect_keys.size() == 3 && o.expect_keys[2] == "f1");
    o = parse({"--expect", "ctrl-a", "--no-expect"});
    CHECK(o.expect_specs.empty() && o.expect_keys.empty());
    o = parse({"--color", "fg:1", "--color", "bg:2,hl:3:bold"});
    CHECK(o.theme.slot(ThemeSlot::Fg).color == 1);
    CHECK(o.theme.slot(ThemeSlot::Bg).color == 2);
    CHECK(o.theme.slot(ThemeSlot::Match).color == 3 && (o.theme.slot(ThemeSlot::Match).attr & kAttrBold));
    o = parse({"--color", "fg:1", "--color", "dark,bg:2"});
    CHECK(o.theme.base == ThemeBase::Dark256 && !o.theme.slot(ThemeSlot::Fg).is_color_defined());
    CHECK(o.theme.slot(ThemeSlot::Bg).color == 2);
    o = parse({"--color", "fg+:#ff0000,bg+:-1,pointer:bright-red:underline"});
    CHECK(color_is_24bit(o.theme.slot(ThemeSlot::Current).color));
    CHECK(o.theme.slot(ThemeSlot::DarkBg).color == kColorDefault);
    CHECK(o.theme.slot(ThemeSlot::Pointer).color == 9 && (o.theme.slot(ThemeSlot::Pointer).attr & kAttrUnderline));
    CHECK(parse({"--color", "bw"}).theme.base == ThemeBase::NoColor);
    CHECK(parse({"+c"}).theme.base == ThemeBase::NoColor);
    CHECK(parse({"--color", "hl:regular:bold"}).theme.slot(ThemeSlot::Match).attr == (kAttrRegular | kAttrBold));
}

static void test_height() {
    Options o = parse({"--height", "40%"});
    CHECK(o.height.percent && o.height.size == 40 && !o.height.auto_);
    CHECK(o.legacy_height == 40 && o.height_is_percent);
    o = parse({"--height", "~40%"});
    CHECK(o.height.auto_ && o.height.percent && o.height.size == 40);
    o = parse({"--height", "-3"});
    CHECK(o.height.inverse && !o.height.percent && o.height.size == 3);
    o = parse({"--height", "20"});
    CHECK(!o.height.percent && o.height.size == 20 && o.legacy_height == 20 && !o.height_is_percent);
    o = parse({"--height", "40%", "--no-height"});
    CHECK(!o.height.is_set() && o.legacy_height == 0);
    CHECK(parse_error({"--height", "101%"}) == "height too large (max: 100%)");
    CHECK(parse({"--min-height", "5+"}).min_height == -5);
    CHECK(parse({"--min-height", "5"}).min_height == 5);
}

static void test_preview_window() {
    Options o = parse({"--preview-window", "up:60%:wrap"});
    CHECK(o.preview.position == WindowPosition::Up && o.preview.size.size == 60 && o.preview.wrap);
    CHECK(o.preview_position == "up" && o.preview_size_percent == 60 && o.preview_wrap);
    o = parse({"--preview-window", "border-rounded,left,35%,wrap,hidden,follow,~3,+{2}-5"});
    CHECK(o.preview.position == WindowPosition::Left && o.preview.size.size == 35 && o.preview.hidden);
    CHECK(o.preview.follow && o.preview.header_lines == 3 && o.preview.scroll == "+{2}-5");
    CHECK(o.preview.border == BorderShape::Rounded);
    o = parse({"--preview-window", "down,10"});
    CHECK(o.preview.position == WindowPosition::Down && !o.preview.size.percent && o.preview.size.size == 10);
    CHECK(!o.preview_size_is_percent && o.preview_size_percent == 10);
    o = parse({"--preview-window", "right,50%,<70(up,40%)"});
    CHECK(o.preview.threshold == 70 && o.preview.alternative && o.preview.alternative->position == WindowPosition::Up);
    o = parse({"--preview-window", "hidden", "--preview-window", "nohidden,default"});
    CHECK(!o.preview.hidden);
    o = parse({"--preview", "cat {}", "--preview-window", "noborder"});
    CHECK(o.preview.border == BorderShape::None && o.preview_command == "cat {}");
    o = parse({"--preview-border", "sharp"});
    CHECK(o.preview.border == BorderShape::Sharp);
}

static void test_margin_padding() {
    Options o = parse({"--margin", "1"});
    CHECK(o.margin[0].size == 1 && o.margin[1].size == 1 && o.margin[2].size == 1 && o.margin[3].size == 1);
    o = parse({"--margin", "1,2"});
    CHECK(o.margin[0].size == 1 && o.margin[1].size == 2 && o.margin[2].size == 1 && o.margin[3].size == 2);
    o = parse({"--margin", "1,2,3"});
    CHECK(o.margin[0].size == 1 && o.margin[1].size == 2 && o.margin[2].size == 3 && o.margin[3].size == 2);
    o = parse({"--margin", "1,2,3,4"});
    CHECK(o.margin[0].size == 1 && o.margin[1].size == 2 && o.margin[2].size == 3 && o.margin[3].size == 4);
    o = parse({"--padding", "10%,5"});
    CHECK(o.padding[0].percent && o.padding[0].size == 10 && !o.padding[1].percent && o.padding[1].size == 5);
    o = parse({"--margin", "3", "--no-margin"});
    CHECK(o.margin[0].size == 0);
}

static void test_nth() {
    auto r = parse_nth("1");
    CHECK((r.size() == 1 && r[0] == Range{1, 1}));
    r = parse_nth("2..");
    CHECK((r.size() == 1 && r[0] == Range{2, kRangeEllipsis}));
    r = parse_nth("..3");
    CHECK((r.size() == 1 && r[0] == Range{kRangeEllipsis, 3}));
    r = parse_nth("1..3");
    CHECK((r.size() == 1 && r[0] == Range{kRangeEllipsis, 3}));   // fzf collapses a leading 1
    r = parse_nth("-1");
    CHECK((r.size() == 1 && r[0] == Range{-1, kRangeEllipsis}));   // -1 == last, end collapses
    r = parse_nth("..");
    CHECK(r.size() == 1 && r[0].is_full());
    r = parse_nth("2,-2..-1,4..5");
    CHECK((r.size() == 3 && r[0] == Range{2, 2} && r[1] == Range{-2, kRangeEllipsis} && r[2] == Range{4, 5}));
    CHECK(parse({"--nth", ".."}).nth.empty());        // full range removed in post-processing
    CHECK(parse({"--nth", "..,2"}).nth.size() == 2);   // not when there are several ranges
    Options o = parse({"--with-nth", "2..", "--accept-nth", "-1", "-d", ","});
    CHECK(o.with_nth_expr == "2.." && o.accept_nth_expr == "-1");
    CHECK(o.with_nth.size() == 1 && o.with_nth[0].begin == 2 && o.with_nth[0].open_end);
    CHECK(o.accept_nth.size() == 1 && o.accept_nth[0].begin == -1 && o.accept_nth[0].open_end);
    CHECK(parse({"--with-nth", "{1} {2}"}).with_nth_expr == "{1} {2}");   // template form (T1.3)
    CHECK(parse_error({"--with-nth", "abc"}).rfind("template should include", 0) == 0);
}

static void test_delimiter() {
    Delimiter d = parse_delimiter(":");
    CHECK(!d.awk && !d.is_regex && d.pattern == ":");
    d = parse_delimiter("\\t");
    CHECK(!d.is_regex && d.pattern == "\t");
    d = parse_delimiter("::");
    CHECK(!d.is_regex && d.pattern == "::");
    d = parse_delimiter("[: ]");
    CHECK(d.is_regex && d.pattern == "[: ]");
    d = parse_delimiter("\\s+");
    CHECK(d.is_regex);
    d = parse_delimiter("[");
    CHECK(!d.is_regex && d.pattern == "[");            // single character
    d = parse_delimiter("[[");
    CHECK(!d.is_regex && d.pattern == "[[");           // invalid regex -> literal
    Options o = parse({"-d", "\\t"});
    CHECK(o.legacy_delimiter == "\t");
    o = parse({"-d", "[: ]"});
    CHECK(o.legacy_delimiter == "[: ]");                // legacy consumers see the raw pattern
}

static void test_shell_words() {
    auto w = shell_split_words("--height 40% --border 'a b' \"c d\" e\\ f");
    CHECK(w.size() == 6 && w[3] == "a b" && w[4] == "c d" && w[5] == "e f");
    w = shell_split_words("--reverse # a comment\n--cycle");
    CHECK(w.size() == 2 && w[1] == "--cycle");
    w = shell_split_words("  --prompt '> '  ");
    CHECK(w.size() == 2 && w[1] == "> ");
    w = shell_split_words("--bind 'ctrl-a:execute(echo \"hi\")'");
    CHECK(w.size() == 2 && w[1] == "ctrl-a:execute(echo \"hi\")");
    bool threw = false;
    try { shell_split_words("--prompt 'unterminated"); } catch (const OptionError&) { threw = true; }
    CHECK(threw);
}

static void test_default_opts_env() {
    setenv("FZF_DEFAULT_OPTS", "--prompt 'env> ' --reverse --border", 1);
    Options o = parse_option_args({"--prompt", "cli> "}, true);
    CHECK(o.prompt == "cli> " && o.layout == LayoutType::Reverse && o.border);
    setenv("FZF_DEFAULT_OPTS", "--bogus", 1);
    std::string err;
    try { parse_option_args({}, true); } catch (const OptionError& e) { err = e.message; }
    CHECK(err == "$FZF_DEFAULT_OPTS: unknown option: --bogus");
    unsetenv("FZF_DEFAULT_OPTS");

    // Height index ordering across DEFAULT_OPTS and argv (used by --tmux checks)
    setenv("FZF_DEFAULT_OPTS", "--height 40%", 1);
    o = parse_option_args({"--tmux"}, true);
    CHECK(o.height.index == 0 && o.tmux->index == 2);
    unsetenv("FZF_DEFAULT_OPTS");

    const char* path = "/tmp/fzfpp_options_test_defaults";
    FILE* f = std::fopen(path, "w");
    std::fputs("# defaults\n--cycle\n--info=inline   # trailing comment\n", f);
    std::fclose(f);
    setenv("FZF_DEFAULT_OPTS_FILE", path, 1);
    o = parse_option_args({}, true);
    CHECK(o.cycle && o.info_style == InfoStyle::Inline);
    unsetenv("FZF_DEFAULT_OPTS_FILE");
    std::remove(path);
}

static void test_legacy_bind_split() {
    Options o = parse({"--bind", "ctrl-/:toggle-preview,ctrl-space:toggle-wrap+toggle-preview-wrap"});
    CHECK(o.bindings.at("ctrl-/") == "toggle-preview");
    CHECK(o.bindings.at("ctrl-space") == "toggle-wrap+toggle-preview-wrap");
    o = parse({"--bind", "focus:transform-header:case $a in a,b) echo x;; esac"});
    CHECK(o.bindings.at("focus") == "transform-header:case $a in a,b) echo x;; esac");
    o = parse({"--bind", "Ctrl-A:accept,ALT-B:abort,alt-b:up"});
    CHECK(o.bindings.at("ctrl-a") == "accept" && o.bindings.at("alt-B") == "abort" && o.bindings.at("alt-b") == "up");
    o = parse({"--bind", "ctrl-c:ignore"});
    CHECK(o.bindings.at("ctrl-c") == "ignore");         // user binding beats the default abort
    o = parse({"--toggle-sort", "ctrl-r"});
    CHECK(o.bindings.at("ctrl-r") == "toggle-sort");
}

static void test_misc() {
    Options o = parse({"--walker-skip", ".git,,node_modules,"});
    CHECK(o.walker_skip == (std::vector<std::string>{".git", "node_modules"}));
    o = parse({"--walker", "dir,follow"});
    CHECK(!o.walker.file && o.walker.dir && o.walker.follow && !o.walker.hidden);
    o = parse({"--walker-root", "/tmp", "/", "--cycle"});
    CHECK(o.walker_root.size() == 2 && o.cycle);
    o = parse({"--no-unicode"});
    CHECK(*o.pointer == ">" && *o.marker == ">");
    o = parse({"--marker", ""});
    CHECK(o.marker->empty() && (*o.marker_multi)[0].empty());
    o = parse({"--pointer", "=>", "--marker", "*"});
    CHECK(*o.pointer == "=>" && *o.marker == "*");
    o = parse({"--tiebreak", "begin,end,index"});
    CHECK(o.criteria.size() == 3 && o.criteria[1] == Criterion::Begin && o.criteria[2] == Criterion::End);
    o = parse({"--scheme", "path"});
    CHECK(o.scheme == "path" && o.criteria.size() == 3 && o.criteria[1] == Criterion::Pathname);
    o = parse({"--scheme", "history"});
    CHECK(o.criteria.size() == 1);
    o = parse({"--print0", "--read0", "--tac", "--sync", "--header-lines", "2", "--literal"});
    CHECK(o.print0 && o.read_zero && o.tac && o.sync && o.header_lines == 2 && !o.normalize);
    o = parse({"--border-label-pos", "-3:bottom"});
    CHECK(o.border_label.column == -3 && o.border_label.bottom);
    o = parse({"--style", "full:sharp"});
    CHECK(o.input_border == BorderShape::Sharp && o.info_style == InfoStyle::InlineRight);
    o = parse({"--marker-multi-line", "abcdef"});
    CHECK((*o.marker_multi)[0] == "ab" && (*o.marker_multi)[2] == "ef");
    o = parse({"--help", "--version"});
    CHECK(o.version && !o.help);
    o = parse({"--version", "--help"});
    CHECK(o.help && !o.version);
}

int main() {
    test_defaults();
    test_every_option_name_is_known();
    test_unknown_and_positional();
    test_value_forms();
    test_optional_values();
    test_precedence();
    test_accumulating_lists();
    test_height();
    test_preview_window();
    test_margin_padding();
    test_nth();
    test_delimiter();
    test_shell_words();
    test_default_opts_env();
    test_legacy_bind_split();
    test_misc();
    std::printf("options_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
