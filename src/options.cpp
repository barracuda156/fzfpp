// Hand-written port of fzf's option parser. See options.hpp for the policy.
// Function-level comments cite the Go original as `fzf: file func`.

#include "options.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <sys/stat.h>
#include <unistd.h>
#include <utf8.h>

namespace fzf {

namespace {

constexpr const char* kCompatVersion = "0.74";
constexpr const char* kPortVersion = "0.2.1";
constexpr int kMaxMulti = INT_MAX;
constexpr const char* kDefaultInfoPrefix = " < ";
constexpr BorderShape kDefaultBorderShape = BorderShape::Rounded;

[[noreturn]] void fail(const std::string& message) {
    throw OptionError{message};
}

// fzf: options.go atoi (strconv.Atoi: optional sign, decimal digits only)
bool strict_atoi(const std::string& s, int& out) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    if (i >= s.size()) return false;
    for (size_t j = i; j < s.size(); ++j) {
        if (!std::isdigit(static_cast<unsigned char>(s[j]))) return false;
    }
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno == ERANGE || v > INT_MAX || v < INT_MIN) return false;
    out = static_cast<int>(v);
    return true;
}

int atoi_or_fail(const std::string& s) {
    int v;
    if (!strict_atoi(s, v)) fail("not a valid integer: " + s);
    return v;
}

double atof_or_fail(const std::string& s) {
    if (s.empty()) fail("not a valid number: " + s);
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE) {
        fail("not a valid number: " + s);
    }
    return v;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has_prefix(const std::string& s, const char* p) {
    return s.compare(0, std::strlen(p), p) == 0;
}

bool has_suffix(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

// Approximate display width used only for option validation (fzf uses
// uniseg.StringWidth): one column per codepoint, zero for combining marks
// and variation selectors. Good enough to reject obviously wide signs.
int approx_width(const std::string& s) {
    int w = 0;
    try {
        auto it = s.begin();
        while (it != s.end()) {
            char32_t cp = utf8::next(it, s.end());
            if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) continue;
            if ((cp >= 0x300 && cp <= 0x36f) || (cp >= 0xfe00 && cp <= 0xfe0f) ||
                (cp >= 0x200b && cp <= 0x200f)) continue;
            w += 1;
        }
    } catch (...) {
        return static_cast<int>(s.size());
    }
    return w;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

// Split on any run of the characters in `seps` (fzf: splitRegexp "[,:]+"),
// like Go's regexp Split which yields empty leading/trailing fields.
std::vector<std::string> split_on_any(const std::string& s, const char* seps) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
        if (std::strchr(seps, s[i])) {
            out.push_back(cur);
            cur.clear();
            while (i < s.size() && std::strchr(seps, s[i])) ++i;
        } else {
            cur += s[i++];
        }
    }
    out.push_back(cur);
    return out;
}

bool is_dir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// fzf: options.go firstLine
std::string first_line(const std::string& s) {
    size_t p = s.find('\n');
    return p == std::string::npos ? s : s.substr(0, p);
}

// ---------------------------------------------------------------------------
// Value parsers
// ---------------------------------------------------------------------------

// fzf: options.go parseSize
SizeSpec parse_size(const std::string& str, double max_percent, const std::string& label) {
    SizeSpec spec;
    if (has_suffix(str, "%")) {
        double val = atof_or_fail(str.substr(0, str.size() - 1));
        if (val < 0) fail(label + " must be non-negative");
        if (val > max_percent) {
            fail(label + " too large (max: " + std::to_string(static_cast<int>(max_percent)) + "%)");
        }
        spec.size = val;
        spec.percent = true;
        return spec;
    }
    if (str.find('.') != std::string::npos) {
        fail(label + " (without %) must be a non-negative integer");
    }
    int i = atoi_or_fail(str);
    if (i < 0) fail(label + " must be non-negative");
    spec.size = i;
    spec.percent = false;
    return spec;
}

// fzf: options.go parseHeight
HeightSpec parse_height(std::string str, int index) {
    HeightSpec h;
    h.index = index;
    if (has_prefix(str, "~")) { h.auto_ = true; str.erase(0, 1); }
    if (has_prefix(str, "-")) { h.inverse = true; str.erase(0, 1); }
    SizeSpec size = parse_size(str, 100, "height");
    h.size = size.size;
    h.percent = size.percent;
    return h;
}

// fzf: options.go parseMargin
std::array<SizeSpec, 4> parse_margin(const std::string& opt, const std::string& margin) {
    auto parts = split(margin, ',');
    auto checked = [&](const std::string& s) { return parse_size(s, 49, opt); };
    switch (parts.size()) {
        case 1: { auto m = checked(parts[0]); return {m, m, m, m}; }
        case 2: { auto tb = checked(parts[0]); auto rl = checked(parts[1]); return {tb, rl, tb, rl}; }
        case 3: { auto t = checked(parts[0]); auto rl = checked(parts[1]); auto b = checked(parts[2]); return {t, rl, b, rl}; }
        case 4: { auto t = checked(parts[0]); auto r = checked(parts[1]); auto b = checked(parts[2]); auto l = checked(parts[3]); return {t, r, b, l}; }
        default: break;
    }
    fail("invalid " + opt + ": " + margin);
}

// fzf: options.go parseBorder
BorderShape parse_border(const std::string& str, bool optional) {
    static const std::unordered_map<std::string, BorderShape> shapes = {
        {"line", BorderShape::Line}, {"inline", BorderShape::Inline},
        {"rounded", BorderShape::Rounded}, {"sharp", BorderShape::Sharp},
        {"bold", BorderShape::Bold}, {"block", BorderShape::Block},
        {"thinblock", BorderShape::ThinBlock}, {"double", BorderShape::Double},
        {"dashed", BorderShape::Dashed}, {"horizontal", BorderShape::Horizontal},
        {"vertical", BorderShape::Vertical}, {"top", BorderShape::Top},
        {"bottom", BorderShape::Bottom}, {"left", BorderShape::Left},
        {"right", BorderShape::Right}, {"none", BorderShape::None},
    };
    auto it = shapes.find(str);
    if (it != shapes.end()) return it->second;
    if (optional && str.empty()) return kDefaultBorderShape;
    fail("invalid border style (expected: rounded|sharp|bold|block|thinblock|double|dashed|horizontal|vertical|top|bottom|left|right|line|inline|none)");
}

// fzf: options.go parseLayout
LayoutType parse_layout(const std::string& str) {
    if (str == "default") return LayoutType::Default;
    if (str == "reverse") return LayoutType::Reverse;
    if (str == "reverse-list") return LayoutType::ReverseList;
    fail("invalid layout (expected: default / reverse / reverse-list)");
}

// fzf: options.go parseInfoStyle
void parse_info_style(const std::string& str, InfoStyle& style, std::string& prefix) {
    if (str == "default") { style = InfoStyle::Default; prefix.clear(); return; }
    if (str == "right") { style = InfoStyle::Right; prefix.clear(); return; }
    if (str == "inline") { style = InfoStyle::Inline; prefix = kDefaultInfoPrefix; return; }
    if (str == "inline-right") { style = InfoStyle::InlineRight; prefix.clear(); return; }
    if (str == "hidden") { style = InfoStyle::Hidden; prefix.clear(); return; }
    struct Spec { const char* name; InfoStyle style; };
    for (const Spec& spec : {Spec{"inline", InfoStyle::Inline}, Spec{"inline-right", InfoStyle::InlineRight}}) {
        std::string p = std::string(spec.name) + ":";
        if (has_prefix(str, p.c_str())) {
            style = spec.style;
            prefix = str.substr(p.size());
            std::replace(prefix.begin(), prefix.end(), '\n', ' ');
            return;
        }
    }
    fail("invalid info style (expected: default|right|hidden|inline[-right][:PREFIX])");
}

// fzf: options.go parseTiebreak
std::vector<Criterion> parse_tiebreak(const std::string& str) {
    std::vector<Criterion> criteria{Criterion::Score};
    bool has_index = false, has_chunk = false, has_length = false,
         has_begin = false, has_end = false, has_pathname = false;
    auto check = [&](bool& not_expected, const char* name) {
        if (not_expected) fail(std::string("duplicate sort criteria: ") + name);
        if (has_index) fail("index should be the last criterion");
        not_expected = true;
    };
    for (const auto& s : split(to_lower(str), ',')) {
        if (s == "index") { check(has_index, "index"); }
        else if (s == "chunk") { check(has_chunk, "chunk"); criteria.push_back(Criterion::Chunk); }
        else if (s == "pathname") { check(has_pathname, "pathname"); criteria.push_back(Criterion::Pathname); }
        else if (s == "length") { check(has_length, "length"); criteria.push_back(Criterion::Length); }
        else if (s == "begin") { check(has_begin, "begin"); criteria.push_back(Criterion::Begin); }
        else if (s == "end") { check(has_end, "end"); criteria.push_back(Criterion::End); }
        else fail("invalid sort criterion: " + s);
    }
    if (criteria.size() > 4) fail("at most 3 tiebreaks are allowed: " + str);
    return criteria;
}

// fzf: options.go parseScheme
std::vector<Criterion> parse_scheme(std::string& str) {
    str = to_lower(str);
    if (str == "history") return {Criterion::Score};
    if (str == "path") return {Criterion::Score, Criterion::Pathname, Criterion::Length};
    if (str == "default") return {Criterion::Score, Criterion::Length};
    fail("invalid scoring scheme: " + str + " (expected: default|path|history)");
}

// fzf: options.go parseAlgo
AlgoType parse_algo(const std::string& str) {
    if (str == "v1") return AlgoType::FuzzyV1;
    if (str == "v2") return AlgoType::FuzzyV2;
    fail("invalid algorithm (expected: v1 or v2)");
}

// fzf: options.go parseWalkerOpts
WalkerOpts parse_walker_opts(const std::string& str) {
    WalkerOpts opts{false, false, false, false};
    for (const auto& s : split(to_lower(str), ',')) {
        if (s == "file") opts.file = true;
        else if (s == "dir") opts.dir = true;
        else if (s == "hidden") opts.hidden = true;
        else if (s == "follow") opts.follow = true;
        else if (s.empty()) continue;
        else fail("invalid walker option: " + s);
    }
    if (!opts.file && !opts.dir) fail("at least one of 'file' or 'dir' should be specified");
    return opts;
}

// fzf: options.go parseLabelPosition
void parse_label_position(LabelOpts& opts, const std::string& arg) {
    opts.column = 0;
    opts.bottom = false;
    for (const auto& token : split_on_any(to_lower(arg), ",:")) {
        if (token == "center") opts.column = 0;
        else if (token == "bottom") opts.bottom = true;
        else if (token == "top") opts.bottom = false;
        else opts.column = atoi_or_fail(token);
    }
}

// fzf: options.go parseMarkerMultiLine (widths approximated per codepoint)
std::array<std::string, 3> parse_marker_multi_line(const std::string& str) {
    std::array<std::string, 3> result{};
    if (str.empty()) return result;
    std::vector<std::string> parts;
    int total = 0;
    auto it = str.begin();
    while (it != str.end()) {
        auto start = it;
        utf8::next(it, str.end());
        std::string cp(start, it);
        total += approx_width(cp);
        parts.push_back(cp);
    }
    if (total != 3 && total != 6) {
        fail("invalid total marker width: " + std::to_string(total) + " (expected: 0, 3 or 6)");
    }
    int expected = total / 3;
    size_t idx = 0;
    for (const auto& part : parts) {
        expected -= approx_width(part);
        result[idx] += part;
        if (expected <= 0) { idx++; expected = total / 3; }
        if (idx == 3) break;
    }
    return result;
}

// fzf: options.go parsePreviewWindowImpl
PreviewOpts default_preview_opts(const std::string& command) {
    PreviewOpts p;
    p.command = command;
    p.border = kDefaultBorderShape;
    return p;
}

void parse_preview_window(PreviewOpts& opts, const std::string& input) {
    // Tokens are separated by ':' or ','; a `<N(...)` token carries the
    // alternative layout used when the terminal is narrower than N columns.
    static const std::regex token_re(R"([:,]*(<([1-9][0-9]*)\(([^)<]+)\)|[^,:]+))");
    static const std::regex size_re("^[0-9]+%?$");
    static const std::regex offset_re(R"(^(\+\{(-?[0-9]+|n)\})?([+-][0-9]+)*(-?/[1-9][0-9]*)?$)");
    static const std::regex header_re("^~(0|[1-9][0-9]*)$");

    std::string alternative;
    for (auto it = std::sregex_iterator(input.begin(), input.end(), token_re);
         it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        if (m[2].matched && m[2].length() > 0) {
            opts.threshold = atoi_or_fail(m[2].str());
            alternative = m[3].str();
            continue;
        }
        std::string token = m[1].str();
        if (token.empty()) continue;
        if (token == "default") opts = default_preview_opts(opts.command);
        else if (token == "hidden") opts.hidden = true;
        else if (token == "nohidden") opts.hidden = false;
        else if (token == "wrap") { opts.wrap = true; opts.wrap_word = false; }
        else if (token == "wrap-word") { opts.wrap = true; opts.wrap_word = true; }
        else if (token == "nowrap") { opts.wrap = false; opts.wrap_word = false; }
        else if (token == "cycle") opts.cycle = true;
        else if (token == "nocycle") opts.cycle = false;
        else if (token == "up" || token == "top") opts.position = WindowPosition::Up;
        else if (token == "down" || token == "bottom") opts.position = WindowPosition::Down;
        else if (token == "left") opts.position = WindowPosition::Left;
        else if (token == "right") opts.position = WindowPosition::Right;
        else if (token == "next") opts.position = WindowPosition::Next;
        else if (token == "rounded" || token == "border" || token == "border-rounded") opts.border = BorderShape::Rounded;
        else if (token == "border-line") opts.border = BorderShape::Line;
        else if (token == "sharp" || token == "border-sharp") opts.border = BorderShape::Sharp;
        else if (token == "border-bold") opts.border = BorderShape::Bold;
        else if (token == "border-block") opts.border = BorderShape::Block;
        else if (token == "border-thinblock") opts.border = BorderShape::ThinBlock;
        else if (token == "border-double") opts.border = BorderShape::Double;
        else if (token == "border-dashed") opts.border = BorderShape::Dashed;
        else if (token == "noborder" || token == "border-none") opts.border = BorderShape::None;
        else if (token == "border-horizontal") opts.border = BorderShape::Horizontal;
        else if (token == "border-vertical") opts.border = BorderShape::Vertical;
        else if (token == "border-up" || token == "border-top") opts.border = BorderShape::Top;
        else if (token == "border-down" || token == "border-bottom") opts.border = BorderShape::Bottom;
        else if (token == "border-left") opts.border = BorderShape::Left;
        else if (token == "border-right") opts.border = BorderShape::Right;
        else if (token == "follow") opts.follow = true;
        else if (token == "nofollow") opts.follow = false;
        else if (token == "info") opts.info = true;
        else if (token == "noinfo") opts.info = false;
        else if (std::regex_match(token, header_re)) opts.header_lines = atoi_or_fail(token.substr(1));
        else if (std::regex_match(token, size_re)) opts.size = parse_size(token, 99, "window size");
        else if (std::regex_match(token, offset_re)) opts.scroll = token;
        else fail("invalid preview window option: " + token);
    }
    if (!alternative.empty()) {
        auto alt = std::make_shared<PreviewOpts>(opts);
        alt->hidden = false;
        alt->alternative.reset();
        parse_preview_window(*alt, alternative);
        opts.alternative = alt;
    }
}

// fzf: options.go parseTmuxOptions (accepted, validated, no effect)
TmuxOptions parse_tmux_options(const std::string& arg, int index) {
    TmuxOptions opts;
    opts.index = index;
    auto tokens = split_on_any(arg, ",:");
    std::string error = "invalid popup option: " + arg +
        " (expected: [center|top|bottom|left|right][,SIZE[%]][,SIZE[%]][,border-native])";
    if (tokens.empty() || tokens.size() > 4) fail(error);
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "border-native") {
            tokens.erase(tokens.begin() + static_cast<long>(i));
            opts.border = true;
            break;
        }
    }
    std::string first = tokens.empty() ? "center" : tokens[0];
    if (first == "top" || first == "up") { opts.position = WindowPosition::Up; opts.width = {100, true}; }
    else if (first == "bottom" || first == "down") { opts.position = WindowPosition::Down; opts.width = {100, true}; }
    else if (first == "left") { opts.position = WindowPosition::Left; opts.height = {100, true}; }
    else if (first == "right") { opts.position = WindowPosition::Right; opts.height = {100, true}; }
    else if (first == "center") {}
    else tokens.insert(tokens.begin(), "center");

    auto size_or_fail = [&](const std::string& s) {
        try { return parse_size(s, 100, "size"); } catch (const OptionError&) { fail(error); }
    };
    SizeSpec size1, size2;
    if (tokens.size() > 1) size1 = size_or_fail(tokens[1]);
    if (tokens.size() == 3) {
        size2 = size_or_fail(tokens[2]);
        opts.width = size1;
        opts.height = size2;
    } else if (tokens.size() == 2) {
        const std::string& t = tokens[0];
        if (t == "top" || t == "up" || t == "bottom" || t == "down") opts.height = size1;
        else if (t == "left" || t == "right") opts.width = size1;
        else { opts.width = size1; opts.height = size1; }
    }
    return opts;
}

// fzf: options.go parseTheme. Base tokens reset the overrides; every other
// token merges into one slot using fzf's mergeAttr rules.
void parse_theme(Theme& theme, const std::string& spec) {
    static const std::regex rrggbb("^#[0-9a-fA-F]{6}$");
    static const std::unordered_map<std::string, ThemeSlot> slots = {
        {"query", ThemeSlot::Input}, {"input", ThemeSlot::Input}, {"input-fg", ThemeSlot::Input},
        {"ghost", ThemeSlot::Ghost}, {"disabled", ThemeSlot::Disabled},
        {"fg", ThemeSlot::Fg}, {"bg", ThemeSlot::Bg},
        {"list-fg", ThemeSlot::ListFg}, {"list-bg", ThemeSlot::ListBg},
        {"preview-fg", ThemeSlot::PreviewFg}, {"preview-bg", ThemeSlot::PreviewBg},
        {"current-fg", ThemeSlot::Current}, {"fg+", ThemeSlot::Current},
        {"current-bg", ThemeSlot::DarkBg}, {"bg+", ThemeSlot::DarkBg},
        {"alt-bg", ThemeSlot::AltBg},
        {"selected-fg", ThemeSlot::SelectedFg}, {"selected-bg", ThemeSlot::SelectedBg},
        {"nth", ThemeSlot::Nth}, {"nomatch", ThemeSlot::Nomatch},
        {"gutter", ThemeSlot::Gutter}, {"alt-gutter", ThemeSlot::AltGutter},
        {"hl", ThemeSlot::Match}, {"current-hl", ThemeSlot::CurrentMatch}, {"hl+", ThemeSlot::CurrentMatch},
        {"selected-hl", ThemeSlot::SelectedMatch},
        {"border", ThemeSlot::Border}, {"preview-border", ThemeSlot::PreviewBorder},
        {"separator", ThemeSlot::Separator}, {"scrollbar", ThemeSlot::Scrollbar},
        {"preview-scrollbar", ThemeSlot::PreviewScrollbar},
        {"label", ThemeSlot::BorderLabel}, {"list-label", ThemeSlot::ListLabel},
        {"list-border", ThemeSlot::ListBorder}, {"preview-label", ThemeSlot::PreviewLabel},
        {"prompt", ThemeSlot::Prompt}, {"input-bg", ThemeSlot::InputBg},
        {"input-border", ThemeSlot::InputBorder}, {"input-label", ThemeSlot::InputLabel},
        {"header-border", ThemeSlot::HeaderBorder}, {"header-label", ThemeSlot::HeaderLabel},
        {"footer-border", ThemeSlot::FooterBorder}, {"footer-label", ThemeSlot::FooterLabel},
        {"spinner", ThemeSlot::Spinner}, {"info", ThemeSlot::Info},
        {"pointer", ThemeSlot::Pointer}, {"marker", ThemeSlot::Marker},
        {"header", ThemeSlot::Header}, {"header-fg", ThemeSlot::Header}, {"header-bg", ThemeSlot::HeaderBg},
        {"footer", ThemeSlot::Footer}, {"footer-fg", ThemeSlot::Footer}, {"footer-bg", ThemeSlot::FooterBg},
        {"gap-line", ThemeSlot::GapLine},
    };
    static const std::unordered_map<std::string, ColorCode> named_colors = {
        {"black", 0}, {"red", 1}, {"green", 2}, {"yellow", 3}, {"blue", 4},
        {"magenta", 5}, {"cyan", 6}, {"white", 7},
        {"bright-black", 8}, {"gray", 8}, {"grey", 8},
        {"bright-red", 9}, {"bright-green", 10}, {"bright-yellow", 11},
        {"bright-blue", 12}, {"bright-magenta", 13}, {"bright-cyan", 14}, {"bright-white", 15},
    };
    static const std::unordered_map<std::string, Attr> named_attrs = {
        {"regular", kAttrRegular}, {"bold", kAttrBold}, {"strong", kAttrBold},
        {"dim", kAttrDim}, {"strip", kAttrStrip}, {"italic", kAttrItalic},
        {"underline", kAttrUnderline},
        {"underline-double", kAttrUnderline | kUlStyleDouble},
        {"underline-curly", kAttrUnderline | kUlStyleCurly},
        {"underline-dotted", kAttrUnderline | kUlStyleDotted},
        {"underline-dashed", kAttrUnderline | kUlStyleDashed},
        {"blink", kAttrBlink}, {"reverse", kAttrReverse}, {"strikethrough", kAttrStrikeThrough},
    };

    std::string error;
    for (auto& raw : split_on_any(to_lower(spec), " \t\n,")) {
        std::string str = raw;
        // trim
        size_t a = str.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        size_t b = str.find_last_not_of(" \t");
        str = str.substr(a, b - a + 1);
        if (str.empty()) continue;

        if (str == "dark") { theme.reset(ThemeBase::Dark256); theme.explicit_base = ThemeBase::Dark256; continue; }
        if (str == "light") { theme.reset(ThemeBase::Light256); theme.explicit_base = ThemeBase::Light256; continue; }
        if (str == "base16" || str == "16") { theme.reset(ThemeBase::Default16); theme.explicit_base = ThemeBase::Default16; continue; }
        if (str == "bw" || str == "no") { theme.reset(ThemeBase::NoColor); theme.explicit_base = ThemeBase::NoColor; continue; }

        auto components = split(str, ':');
        if (components.size() < 2) { error = "invalid color specification: " + str; continue; }
        auto slot_it = slots.find(components[0]);
        if (slot_it == slots.end()) { error = "invalid color specification: " + str; continue; }
        ColorAttr& cattr = theme.slot(slot_it->second);
        for (size_t i = 1; i < components.size(); ++i) {
            const std::string& c = components[i];
            if (c.empty()) continue;
            auto attr_it = named_attrs.find(c);
            if (attr_it != named_attrs.end()) {
                if (attr_it->second == kAttrRegular) cattr.attr = kAttrRegular;
                else cattr.attr |= attr_it->second;
                continue;
            }
            auto col_it = named_colors.find(c);
            if (col_it != named_colors.end()) { cattr.color = col_it->second; continue; }
            if (std::regex_match(c, rrggbb)) {
                long r = std::strtol(c.substr(1, 2).c_str(), nullptr, 16);
                long g = std::strtol(c.substr(3, 2).c_str(), nullptr, 16);
                long bb = std::strtol(c.substr(5, 2).c_str(), nullptr, 16);
                cattr.color = static_cast<ColorCode>((1 << 24) + (r << 16) + (g << 8) + bb);
                continue;
            }
            int ansi;
            if (!strict_atoi(c, ansi) || ansi < -1 || ansi > 255) {
                error = "invalid color specification: " + str;
                continue;
            }
            cattr.color = ansi;
        }
    }
    if (!error.empty()) fail(error);
}

// fzf: options.go applyPreset (--style). Only the parts that have a
// counterpart here; the rest is accepted.
void apply_preset(Options& opts, const std::string& preset, BorderShape& default_border) {
    default_border = BorderShape::Rounded;
    std::string p = to_lower(preset);
    if (p == "default") {
        opts.list_border = BorderShape::Undefined;
        opts.input_border = BorderShape::Undefined;
        opts.header_border = BorderShape::Undefined;
        opts.footer_border = BorderShape::Undefined;
        opts.preview.border = default_border;
        opts.preview.info = true;
        opts.info_style = InfoStyle::Default;
        opts.theme.slot(ThemeSlot::Gutter) = ColorAttr{};
        opts.separator.reset();
        opts.scrollbar.reset();
        opts.cursor_line = false;
    } else if (p == "minimal") {
        opts.list_border = BorderShape::Undefined;
        opts.input_border = BorderShape::Undefined;
        opts.header_border = BorderShape::Undefined;
        opts.footer_border = BorderShape::Line;
        opts.preview.border = BorderShape::Line;
        opts.preview.info = false;
        opts.info_style = InfoStyle::Default;
        opts.theme.slot(ThemeSlot::Gutter) = ColorAttr{};
        opts.gutter = " ";
        opts.separator = "";
        opts.scrollbar = "";
        opts.cursor_line = false;
    } else {
        auto tokens = split(preset, ':');
        if (tokens[0] != "full") fail("unsupported style preset: " + preset);
        if (tokens.size() >= 2 && !tokens[1].empty()) {
            default_border = parse_border(tokens[1], false);
        }
        if (default_border != BorderShape::Line) opts.list_border = default_border;
        opts.input_border = default_border;
        opts.header_border = default_border;
        opts.footer_border = default_border;
        opts.preview.border = default_border;
        if (default_border == BorderShape::Line) opts.border_shape = default_border;
        opts.preview.info = true;
        opts.info_style = InfoStyle::InlineRight;
        opts.theme.slot(ThemeSlot::Gutter) = ColorAttr{};
        opts.separator.reset();
        opts.scrollbar.reset();
        opts.cursor_line = true;
    }
}

// ---------------------------------------------------------------------------
// Option table
// ---------------------------------------------------------------------------

enum class Opt {
    Man, Bash, Zsh, Fish, Nushell, Help, Version, NoWinpty, Tmux, NoTmux,
    TtyDefault, NoTtyDefault, ForceTtyIn, NoForceTtyIn, ProxyScript,
    Extended, Exact, ExtendedExact, NoExtended, NoExact, Query, Filter,
    Literal, NoLiteral, Algo, Scheme, Expect, NoExpect, Enabled, Disabled,
    NoInput, Tiebreak, Bind, ColorOpt, ToggleSort, Delimiter, Nth, FreezeLeft,
    FreezeRight, WithNth, AcceptNth, Sort, NoSort, Raw, NoRaw, Track, NoTrack,
    IdNth, NoIdNth, Tac, NoTac, Tail, NoTail, SmartCase, IgnoreCase,
    NoIgnoreCase, Multi, NoMulti, Ansi, NoAnsi, NoMouse, NoColor, No256,
    Black, NoBlack, Bold, NoBold, Layout, Reverse, NoReverse, Cycle,
    HighlightLine, NoHighlightLine, NoCycle, Wrap, NoWrap, WrapWord,
    NoWrapWord, WrapSign, MultiLine, NoMultiLine, KeepRight, NoKeepRight,
    Hscroll, NoHscroll, HscrollOff, ScrollOff, FilepathWord, NoFilepathWord,
    Info, InfoCommand, NoInfoCommand, NoInfo, InlineInfo, NoInlineInfo,
    Separator, NoSeparator, Ghost, Scrollbar, NoScrollbar, JumpLabels,
    Select1, NoSelect1, Exit0, NoExit0, Read0, NoRead0, Print0, NoPrint0,
    PrintQuery, NoPrintQuery, Prompt, Gutter, GutterRaw, Pointer, Marker,
    MarkerMultiLine, Sync, NoSync, NoHistory, History, HistorySize, NoHeader,
    NoHeaderLines, Header, HeaderLines, NoFooter, Footer, HeaderFirst,
    NoHeaderFirst, Gap, NoGap, GapLine, NoGapLine, Ellipsis, Preview,
    NoPreview, PreviewWindow, NoPreviewBorder, PreviewBorder, PreviewWrapSign,
    Height, MinHeight, NoHeight, NoMargin, NoPadding, NoBorder, Border,
    ListBorder, NoListBorder, NoListLabel, ListLabel, ListLabelPos,
    NoHeaderBorder, HeaderBorder, NoHeaderLinesBorder, HeaderLinesBorder,
    NoHeaderLabel, HeaderLabel, HeaderLabelPos, NoFooterBorder, FooterBorder,
    NoFooterLabel, FooterLabel, FooterLabelPos, NoInputBorder, InputBorder,
    NoInputLabel, InputLabel, InputLabelPos, NoBorderLabel, BorderLabel,
    BorderLabelPos, NoPreviewLabel, PreviewLabel, PreviewLabelPos, Style,
    NoUnicode, Unicode, Ambidouble, NoAmbidouble, Margin, Padding, Tabstop,
    WithShell, Listen, ListenUnsafe, NoListen, Clear, NoClear, Walker,
    WalkerRoot, WalkerSkip, Threads, Bench, ProfileCpu, ProfileMem,
    ProfileBlock, ProfileMutex, DoubleDash,
};

struct OptName { const char* name; Opt opt; };

// fzf: options.go parseOptions switch labels, in the same order.
const OptName kOptNames[] = {
    {"--man", Opt::Man}, {"--bash", Opt::Bash}, {"--zsh", Opt::Zsh},
    {"--fish", Opt::Fish}, {"--nushell", Opt::Nushell},
    {"-h", Opt::Help}, {"--help", Opt::Help}, {"--version", Opt::Version},
    {"--no-winpty", Opt::NoWinpty},
    {"--tmux", Opt::Tmux}, {"--popup", Opt::Tmux},
    {"--no-tmux", Opt::NoTmux}, {"--no-popup", Opt::NoTmux},
    {"--tty-default", Opt::TtyDefault}, {"--no-tty-default", Opt::NoTtyDefault},
    {"--force-tty-in", Opt::ForceTtyIn}, {"--no-force-tty-in", Opt::NoForceTtyIn},
    {"--proxy-script", Opt::ProxyScript},
    {"-x", Opt::Extended}, {"--extended", Opt::Extended},
    {"-e", Opt::Exact}, {"--exact", Opt::Exact},
    {"--extended-exact", Opt::ExtendedExact},
    {"+x", Opt::NoExtended}, {"--no-extended", Opt::NoExtended},
    {"+e", Opt::NoExact}, {"--no-exact", Opt::NoExact},
    {"-q", Opt::Query}, {"--query", Opt::Query},
    {"-f", Opt::Filter}, {"--filter", Opt::Filter},
    {"--literal", Opt::Literal}, {"--no-literal", Opt::NoLiteral},
    {"--algo", Opt::Algo}, {"--scheme", Opt::Scheme},
    {"--expect", Opt::Expect}, {"--no-expect", Opt::NoExpect},
    {"--enabled", Opt::Enabled}, {"--no-phony", Opt::Enabled},
    {"--disabled", Opt::Disabled}, {"--phony", Opt::Disabled},
    {"--no-input", Opt::NoInput},
    {"--tiebreak", Opt::Tiebreak}, {"--bind", Opt::Bind}, {"--color", Opt::ColorOpt},
    {"--toggle-sort", Opt::ToggleSort},
    {"-d", Opt::Delimiter}, {"--delimiter", Opt::Delimiter},
    {"-n", Opt::Nth}, {"--nth", Opt::Nth},
    {"--freeze-left", Opt::FreezeLeft}, {"--freeze-right", Opt::FreezeRight},
    {"--with-nth", Opt::WithNth}, {"--accept-nth", Opt::AcceptNth},
    {"-s", Opt::Sort}, {"--sort", Opt::Sort},
    {"+s", Opt::NoSort}, {"--no-sort", Opt::NoSort},
    {"--raw", Opt::Raw}, {"--no-raw", Opt::NoRaw},
    {"--track", Opt::Track}, {"--no-track", Opt::NoTrack},
    {"--id-nth", Opt::IdNth}, {"--no-id-nth", Opt::NoIdNth},
    {"--tac", Opt::Tac}, {"--no-tac", Opt::NoTac},
    {"--tail", Opt::Tail}, {"--no-tail", Opt::NoTail},
    {"--smart-case", Opt::SmartCase},
    {"-i", Opt::IgnoreCase}, {"--ignore-case", Opt::IgnoreCase},
    {"+i", Opt::NoIgnoreCase}, {"--no-ignore-case", Opt::NoIgnoreCase},
    {"-m", Opt::Multi}, {"--multi", Opt::Multi},
    {"+m", Opt::NoMulti}, {"--no-multi", Opt::NoMulti},
    {"--ansi", Opt::Ansi}, {"--no-ansi", Opt::NoAnsi},
    {"--no-mouse", Opt::NoMouse},
    {"+c", Opt::NoColor}, {"--no-color", Opt::NoColor},
    {"+2", Opt::No256}, {"--no-256", Opt::No256},
    {"--black", Opt::Black}, {"--no-black", Opt::NoBlack},
    {"--bold", Opt::Bold}, {"--no-bold", Opt::NoBold},
    {"--layout", Opt::Layout}, {"--reverse", Opt::Reverse}, {"--no-reverse", Opt::NoReverse},
    {"--cycle", Opt::Cycle}, {"--highlight-line", Opt::HighlightLine},
    {"--no-highlight-line", Opt::NoHighlightLine}, {"--no-cycle", Opt::NoCycle},
    {"--wrap", Opt::Wrap}, {"--no-wrap", Opt::NoWrap},
    {"--wrap-word", Opt::WrapWord}, {"--no-wrap-word", Opt::NoWrapWord},
    {"--wrap-sign", Opt::WrapSign},
    {"--multi-line", Opt::MultiLine}, {"--no-multi-line", Opt::NoMultiLine},
    {"--keep-right", Opt::KeepRight}, {"--no-keep-right", Opt::NoKeepRight},
    {"--hscroll", Opt::Hscroll}, {"--no-hscroll", Opt::NoHscroll},
    {"--hscroll-off", Opt::HscrollOff}, {"--scroll-off", Opt::ScrollOff},
    {"--filepath-word", Opt::FilepathWord}, {"--no-filepath-word", Opt::NoFilepathWord},
    {"--info", Opt::Info}, {"--info-command", Opt::InfoCommand},
    {"--no-info-command", Opt::NoInfoCommand}, {"--no-info", Opt::NoInfo},
    {"--inline-info", Opt::InlineInfo}, {"--no-inline-info", Opt::NoInlineInfo},
    {"--separator", Opt::Separator}, {"--no-separator", Opt::NoSeparator},
    {"--ghost", Opt::Ghost},
    {"--scrollbar", Opt::Scrollbar}, {"--no-scrollbar", Opt::NoScrollbar},
    {"--jump-labels", Opt::JumpLabels},
    {"-1", Opt::Select1}, {"--select-1", Opt::Select1},
    {"+1", Opt::NoSelect1}, {"--no-select-1", Opt::NoSelect1},
    {"-0", Opt::Exit0}, {"--exit-0", Opt::Exit0},
    {"+0", Opt::NoExit0}, {"--no-exit-0", Opt::NoExit0},
    {"--read0", Opt::Read0}, {"--no-read0", Opt::NoRead0},
    {"--print0", Opt::Print0}, {"--no-print0", Opt::NoPrint0},
    {"--print-query", Opt::PrintQuery}, {"--no-print-query", Opt::NoPrintQuery},
    {"--prompt", Opt::Prompt}, {"--gutter", Opt::Gutter}, {"--gutter-raw", Opt::GutterRaw},
    {"--pointer", Opt::Pointer}, {"--marker", Opt::Marker},
    {"--marker-multi-line", Opt::MarkerMultiLine},
    {"--sync", Opt::Sync}, {"--no-sync", Opt::NoSync}, {"--async", Opt::NoSync},
    {"--no-history", Opt::NoHistory}, {"--history", Opt::History},
    {"--history-size", Opt::HistorySize},
    {"--no-header", Opt::NoHeader}, {"--no-header-lines", Opt::NoHeaderLines},
    {"--header", Opt::Header}, {"--header-lines", Opt::HeaderLines},
    {"--no-footer", Opt::NoFooter}, {"--footer", Opt::Footer},
    {"--header-first", Opt::HeaderFirst}, {"--no-header-first", Opt::NoHeaderFirst},
    {"--gap", Opt::Gap}, {"--no-gap", Opt::NoGap},
    {"--gap-line", Opt::GapLine}, {"--no-gap-line", Opt::NoGapLine},
    {"--ellipsis", Opt::Ellipsis},
    {"--preview", Opt::Preview}, {"--no-preview", Opt::NoPreview},
    {"--preview-window", Opt::PreviewWindow},
    {"--no-preview-border", Opt::NoPreviewBorder}, {"--preview-border", Opt::PreviewBorder},
    {"--preview-wrap-sign", Opt::PreviewWrapSign},
    {"--height", Opt::Height}, {"--min-height", Opt::MinHeight}, {"--no-height", Opt::NoHeight},
    {"--no-margin", Opt::NoMargin}, {"--no-padding", Opt::NoPadding},
    {"--no-border", Opt::NoBorder}, {"--border", Opt::Border},
    {"--list-border", Opt::ListBorder}, {"--no-list-border", Opt::NoListBorder},
    {"--no-list-label", Opt::NoListLabel}, {"--list-label", Opt::ListLabel},
    {"--list-label-pos", Opt::ListLabelPos},
    {"--no-header-border", Opt::NoHeaderBorder}, {"--header-border", Opt::HeaderBorder},
    {"--no-header-lines-border", Opt::NoHeaderLinesBorder},
    {"--header-lines-border", Opt::HeaderLinesBorder},
    {"--no-header-label", Opt::NoHeaderLabel}, {"--header-label", Opt::HeaderLabel},
    {"--header-label-pos", Opt::HeaderLabelPos},
    {"--no-footer-border", Opt::NoFooterBorder}, {"--footer-border", Opt::FooterBorder},
    {"--no-footer-label", Opt::NoFooterLabel}, {"--footer-label", Opt::FooterLabel},
    {"--footer-label-pos", Opt::FooterLabelPos},
    {"--no-input-border", Opt::NoInputBorder}, {"--input-border", Opt::InputBorder},
    {"--no-input-label", Opt::NoInputLabel}, {"--input-label", Opt::InputLabel},
    {"--input-label-pos", Opt::InputLabelPos},
    {"--no-border-label", Opt::NoBorderLabel}, {"--border-label", Opt::BorderLabel},
    {"--border-label-pos", Opt::BorderLabelPos},
    {"--no-preview-label", Opt::NoPreviewLabel}, {"--preview-label", Opt::PreviewLabel},
    {"--preview-label-pos", Opt::PreviewLabelPos},
    {"--style", Opt::Style},
    {"--no-unicode", Opt::NoUnicode}, {"--unicode", Opt::Unicode},
    {"--ambidouble", Opt::Ambidouble}, {"--no-ambidouble", Opt::NoAmbidouble},
    {"--margin", Opt::Margin}, {"--padding", Opt::Padding}, {"--tabstop", Opt::Tabstop},
    {"--with-shell", Opt::WithShell},
    {"--listen", Opt::Listen}, {"--listen-unsafe", Opt::ListenUnsafe},
    {"--no-listen", Opt::NoListen}, {"--no-listen-unsafe", Opt::NoListen},
    {"--clear", Opt::Clear}, {"--no-clear", Opt::NoClear},
    {"--walker", Opt::Walker}, {"--walker-root", Opt::WalkerRoot},
    {"--walker-skip", Opt::WalkerSkip}, {"--threads", Opt::Threads},
    {"--bench", Opt::Bench}, {"--profile-cpu", Opt::ProfileCpu},
    {"--profile-mem", Opt::ProfileMem}, {"--profile-block", Opt::ProfileBlock},
    {"--profile-mutex", Opt::ProfileMutex},
    {"--", Opt::DoubleDash},
};

const std::unordered_map<std::string, Opt>& opt_table() {
    static const std::unordered_map<std::string, Opt> table = [] {
        std::unordered_map<std::string, Opt> t;
        for (const auto& e : kOptNames) t.emplace(e.name, e.opt);
        return t;
    }();
    return table;
}

// FZFPP_WARN_UNSUPPORTED=1: one stderr line per accepted-but-inert option.
void warn_unsupported(const std::string& name) {
    static const bool enabled = [] {
        const char* v = std::getenv("FZFPP_WARN_UNSUPPORTED");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    if (!enabled) return;
    static std::unordered_set<std::string> seen;
    if (seen.insert(name).second) {
        std::cerr << "fzf++: " << name << " is accepted but has no effect in this version" << std::endl;
    }
}

// ---------------------------------------------------------------------------
// The parser proper (fzf: options.go parseOptions)
// ---------------------------------------------------------------------------

struct ParseState {
    Options& opts;
    int& global_index;          // position across DEFAULT_OPTS_FILE / DEFAULT_OPTS / argv
    BorderShape default_border = kDefaultBorderShape;
};

void parse_into(ParseState& st, const std::vector<std::string>& args) {
    Options& opts = st.opts;
    int start_index = st.global_index;
    size_t i = 0;
    std::optional<std::string> val;   // value from `--opt=value`

    auto clear_exiting = [&]() {
        opts.bash = opts.zsh = opts.fish = opts.nushell = false;
        opts.help = opts.version = opts.man = false;
    };
    auto next_string = [&](const char* message) -> std::string {
        if (val) { std::string v = *val; val.reset(); return v; }
        if (args.size() > i + 1) { ++i; return args[i]; }
        fail(message);
    };
    auto optional_next_string = [&](bool& given) -> std::string {
        if (val) { std::string v = *val; val.reset(); given = true; return v; }
        if (args.size() > i + 1 && !has_prefix(args[i + 1], "-") && !has_prefix(args[i + 1], "+")) {
            ++i; given = true; return args[i];
        }
        given = false;
        return "";
    };
    auto next_int = [&](const char* message) -> int {
        std::string s;
        if (val) { s = *val; val.reset(); }
        else if (args.size() > i + 1) { ++i; s = args[i]; }
        else fail(message);
        int n;
        if (!strict_atoi(s, n)) fail(message);
        return n;
    };
    auto optional_numeric = [&](int default_value) -> int {
        std::string s;
        if (val) { s = *val; val.reset(); }
        else if (args.size() > i + 1 && !args[i + 1].empty() &&
                 std::isdigit(static_cast<unsigned char>(args[i + 1][0]))) {
            ++i; s = args[i];
        } else {
            return default_value;
        }
        return atoi_or_fail(s);
    };
    auto next_dirs = [&]() -> std::vector<std::string> {
        std::vector<std::string> dirs;
        if (val) { dirs.push_back(*val); val.reset(); }
        while (i + 1 < args.size() && is_dir(args[i + 1])) {
            dirs.push_back(args[i + 1]);
            ++i;
        }
        if (dirs.empty()) fail("no directory specified");
        return dirs;
    };
    auto border_arg = [&]() -> BorderShape {
        bool given;
        std::string arg = optional_next_string(given);
        return parse_border(arg, !given);
    };
    auto label_pos = [&](LabelOpts& l, const char* message) {
        parse_label_position(l, next_string(message));
    };

    for (; i < args.size(); ++i) {
        std::string arg = args[i];
        int index = static_cast<int>(i) + start_index;
        val.reset();
        if (has_prefix(arg, "--")) {
            size_t eq = arg.find('=');
            if (eq != std::string::npos && eq > 0) {
                val = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
            }
        }

        auto it = opt_table().find(arg);
        if (it == opt_table().end()) {
            // fzf: short options with an attached value (-qfoo, -m3, ...)
            if (has_prefix(arg, "-q")) opts.query = arg.substr(2);
            else if (has_prefix(arg, "-f")) opts.filter = arg.substr(2);
            else if (has_prefix(arg, "-d")) opts.delimiter = parse_delimiter(arg.substr(2));
            else if (has_prefix(arg, "-n")) opts.nth = parse_nth(arg.substr(2));
            else if (has_prefix(arg, "-s")) opts.sort = 1;
            else if (has_prefix(arg, "-m")) opts.multi = atoi_or_fail(arg.substr(2));
            else fail("unknown option: " + arg);
            if (val) fail("unexpected value for " + arg + ": " + *val);
            continue;
        }

        switch (it->second) {
            case Opt::Man: clear_exiting(); opts.man = true; break;
            case Opt::Bash: clear_exiting(); opts.bash = true; break;
            case Opt::Zsh: clear_exiting(); opts.zsh = true; break;
            case Opt::Fish: clear_exiting(); opts.fish = true; break;
            case Opt::Nushell: clear_exiting(); opts.nushell = true; break;
            case Opt::Help: clear_exiting(); opts.help = true; break;
            case Opt::Version: clear_exiting(); opts.version = true; break;
            case Opt::NoWinpty: opts.no_winpty = true; warn_unsupported(arg); break;
            case Opt::Tmux: {
                bool given;
                std::string str = optional_next_string(given);
                if (given) opts.tmux = parse_tmux_options(str, index);
                else { TmuxOptions t; t.index = index; opts.tmux = t; }
                warn_unsupported(arg);
                break;
            }
            case Opt::NoTmux: opts.tmux.reset(); break;
            case Opt::TtyDefault: opts.tty_default = next_string("tty device name required"); warn_unsupported(arg); break;
            case Opt::NoTtyDefault: opts.tty_default.clear(); break;
            case Opt::ForceTtyIn: opts.force_tty_in = true; warn_unsupported(arg); break;
            case Opt::NoForceTtyIn: opts.force_tty_in = false; break;
            case Opt::ProxyScript: opts.proxy_script = next_string(""); warn_unsupported(arg); break;
            case Opt::Extended: opts.extended = true; break;
            case Opt::Exact: opts.fuzzy = false; break;
            case Opt::ExtendedExact: opts.fuzzy = false; opts.extended = true; break;
            case Opt::NoExtended: opts.extended = false; break;
            case Opt::NoExact: opts.fuzzy = true; break;
            case Opt::Query: opts.query = next_string("query string required"); break;
            case Opt::Filter: opts.filter = next_string("query string required"); break;
            case Opt::Literal: opts.normalize = false; break;
            case Opt::NoLiteral: opts.normalize = true; break;
            case Opt::Algo: opts.algo = parse_algo(next_string("algorithm required (v1|v2)")); break;
            case Opt::Scheme: {
                std::string s = next_string("scoring scheme required (default|path|history)");
                opts.criteria = parse_scheme(s);
                opts.scheme = s;
                break;
            }
            case Opt::Expect: {
                std::string s = next_string("key names required");
                if (s.empty()) fail("key names required");
                opts.expect_specs.push_back(s);
                break;
            }
            case Opt::NoExpect: opts.expect_specs.clear(); break;
            case Opt::Enabled: opts.phony = false; break;
            case Opt::Disabled: opts.phony = true; break;
            case Opt::NoInput: opts.inputless = true; warn_unsupported(arg); break;
            case Opt::Tiebreak: opts.criteria = parse_tiebreak(next_string("sort criterion required")); break;
            case Opt::Bind: opts.bind_specs.push_back(next_string("bind expression required")); break;
            case Opt::ColorOpt: {
                bool given;
                std::string spec = optional_next_string(given);
                if (spec.empty()) {
                    opts.theme.reset(ThemeBase::Empty);
                } else {
                    parse_theme(opts.theme, spec);
                }
                break;
            }
            case Opt::ToggleSort: {
                std::string s = next_string("key name required");
                opts.toggle_sort_specs.push_back(s);
                break;
            }
            case Opt::Delimiter: opts.delimiter = parse_delimiter(next_string("delimiter required")); break;
            case Opt::Nth: opts.nth = parse_nth(next_string("nth expression required")); break;
            case Opt::FreezeLeft: opts.freeze_left = next_int("number of fields required"); warn_unsupported(arg); break;
            case Opt::FreezeRight: opts.freeze_right = next_int("number of fields required"); warn_unsupported(arg); break;
            case Opt::WithNth: {
                std::string s = next_string("nth expression required");
                static const std::regex ranges_re("^[0-9,.-]+$");
                if (std::regex_match(s, ranges_re)) parse_nth(s);   // validate
                else if (s.find('{') == std::string::npos) fail("template should include at least 1 placeholder: " + s);
                opts.with_nth_expr = s;
                break;
            }
            case Opt::AcceptNth: {
                std::string s = next_string("nth expression required");
                static const std::regex ranges_re("^[0-9,.-]+$");
                if (std::regex_match(s, ranges_re)) parse_nth(s);
                else if (s.find('{') == std::string::npos) fail("template should include at least 1 placeholder: " + s);
                opts.accept_nth_expr = s;
                break;
            }
            case Opt::Sort: opts.sort = optional_numeric(1); break;
            case Opt::NoSort: opts.sort = 0; break;
            case Opt::Raw: opts.raw = true; warn_unsupported(arg); break;
            case Opt::NoRaw: opts.raw = false; break;
            case Opt::Track: opts.track = TrackOption::Enabled; warn_unsupported(arg); break;
            case Opt::NoTrack: opts.track = TrackOption::Disabled; break;
            case Opt::IdNth: opts.id_nth = parse_nth(next_string("nth expression required")); warn_unsupported(arg); break;
            case Opt::NoIdNth: opts.id_nth.clear(); break;
            case Opt::Tac: opts.tac = true; break;
            case Opt::NoTac: opts.tac = false; break;
            case Opt::Tail:
                opts.tail = next_int("number of items to keep required");
                if (opts.tail <= 0) fail("number of items to keep must be a positive integer");
                warn_unsupported(arg);
                break;
            case Opt::NoTail: opts.tail = 0; break;
            case Opt::SmartCase: opts.case_mode = CaseMode::Smart; break;
            case Opt::IgnoreCase: opts.case_mode = CaseMode::Ignore; break;
            case Opt::NoIgnoreCase: opts.case_mode = CaseMode::Respect; break;
            case Opt::Multi: opts.multi = optional_numeric(kMaxMulti); break;
            case Opt::NoMulti: opts.multi = 0; break;
            case Opt::Ansi: opts.ansi = true; break;
            case Opt::NoAnsi: opts.ansi = false; break;
            case Opt::NoMouse: opts.mouse = false; break;
            case Opt::NoColor: opts.theme.reset(ThemeBase::NoColor); opts.theme.explicit_base = ThemeBase::NoColor; break;
            case Opt::No256: opts.theme.reset(ThemeBase::Default16); break;
            case Opt::Black: opts.black = true; break;
            case Opt::NoBlack: opts.black = false; break;
            case Opt::Bold: opts.bold = true; break;
            case Opt::NoBold: opts.bold = false; break;
            case Opt::Layout: opts.layout = parse_layout(next_string("layout required (default / reverse / reverse-list)")); break;
            case Opt::Reverse: opts.layout = LayoutType::Reverse; break;
            case Opt::NoReverse: opts.layout = LayoutType::Default; break;
            case Opt::Cycle: opts.cycle = true; break;
            case Opt::HighlightLine: opts.cursor_line = true; break;
            case Opt::NoHighlightLine: opts.cursor_line = false; break;
            case Opt::NoCycle: opts.cycle = false; break;
            case Opt::Wrap: {
                bool given;
                std::string s = optional_next_string(given);
                if (given) {
                    if (s == "char") { opts.wrap = true; opts.wrap_word = false; }
                    else if (s == "word") { opts.wrap = true; opts.wrap_word = true; }
                    else fail("invalid wrap mode: " + s + " (expected: char or word)");
                } else {
                    opts.wrap = true;
                }
                break;
            }
            case Opt::NoWrap: opts.wrap = false; opts.wrap_word = false; break;
            case Opt::WrapWord: opts.wrap = true; opts.wrap_word = true; break;
            case Opt::NoWrapWord: opts.wrap_word = false; break;
            case Opt::WrapSign: opts.wrap_sign = next_string("wrap sign required"); break;
            case Opt::MultiLine: opts.multi_line = true; break;
            case Opt::NoMultiLine: opts.multi_line = false; break;
            case Opt::KeepRight: opts.keep_right = true; break;
            case Opt::NoKeepRight: opts.keep_right = false; break;
            case Opt::Hscroll: opts.hscroll = true; break;
            case Opt::NoHscroll: opts.hscroll = false; break;
            case Opt::HscrollOff: opts.hscroll_off = next_int("hscroll offset required"); break;
            case Opt::ScrollOff: opts.scroll_off = next_int("scroll offset required"); break;
            case Opt::FilepathWord: opts.file_word = true; break;
            case Opt::NoFilepathWord: opts.file_word = false; break;
            case Opt::Info: parse_info_style(next_string("info style required"), opts.info_style, opts.info_prefix); break;
            case Opt::InfoCommand: opts.info_command = next_string("info command required"); warn_unsupported(arg); break;
            case Opt::NoInfoCommand: opts.info_command.clear(); break;
            case Opt::NoInfo: opts.info_style = InfoStyle::Hidden; break;
            case Opt::InlineInfo: opts.info_style = InfoStyle::Inline; opts.info_prefix = kDefaultInfoPrefix; break;
            case Opt::NoInlineInfo: opts.info_style = InfoStyle::Default; break;
            case Opt::Separator: opts.separator = next_string("separator character required"); break;
            case Opt::NoSeparator: opts.separator = ""; break;
            case Opt::Ghost: opts.ghost = next_string("ghost text required"); warn_unsupported(arg); break;
            case Opt::Scrollbar: {
                bool given;
                std::string bar = optional_next_string(given);
                if (given) opts.scrollbar = bar; else opts.scrollbar.reset();
                break;
            }
            case Opt::NoScrollbar: opts.scrollbar = ""; break;
            case Opt::JumpLabels:
                opts.jump_labels = next_string("label characters required");
                warn_unsupported(arg);
                break;
            case Opt::Select1: opts.select_1 = true; break;
            case Opt::NoSelect1: opts.select_1 = false; break;
            case Opt::Exit0: opts.exit_0 = true; break;
            case Opt::NoExit0: opts.exit_0 = false; break;
            case Opt::Read0: opts.read_zero = true; break;
            case Opt::NoRead0: opts.read_zero = false; break;
            case Opt::Print0: opts.print0 = true; break;
            case Opt::NoPrint0: opts.print0 = false; break;
            case Opt::PrintQuery: opts.print_query = true; break;
            case Opt::NoPrintQuery: opts.print_query = false; break;
            case Opt::Prompt: opts.prompt = next_string("prompt string required"); break;
            case Opt::Gutter: opts.gutter = first_line(next_string("gutter character required")); break;
            case Opt::GutterRaw: opts.gutter_raw = first_line(next_string("gutter character for raw mode required")); break;
            case Opt::Pointer: opts.pointer = first_line(next_string("pointer sign required")); break;
            case Opt::Marker: opts.marker = first_line(next_string("marker sign required")); break;
            case Opt::MarkerMultiLine:
                opts.marker_multi = parse_marker_multi_line(first_line(next_string("marker sign for multi-line entries required")));
                break;
            case Opt::Sync: opts.sync = true; break;
            case Opt::NoSync: opts.sync = false; break;
            case Opt::NoHistory: opts.history.reset(); break;
            case Opt::History: opts.history = next_string("history file path required"); warn_unsupported(arg); break;
            case Opt::HistorySize: {
                int n = next_int("history max size required");
                if (n < 1) fail("history max must be a positive integer");
                opts.history_size = n;
                break;
            }
            case Opt::NoHeader: opts.header.clear(); break;
            case Opt::NoHeaderLines: opts.header_lines = 0; break;
            case Opt::Header: opts.header = str_lines(next_string("header string required")); break;
            case Opt::HeaderLines: opts.header_lines = next_int("number of header lines required"); break;
            case Opt::NoFooter: opts.footer.clear(); break;
            case Opt::Footer: opts.footer = str_lines(next_string("footer string required")); warn_unsupported(arg); break;
            case Opt::HeaderFirst: opts.header_first = true; break;
            case Opt::NoHeaderFirst: opts.header_first = false; break;
            case Opt::Gap: opts.gap = optional_numeric(1); warn_unsupported(arg); break;
            case Opt::NoGap: opts.gap = 0; break;
            case Opt::GapLine: {
                bool given;
                std::string bar = optional_next_string(given);
                if (given) opts.gap_line = bar; else opts.gap_line.reset();
                break;
            }
            case Opt::NoGapLine: opts.gap_line = ""; break;
            case Opt::Ellipsis: opts.ellipsis = first_line(next_string("ellipsis string required")); break;
            case Opt::Preview: opts.preview.command = next_string("preview command required"); break;
            case Opt::NoPreview: opts.preview.command.clear(); break;
            case Opt::PreviewWindow:
                parse_preview_window(opts.preview, next_string(
                    "preview window layout required: [up|down|left|right|next][,SIZE[%]][,border-STYLE][,wrap][,cycle][,hidden][,+SCROLL[OFFSETS][/DENOM]][,~HEADER_LINES][,default]"));
                break;
            case Opt::NoPreviewBorder: opts.preview.border = BorderShape::None; break;
            case Opt::PreviewBorder: opts.preview.border = border_arg(); break;
            case Opt::PreviewWrapSign: opts.preview_wrap_sign = next_string("preview wrap sign required"); break;
            case Opt::Height: opts.height = parse_height(next_string("height required: [~][-]HEIGHT[%]"), index); break;
            case Opt::MinHeight: {
                std::string expr = next_string("minimum height required: HEIGHT[+]");
                bool auto_ = false;
                if (has_suffix(expr, "+")) { expr.pop_back(); auto_ = true; }
                int num;
                if (!strict_atoi(expr, num) || num < 0) fail("minimum height must be a non-negative integer");
                opts.min_height = auto_ ? -num : num;
                break;
            }
            case Opt::NoHeight: opts.height = HeightSpec{}; break;
            case Opt::NoMargin: opts.margin = {}; break;
            case Opt::NoPadding: opts.padding = {}; break;
            case Opt::NoBorder: opts.border_shape = BorderShape::None; break;
            case Opt::Border: opts.border_shape = border_arg(); break;
            case Opt::ListBorder: {
                bool given;
                std::string a = optional_next_string(given);
                opts.list_border = parse_border(a, !given);
                if (opts.list_border == BorderShape::Line) {
                    if (given) fail("list border cannot be 'line'");
                    opts.list_border = BorderShape::Rounded;
                }
                warn_unsupported(arg);
                break;
            }
            case Opt::NoListBorder: opts.list_border = BorderShape::None; break;
            case Opt::NoListLabel: opts.list_label.label.clear(); break;
            case Opt::ListLabel: opts.list_label.label = next_string("label required"); warn_unsupported(arg); break;
            case Opt::ListLabelPos: label_pos(opts.list_label, "label position required (positive or negative integer or 'center')"); break;
            case Opt::NoHeaderBorder: opts.header_border = BorderShape::None; break;
            case Opt::HeaderBorder: opts.header_border = border_arg(); warn_unsupported(arg); break;
            case Opt::NoHeaderLinesBorder: opts.header_lines_border = BorderShape::Undefined; break;
            case Opt::HeaderLinesBorder: opts.header_lines_border = border_arg(); warn_unsupported(arg); break;
            case Opt::NoHeaderLabel: opts.header_label.label.clear(); break;
            case Opt::HeaderLabel: opts.header_label.label = next_string("header label required"); warn_unsupported(arg); break;
            case Opt::HeaderLabelPos: label_pos(opts.header_label, "header label position required (positive or negative integer or 'center')"); break;
            case Opt::NoFooterBorder: opts.footer_border = BorderShape::None; break;
            case Opt::FooterBorder: opts.footer_border = border_arg(); warn_unsupported(arg); break;
            case Opt::NoFooterLabel: opts.footer_label.label.clear(); break;
            case Opt::FooterLabel: opts.footer_label.label = next_string("footer label required"); warn_unsupported(arg); break;
            case Opt::FooterLabelPos: label_pos(opts.footer_label, "footer label position required (positive or negative integer or 'center')"); break;
            case Opt::NoInputBorder: opts.input_border = BorderShape::None; break;
            case Opt::InputBorder: opts.input_border = border_arg(); warn_unsupported(arg); break;
            case Opt::NoInputLabel: opts.input_label.label.clear(); break;
            case Opt::InputLabel: opts.input_label.label = next_string("input label required"); warn_unsupported(arg); break;
            case Opt::InputLabelPos: label_pos(opts.input_label, "input label position required (positive or negative integer or 'center')"); break;
            case Opt::NoBorderLabel: opts.border_label.label.clear(); break;
            case Opt::BorderLabel: opts.border_label.label = next_string("label required"); break;
            case Opt::BorderLabelPos: label_pos(opts.border_label, "label position required (positive or negative integer or 'center')"); break;
            case Opt::NoPreviewLabel: opts.preview_label.label.clear(); break;
            case Opt::PreviewLabel: opts.preview_label.label = next_string("preview label required"); break;
            case Opt::PreviewLabelPos: label_pos(opts.preview_label, "preview label position required (positive or negative integer or 'center')"); break;
            case Opt::Style: apply_preset(opts, next_string("preset name required: [default|minimal|full[:BORDER_STYLE]]"), st.default_border); break;
            case Opt::NoUnicode: opts.unicode = false; break;
            case Opt::Unicode: opts.unicode = true; break;
            case Opt::Ambidouble: opts.ambidouble = true; break;
            case Opt::NoAmbidouble: opts.ambidouble = false; break;
            case Opt::Margin: opts.margin = parse_margin("margin", next_string("margin required (TRBL / TB,RL / T,RL,B / T,R,B,L)")); break;
            case Opt::Padding: opts.padding = parse_margin("padding", next_string("padding required (TRBL / TB,RL / T,RL,B / T,R,B,L)")); break;
            case Opt::Tabstop: opts.tabstop = next_int("tab stop required"); break;
            case Opt::WithShell: opts.with_shell = next_string("shell command and flags required"); break;
            case Opt::Listen:
            case Opt::ListenUnsafe: {
                bool given;
                std::string s = optional_next_string(given);
                opts.listen_addr = given ? s : std::string("localhost:0");
                opts.unsafe = (it->second == Opt::ListenUnsafe);
                warn_unsupported(arg);
                break;
            }
            case Opt::NoListen: opts.listen_addr.reset(); opts.unsafe = false; break;
            case Opt::Clear: opts.clear_on_exit = true; break;
            case Opt::NoClear: opts.clear_on_exit = false; break;
            case Opt::Walker: opts.walker = parse_walker_opts(next_string("walker options required [file][,dir][,follow][,hidden]")); break;
            case Opt::WalkerRoot: opts.walker_root = next_dirs(); break;
            case Opt::WalkerSkip: {
                opts.walker_skip.clear();
                for (const auto& s : split(next_string("directory names to ignore required"), ',')) {
                    if (!s.empty()) opts.walker_skip.push_back(s);
                }
                break;
            }
            case Opt::Threads:
                opts.threads = next_int("number of threads required");
                if (opts.threads < 0) fail("--threads must be a positive integer");
                break;
            case Opt::Bench: {
                std::string s = next_string("duration required (e.g. 3s, 500ms)");
                static const std::regex dur_re("^[0-9.]+(ns|us|µs|ms|s|m|h)$");
                if (!std::regex_match(s, dur_re)) fail("invalid duration for --bench: " + s);
                warn_unsupported(arg);
                break;
            }
            case Opt::ProfileCpu: next_string("file path required: cpu"); warn_unsupported(arg); break;
            case Opt::ProfileMem: next_string("file path required: mem"); warn_unsupported(arg); break;
            case Opt::ProfileBlock: next_string("file path required: block"); warn_unsupported(arg); break;
            case Opt::ProfileMutex: next_string("file path required: mutex"); warn_unsupported(arg); break;
            case Opt::DoubleDash: break;   // fzf: ignored
        }

        if (val) fail("unexpected value for " + arg + ": " + *val);
    }
    st.global_index += static_cast<int>(args.size());

    // fzf: trailing checks at the end of parseOptions
    if (opts.header_lines < 0) fail("header lines must be a non-negative integer");
    if (opts.hscroll_off < 0) fail("hscroll offset must be a non-negative integer");
    if (opts.scroll_off < 0) fail("scroll offset must be a non-negative integer");
    if (opts.tabstop < 1) fail("tab stop must be a positive integer");
    if (opts.jump_labels.empty()) fail("empty jump labels");
    if (opts.freeze_left < 0 || opts.freeze_right < 0) fail("--freeze-left and --freeze-right must be non-negative integers");
}

// fzf: options.go validateOptions
void validate(const Options& opts) {
    if (opts.pointer && approx_width(*opts.pointer) > 2) fail("pointer display width should be up to 2");
    if (opts.marker && approx_width(*opts.marker) > 2) fail("marker display width should be up to 2");
    if ((opts.gutter && approx_width(*opts.gutter) != 1) ||
        (opts.gutter_raw && approx_width(*opts.gutter_raw) != 1)) {
        fail("gutter display width should be 1");
    }
    if (opts.scrollbar) {
        size_t n = 0;
        try { n = static_cast<size_t>(utf8::distance(opts.scrollbar->begin(), opts.scrollbar->end())); }
        catch (...) { n = opts.scrollbar->size(); }
        if (n > 2) fail("--scrollbar should be given one or two characters");
    }
    if (opts.height.auto_ && (!opts.tmux || opts.tmux->index < opts.height.index)) {
        if (opts.margin[0].percent || opts.margin[2].percent) {
            fail("adaptive height is not compatible with top/bottom percent margin");
        }
        if (opts.padding[0].percent || opts.padding[2].percent) {
            fail("adaptive height is not compatible with top/bottom percent padding");
        }
    }
    if (opts.theme.slot(ThemeSlot::Nth).is_color_defined()) {
        fail("only ANSI attributes are allowed for 'nth' (regular, bold, underline, reverse, dim, italic, strikethrough)");
    }
    if (opts.border_shape == BorderShape::Inline || opts.list_border == BorderShape::Inline ||
        opts.input_border == BorderShape::Inline || opts.preview.border == BorderShape::Inline) {
        fail("inline border is only supported for --header-border, --header-lines-border, and --footer-border");
    }
    if (opts.header_border == BorderShape::Inline &&
        opts.header_lines_border != BorderShape::Inline &&
        opts.header_lines_border != BorderShape::Undefined &&
        opts.header_lines_border != BorderShape::None) {
        fail("--header-border=inline requires --header-lines-border to be inline or unset");
    }
}

// The subset of fzf: postProcessOptions that concerns parsed values.
void post_process(Options& opts) {
    if (opts.border_shape == BorderShape::Undefined) opts.border_shape = BorderShape::None;
    if (opts.list_border == BorderShape::Undefined) opts.list_border = BorderShape::None;
    if (opts.input_border == BorderShape::Undefined) opts.input_border = BorderShape::None;
    if (opts.header_border == BorderShape::Undefined) opts.header_border = BorderShape::None;
    if (opts.footer_border == BorderShape::Undefined) opts.footer_border = BorderShape::Line;
    if (opts.header_lines_border == BorderShape::None) opts.header_lines_border = BorderShape::Phantom;

    if (!opts.pointer) opts.pointer = opts.unicode ? "\xE2\x96\x8C" : ">";      // ▌
    if (!opts.gap_line) opts.gap_line = opts.unicode ? "\xE2\x94\x88" : "-";    // ┈
    if (!opts.marker) {
        if (opts.marker_multi && (*opts.marker_multi)[0].empty()) opts.marker = "";
        else opts.marker = opts.unicode ? "\xE2\x94\x83" : ">";                 // ┃
    }
    if (!opts.marker_multi) {
        if (opts.marker->empty()) opts.marker_multi = std::array<std::string, 3>{"", "", ""};
        else if (opts.unicode) opts.marker_multi = std::array<std::string, 3>{"\xE2\x95\xBB", "\xE2\x94\x83", "\xE2\x95\xB9"};
        else opts.marker_multi = std::array<std::string, 3>{".", "|", "'"};
    }
    int marker_len = approx_width(*opts.marker);
    int marker_multi_len = approx_width((*opts.marker_multi)[0]);
    int diff = marker_multi_len - marker_len;
    if (diff > 0) *opts.marker += std::string(static_cast<size_t>(diff), ' ');
    else if (diff < 0) for (auto& m : *opts.marker_multi) m += std::string(static_cast<size_t>(-diff), ' ');

    // --nth covering everything is the same as no --nth
    if (!opts.extended || opts.nth.size() == 1) {
        for (const auto& r : opts.nth) {
            if (r.is_full()) { opts.nth.clear(); break; }
        }
    }
}

bool has_reload_or_transform_on_start(const Options& opts) {
    for (const auto& spec : opts.bind_specs) {
        std::string s = to_lower(spec);
        size_t p = s.find("start:");
        if (p == std::string::npos) continue;
        std::string rest = s.substr(p + 6);
        if (rest.find("reload") != std::string::npos || rest.find("transform") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Legacy view (removed in T1.7)
// ---------------------------------------------------------------------------

// fzf's key names are case-insensitive; the legacy dispatcher looks keys up
// in lowercase, except the letter after "alt-" (alt-B and alt-b differ).
std::string normalize_bind_key(const std::string& key) {
    std::string lower = to_lower(key);
    if (lower.size() > 4 && lower.compare(0, 4, "alt-") == 0) lower[4] = key[4];
    return lower;
}

void legacy_split_binds(Options& opts) {
    static const std::vector<std::string> trailing_colon_actions = {
        "reload", "preview", "change-preview", "change-prompt", "change-header",
        "transform-header", "transform", "execute", "execute-silent",
        "become", "unbind", "rebind",
    };
    auto match_trailing = [](const std::string& s, size_t seg_start) -> size_t {
        for (const auto& name : trailing_colon_actions) {
            size_t n = name.size();
            if (seg_start + n < s.size() && s.compare(seg_start, n, name) == 0 && s[seg_start + n] == ':') {
                return n;
            }
        }
        return 0;
    };

    for (const auto& spec : opts.bind_specs) {
        size_t start = 0, seg_start = 0;
        bool seg_start_valid = false;
        int depth = 0;
        bool in_trailing_arg = false;
        for (size_t i = 0; i <= spec.size(); ++i) {
            bool at_end = (i == spec.size());
            char c = at_end ? '\0' : spec[i];
            if (!in_trailing_arg) {
                if (c == '(' || c == '[' || c == '{') depth++;
                else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
                else if (c == ':' && depth == 0) {
                    if (!seg_start_valid) { seg_start_valid = true; seg_start = i + 1; }
                    else {
                        size_t n = match_trailing(spec, seg_start);
                        if (n > 0 && seg_start + n == i) in_trailing_arg = true;
                    }
                } else if (c == '+' && depth == 0 && seg_start_valid) {
                    seg_start = i + 1;
                }
            }
            if ((c == ',' && depth == 0 && !in_trailing_arg) || at_end) {
                std::string pair = spec.substr(start, i - start);
                start = i + 1;
                seg_start_valid = false;
                size_t colon = pair.find(':');
                if (colon != std::string::npos) {
                    std::string key = pair.substr(0, colon);
                    std::string action = pair.substr(colon + 1);
                    if (!key.empty()) opts.bindings[normalize_bind_key(key)] = action;
                }
                if (in_trailing_arg) break;
            }
        }
    }

    for (const auto& spec : opts.toggle_sort_specs) {
        opts.bindings[normalize_bind_key(spec)] = "toggle-sort";
    }

    for (const char* key : {"ctrl-c", "ctrl-g", "ctrl-q"}) opts.bindings.emplace(key, "abort");
    static const std::pair<const char*, const char*> default_binds[] = {
        {"ctrl-j", "down"}, {"ctrl-k", "up"}, {"ctrl-p", "up"}, {"ctrl-n", "down"},
        {"ctrl-u", "unix-line-discard"}, {"ctrl-w", "unix-word-rubout"},
        {"ctrl-a", "beginning-of-line"}, {"ctrl-e", "end-of-line"},
        {"ctrl-b", "backward-char"}, {"ctrl-f", "forward-char"},
        {"ctrl-d", "delete-char/eof"}, {"ctrl-h", "backward-delete-char"},
        {"alt-b", "backward-word"}, {"alt-f", "forward-word"}, {"alt-d", "kill-word"},
        {"alt-bs", "backward-kill-word"}, {"btab", "toggle+up"}, {"tab", "toggle+down"},
        {"home", "first"}, {"end", "last"},
    };
    for (const auto& [key, action] : default_binds) opts.bindings.emplace(key, action);
}

void derive_legacy_fields(Options& opts) {
    opts.disabled = opts.phony;
    if (opts.filter) opts.query = *opts.filter;

    if (opts.height.is_set() && opts.height.size > 0) {
        opts.legacy_height = static_cast<int>(opts.height.size);
        opts.height_is_percent = opts.height.percent;
        if (opts.height.percent) opts.legacy_height = std::clamp(opts.legacy_height, 0, 100);
    } else {
        opts.legacy_height = 0;
        opts.height_is_percent = false;
    }
    opts.legacy_header = opts.header.empty() ? "" : opts.header[0];
    opts.border = (opts.border_shape != BorderShape::None);
    opts.no_mouse = !opts.mouse;
    opts.preview_command = opts.preview.command;
    opts.info_hidden = (opts.info_style == InfoStyle::Hidden);
    switch (opts.preview.position) {
        case WindowPosition::Up: opts.preview_position = "up"; break;
        case WindowPosition::Down: opts.preview_position = "down"; break;
        case WindowPosition::Left: opts.preview_position = "left"; break;
        default: opts.preview_position = "right"; break;
    }
    opts.preview_size_percent = static_cast<int>(opts.preview.size.size);
    opts.preview_size_is_percent = opts.preview.size.percent;
    opts.preview_wrap = opts.preview.wrap;
    opts.preview_hidden = opts.preview.hidden;
    opts.preview_follow = opts.preview.follow;

    opts.legacy_delimiter = opts.delimiter.awk ? "" : opts.delimiter.pattern;

    opts.bindings.clear();
    legacy_split_binds(opts);

    opts.expect_keys.clear();
    for (const auto& spec : opts.expect_specs) {
        for (auto key : split(spec, ',')) {
            size_t a = key.find_first_not_of(" \t");
            size_t b = key.find_last_not_of(" \t");
            if (a == std::string::npos) continue;
            key = key.substr(a, b - a + 1);
            if (std::find(opts.expect_keys.begin(), opts.expect_keys.end(), key) == opts.expect_keys.end()) {
                opts.expect_keys.push_back(key);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

const char* kUsage = R"(usage: fzf [options]

  Search mode
    -x, --extended           Extended-search mode
                             (enabled by default; +x or --no-extended to disable)
    -e, --exact              Enable Exact-match
    -i, --ignore-case        Case-insensitive match (default: smart-case match)
    +i, --no-ignore-case     Case-sensitive match
    --smart-case             Smart-case match (default)
    --scheme=SCHEME          Scoring scheme [default|path|history]
    --algo=TYPE              Fuzzy matching algorithm: [v1|v2] (default: v2)
    -n, --nth=N[,..]         Comma-separated list of field index expressions
                             for limiting search scope. Each can be a non-zero
                             integer or a range expression ([BEGIN]..[END]).
    --with-nth=N[,..]        Transform the presentation of each line using
                             field index expressions
    --accept-nth=N[,..]      Define which fields to print on accept
    -d, --delimiter=STR      Field delimiter regex (default: AWK-style)
    --disabled               Do not perform search
    --tiebreak=CRI[,..]      Comma-separated list of sort criteria to apply
                             when the scores are tied [length|chunk|begin|end|index]
                             (default: length)

  Input/Output
    --read0                  Read input delimited by ASCII NUL characters
    --print0                 Print output delimited by ASCII NUL characters
    --ansi                   Enable processing of ANSI color codes
    --sync                   Synchronous search for multi-staged filtering
    --tac                    Reverse the order of the input
    --no-sort                Do not sort the result

  Interface
    -m, --multi[=MAX]        Enable multi-select with tab/shift-tab
    --no-mouse               Disable mouse
    --bind=KEYBINDS          Custom key bindings. Refer to the man page.
    --cycle                  Enable cyclic scroll
    --keep-right             Keep the right end of the line visible on overflow
    --hscroll-off=COLS       Number of screen columns to keep to the right of the
                             highlighted substring (default: 10)
    --filepath-word          Make word-wise movements respect path separators
    --jump-labels=CHARS      Label characters for jump mode

  Layout
    --height=[~]HEIGHT[%]    Display fzf window below the cursor with the given
                             height instead of using fullscreen
    --min-height=HEIGHT[+]   Minimum height when --height is given as a percentage
    --layout=LAYOUT          Choose layout: [default|reverse|reverse-list]
    --border[=STYLE]         Draw border around the finder
                             [rounded|sharp|bold|block|thinblock|double|horizontal|
                              vertical|top|bottom|left|right|none] (default: rounded)
    --border-label=LABEL     Label to print on the border
    --margin=MARGIN          Screen margin (TRBL | TB,RL | T,RL,B | T,R,B,L)
    --padding=PADDING        Padding inside border (TRBL | TB,RL | T,RL,B | T,R,B,L)
    --info=STYLE             Finder info style [default|right|hidden|inline[-right][:PREFIX]]
    --separator=STR          String to form horizontal separator on info line
    --no-separator           Hide info line separator
    --scrollbar[=C1[C2]]     Scrollbar character(s) (each for main and preview window)
    --no-scrollbar           Hide scrollbar
    --prompt=STR             Input prompt (default: '> ')
    --pointer=STR            Pointer to the current line (default: '▌' or '>')
    --marker=STR             Multi-select marker (default: '┃' or '>')
    --header=STR             String to print as header
    --header-lines=N         The first N lines of the input are treated as header
    --header-first           Print header before the prompt line
    --ellipsis=STR           Ellipsis to show when line is truncated (default: '..')

  Display
    --color=COLSPEC          Base scheme (dark|light|16|bw) and/or custom colors
    --no-bold                Do not use bold text
    --black                  Use black background
    --tabstop=SPACES         Number of spaces for a tab character (default: 8)
    --no-unicode             Use ASCII characters instead of Unicode drawing characters

  Preview
    --preview=COMMAND        Command to preview highlighted line ({})
    --preview-window=OPT     Preview window layout (default: right:50%)
                             [up|down|left|right][,SIZE[%]]
                             [,[no]wrap][,[no]cycle][,[no]follow][,[no]hidden]
                             [,border-BORDER_OPT][,+SCROLL[OFFSETS][/DENOM]]
                             [,~HEADER_LINES][,default]
    --preview-label=LABEL    Label to print on the preview window border

  Scripting
    -q, --query=STR          Start the finder with the given query
    -1, --select-1           Automatically select the only match
    -0, --exit-0             Exit immediately when there's no match
    -f, --filter=STR         Filter mode. Do not start interactive finder.
    --print-query            Print query as the first line
    --expect=KEYS            Comma-separated list of keys to complete fzf
    --with-shell=STR         Shell command and flags to start child processes with

  Environment variables
    FZF_DEFAULT_COMMAND      Default command to use when input is tty
    FZF_DEFAULT_OPTS         Default options (e.g. '--layout=reverse --info=inline')
    FZF_DEFAULT_OPTS_FILE    Location of the file to read default options from

fzf++ )" ;

} // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

const char* fzf_compat_version() { return kCompatVersion; }
const char* fzfpp_version() { return kPortVersion; }

// fzf: options.go strLines
std::vector<std::string> str_lines(const std::string& s) {
    std::string t = s;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    return split(t, '\n');
}

// go-shellwords with ParseComment: whitespace-separated words, single and
// double quotes, backslash escapes, `#` starts a comment at a word boundary.
std::vector<std::string> shell_split_words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_word = false;
    enum { NONE, SINGLE, DOUBLE } q = NONE;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q == SINGLE) {
            if (c == '\'') q = NONE; else cur += c;
            in_word = true;
        } else if (q == DOUBLE) {
            if (c == '"') q = NONE;
            else if (c == '\\' && i + 1 < s.size() &&
                     (s[i + 1] == '"' || s[i + 1] == '\\' || s[i + 1] == '$' || s[i + 1] == '`')) cur += s[++i];
            else cur += c;
            in_word = true;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (in_word) { out.push_back(cur); cur.clear(); in_word = false; }
        } else if (c == '#' && !in_word) {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '\'') { q = SINGLE; in_word = true; }
        else if (c == '"') { q = DOUBLE; in_word = true; }
        else if (c == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == '\n') { ++i; continue; }
            cur += s[++i]; in_word = true;
        } else { cur += c; in_word = true; }
    }
    if (q != NONE) throw OptionError{"invalid command line string"};
    if (in_word) out.push_back(cur);
    return out;
}

// fzf: tokenizer.go ParseRange / newRange, options.go splitNth
namespace {
Range new_range(int begin, int end) {
    if (begin == 1 && end != 1) begin = kRangeEllipsis;
    if (end == -1) end = kRangeEllipsis;
    return Range{begin, end};
}

bool parse_range(const std::string& str, Range& out) {
    int begin, end;
    if (str == "..") { out = new_range(kRangeEllipsis, kRangeEllipsis); return true; }
    if (has_prefix(str, "..")) {
        if (!strict_atoi(str.substr(2), end) || end == 0) return false;
        out = new_range(kRangeEllipsis, end);
        return true;
    }
    if (has_suffix(str, "..")) {
        if (!strict_atoi(str.substr(0, str.size() - 2), begin) || begin == 0) return false;
        out = new_range(begin, kRangeEllipsis);
        return true;
    }
    size_t dots = str.find("..");
    if (dots != std::string::npos) {
        std::string a = str.substr(0, dots), b = str.substr(dots + 2);
        if (b.find("..") != std::string::npos) return false;
        if (!strict_atoi(a, begin) || !strict_atoi(b, end) || begin == 0 || end == 0 ||
            (begin < 0 && end > 0)) return false;
        out = new_range(begin, end);
        return true;
    }
    int n;
    if (!strict_atoi(str, n) || n == 0) return false;
    out = new_range(n, n);
    return true;
}
} // namespace

std::vector<Range> parse_nth(const std::string& spec) {
    static const std::regex re("^[0-9,.-]+$");
    if (!std::regex_match(spec, re)) fail("invalid format: " + spec);
    std::vector<Range> ranges;
    for (const auto& token : split(spec, ',')) {
        Range r;
        if (!parse_range(token, r)) fail("invalid format: " + spec);
        ranges.push_back(r);
    }
    return ranges;
}

// fzf: options.go delimiterRegexp
Delimiter parse_delimiter(const std::string& spec) {
    Delimiter d;
    d.awk = false;
    std::string str = spec;
    // Special handling of \t
    for (size_t p = str.find("\\t"); p != std::string::npos; p = str.find("\\t", p + 1)) {
        str.replace(p, 2, "\t");
    }
    d.pattern = str;
    size_t runes = 0;
    try { runes = static_cast<size_t>(utf8::distance(str.begin(), str.end())); } catch (...) { runes = str.size(); }
    if (runes == 1) return d;                                     // single character
    if (str.find_first_of("\\.+*?()|[]{}^$") == std::string::npos) return d;   // no metacharacters
    try {
        std::regex probe(str, std::regex::ECMAScript);
        d.is_regex = true;
    } catch (const std::regex_error&) {
        d.is_regex = false;                                        // invalid regex: literal
    }
    return d;
}

// fzf: options.go ParseOptions
Options parse_option_args(const std::vector<std::string>& args, bool use_defaults) {
    Options opts;
    if (const char* nc = std::getenv("NO_COLOR"); nc && *nc) {
        opts.theme.reset(ThemeBase::NoColor);
        opts.theme.explicit_base = ThemeBase::NoColor;
    }
    if (const char* ea = std::getenv("RUNEWIDTH_EASTASIAN"); ea && std::strcmp(ea, "1") == 0) {
        opts.ambidouble = true;
    }

    int index = 0;
    ParseState st{opts, index};

    if (use_defaults) {
        if (const char* path = std::getenv("FZF_DEFAULT_OPTS_FILE"); path && *path) {
            std::ifstream f(path, std::ios::binary);
            if (!f) fail(std::string("$FZF_DEFAULT_OPTS_FILE: open ") + path + ": " + std::strerror(errno));
            std::stringstream ss;
            ss << f.rdbuf();
            std::vector<std::string> words;
            try { words = shell_split_words(ss.str()); }
            catch (const OptionError& e) { fail(std::string(path) + ": " + e.message); }
            if (!words.empty()) {
                try { parse_into(st, words); }
                catch (const OptionError& e) { fail(std::string(path) + ": " + e.message); }
            }
        }
        if (const char* env = std::getenv("FZF_DEFAULT_OPTS")) {
            std::vector<std::string> words;
            try { words = shell_split_words(env); }
            catch (const OptionError& e) { fail("$FZF_DEFAULT_OPTS: " + e.message); }
            if (!words.empty()) {
                try { parse_into(st, words); }
                catch (const OptionError& e) { fail("$FZF_DEFAULT_OPTS: " + e.message); }
            }
        }
    }

    parse_into(st, args);

    if (opts.scheme.empty()) {
        opts.scheme = "default";
        if (opts.criteria.empty()) {
            if (!has_reload_or_transform_on_start(opts) && isatty(STDIN_FILENO)) opts.scheme = "path";
            opts.criteria = parse_scheme(opts.scheme);
        }
    }

    validate(opts);
    post_process(opts);
    derive_legacy_fields(opts);
    return opts;
}

Options parse_options(int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    Options opts;
    try {
        opts = parse_option_args(args, true);
    } catch (const OptionError& e) {
        std::cerr << e.message << std::endl;
        std::exit(2);
    }

    if (opts.help || opts.man) {
        std::cout << kUsage << kPortVersion << " (fzf " << kCompatVersion << " compatible)" << std::endl;
        std::exit(0);
    }
    if (opts.version) {
        std::cout << kCompatVersion << " (fzf++ " << kPortVersion << ")" << std::endl;
        std::exit(0);
    }
    if (opts.bash || opts.zsh || opts.fish || opts.nushell) {
        std::cerr << "fzf++: shell integration scripts are not bundled; use the scripts shipped with fzf" << std::endl;
        std::exit(2);
    }
    return opts;
}

} // namespace fzf
