// Field tokenizer and nth transformation (fzf: src/tokenizer.go, and
// nthTransformer in src/options.go). See docs/DESIGN.md section 5.
//
// Differences from the Go original, all deliberate:
//   - Tokens are string_views into the caller's line: fzf allocates a
//     util.Chars per token, we allocate nothing while tokenizing.
//   - fzf's Token.prefixLength (a rune offset) is kept as `rune_offset`, and
//     the byte offset is recorded alongside it. Because tokens are contiguous
//     and cover the line from its first token onwards, the rune offset of a
//     token is simply the number of codepoints preceding its byte offset;
//     that count is accumulated in a single forward scan.
//   - Rune counting counts bytes that are not UTF-8 continuation bytes, the
//     same shortcut as fzf's util.countRunes. It agrees with Go's rune count
//     on valid UTF-8; on invalid bytes Go decodes one RuneError per byte, so
//     a malformed line can undercount here. Match positions are only ever
//     used for display, so this is harmless.

#include "tokenizer.hpp"

#include <algorithm>
#include <string>

namespace fzf {

namespace {

inline bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Compiles (and caches, one entry per thread) a regex delimiter. The pattern
// is fixed for the lifetime of a run, so this is a compile per thread; the
// Tokenizer keeps its own copy and never comes here.
const std::regex* delimiter_regex(const std::string& pattern) {
    thread_local std::string cached_pattern;
    thread_local std::regex cached_re;
    thread_local bool primed = false;
    thread_local bool valid = false;
    if (!primed || cached_pattern != pattern) {
        cached_pattern = pattern;
        primed = true;
        valid = false;
        try {
            cached_re.assign(pattern, std::regex::ECMAScript);
            valid = true;
        } catch (const std::regex_error&) {
            valid = false;
        }
    }
    return valid ? &cached_re : nullptr;
}

// unicode.IsSpace, which fzf's StripLastDelimiter uses for the awk delimiter.
bool is_unicode_space(char32_t r) {
    switch (r) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r': case U' ':
        case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
        case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return r >= 0x2000 && r <= 0x200A;
    }
}

// Decodes the codepoint ending at `end` (exclusive). Returns false if the
// bytes there are not a well-formed sequence, in which case nothing is
// trimmed, matching Go's TrimRightFunc on invalid UTF-8 (RuneError is not a
// space, so it stops).
bool last_rune(std::string_view s, size_t end, char32_t& out, size_t& start) {
    if (end == 0) return false;
    size_t i = end - 1;
    size_t back = 0;
    while (i > 0 && is_continuation(s[i]) && back < 3) { --i; ++back; }
    unsigned char lead = static_cast<unsigned char>(s[i]);
    size_t len = end - i;
    if (lead < 0x80) {
        if (len != 1) return false;
        out = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        if (len != 2) return false;
        out = static_cast<char32_t>(lead & 0x1F);
    } else if ((lead & 0xF0) == 0xE0) {
        if (len != 3) return false;
        out = static_cast<char32_t>(lead & 0x0F);
    } else if ((lead & 0xF8) == 0xF0) {
        if (len != 4) return false;
        out = static_cast<char32_t>(lead & 0x07);
    } else {
        return false;
    }
    for (size_t k = i + 1; k < end; ++k) {
        out = (out << 6) | static_cast<char32_t>(static_cast<unsigned char>(s[k]) & 0x3F);
    }
    start = i;
    return true;
}

// fzf: strings.TrimRightFunc(str, unicode.IsSpace)
size_t trim_right_space(std::string_view s) {
    size_t end = s.size();
    while (end > 0) {
        char32_t r = 0;
        size_t start = 0;
        unsigned char last = static_cast<unsigned char>(s[end - 1]);
        if (last < 0x80) {                      // ASCII fast path
            if (!is_unicode_space(last)) break;
            --end;
            continue;
        }
        if (!last_rune(s, end, r, start) || !is_unicode_space(r)) break;
        end = start;
    }
    return end;
}

bool has_suffix(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The character class fzf uses to tell a plain nth expression from a template:
// regexp "^[0-9,-.]+$" (`,-.` is the range ',' '-' '.').
bool is_range_chars(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || c == ',' || c == '-' || c == '.')) return false;
    }
    return true;
}

// Emits tokens with their byte and rune offsets. The scan position only ever
// moves forward, so the whole line is walked once for the rune count.
class TokenSink {
public:
    TokenSink(std::string_view line, std::vector<Token>& out) : line_(line), out_(out) {}

    void emit(size_t begin, size_t end) {
        while (scanned_ < begin) {
            if (!is_continuation(line_[scanned_])) ++runes_;
            ++scanned_;
        }
        out_.push_back(Token{line_.substr(begin, end - begin),
                             static_cast<uint32_t>(begin),
                             runes_});
    }

private:
    std::string_view line_;
    std::vector<Token>& out_;
    size_t scanned_ = 0;
    uint32_t runes_ = 0;
};

// fzf: tokenizer.go awkTokenizer. Whitespace is exactly tab, space and
// newline (fzf's `r == 9 || r == 32 || r == 10`); leading whitespace is not a
// token of its own but the first token's prefix length -- which here falls out
// of the rune offset of that token's byte offset.
void awk_tokenize(std::string_view input, TokenSink& sink) {
    enum State { Nil, Black, White };
    State state = Nil;
    size_t begin = 0, end = 0;
    for (size_t idx = 0; idx < input.size(); ++idx) {
        char r = input[idx];
        bool white = (r == 9 || r == 32 || r == 10);
        switch (state) {
            case Nil:
                if (!white) { state = Black; begin = idx; end = idx + 1; }
                break;
            case Black:
                end = idx + 1;
                if (white) state = White;
                break;
            case White:
                if (white) {
                    end = idx + 1;
                } else {
                    sink.emit(begin, end);
                    state = Black;
                    begin = idx;
                    end = idx + 1;
                }
                break;
        }
    }
    if (begin < end) sink.emit(begin, end);
}

// fzf: strings.SplitAfter(text, sep) -- every piece keeps its delimiter, and a
// line ending with the delimiter gets a final empty token.
void literal_tokenize(std::string_view input, const std::string& sep, TokenSink& sink) {
    if (sep.empty()) {
        // Go's SplitAfter with an empty separator explodes into runes and
        // returns nothing for an empty string.
        size_t i = 0;
        while (i < input.size()) {
            size_t j = i + 1;
            while (j < input.size() && is_continuation(input[j])) ++j;
            sink.emit(i, j);
            i = j;
        }
        return;
    }
    size_t pos = 0;
    for (;;) {
        size_t p = input.find(sep, pos);
        if (p == std::string_view::npos) {
            sink.emit(pos, input.size());
            return;
        }
        sink.emit(pos, p + sep.size());
        pos = p + sep.size();
    }
}

// fzf: the regex branch of Tokenize -- a token runs up to and including the
// end of each delimiter match, plus whatever trails the last match.
void regex_tokenize(std::string_view input, const std::regex& re, TokenSink& sink) {
    const char* data = input.data();
    size_t begin = 0;
    for (std::cregex_iterator it(data, data + input.size(), re), last; it != last; ++it) {
        const std::cmatch& m = *it;
        size_t match_end = static_cast<size_t>(m.position(0)) + static_cast<size_t>(m.length(0));
        sink.emit(begin, match_end);
        begin = match_end;
    }
    if (begin < input.size()) sink.emit(begin, input.size());
}

} // namespace

// fzf: tokenizer.go Tokenize / options.go delimiterRegexp
Tokenizer::Tokenizer(const Delimiter& d) : delim_(d) {
    if (!d.awk && d.is_regex) {
        try {
            re_.assign(d.pattern, std::regex::ECMAScript);
            use_regex_ = true;
        } catch (const std::regex_error&) {
            // fzf never gets here (delimiterRegexp already rejected invalid
            // patterns at parse time), but fall back the same way it does:
            // treat the pattern as a literal string.
            use_regex_ = false;
            delim_.is_regex = false;
        }
    }
}

// fzf: tokenizer.go Tokenize
void Tokenizer::tokenize(std::string_view line, std::vector<Token>& out) const {
    out.clear();
    TokenSink sink(line, out);
    if (delim_.awk) {
        awk_tokenize(line, sink);
    } else if (use_regex_) {
        regex_tokenize(line, re_, sink);
    } else {
        literal_tokenize(line, delim_.pattern, sink);
    }
}

std::vector<Token> Tokenizer::tokenize(std::string_view line) const {
    std::vector<Token> out;
    tokenize(line, out);
    return out;
}

// fzf: tokenizer.go JoinTokens
std::string join_tokens(const std::vector<Token>& tokens) {
    size_t total = 0;
    for (const Token& t : tokens) total += t.text.size();
    std::string out;
    out.reserve(total);
    for (const Token& t : tokens) out.append(t.text);
    return out;
}

namespace {

// The index arithmetic of fzf's Transform, shared by transform() and
// transform_join(). Calls `part(token_index)` for every selected token, in
// order, and returns the rune offset (fzf's prefixLength) of the part.
template <typename Fn>
uint32_t transform_range(const std::vector<Token>& tokens, const Range& r, Fn&& part) {
    const int num_tokens = static_cast<int>(tokens.size());
    int min_idx = 0;
    if (r.begin == r.end) {
        int idx = r.begin;
        if (idx == kRangeEllipsis) {
            for (int i = 0; i < num_tokens; ++i) part(i);
        } else {
            if (idx < 0) idx += num_tokens + 1;
            if (idx >= 1 && idx <= num_tokens) {
                min_idx = idx - 1;
                part(idx - 1);
            }
        }
    } else {
        int begin, end;
        if (r.begin == kRangeEllipsis) {          // ..N
            begin = 1;
            end = r.end;
            if (end < 0) end += num_tokens + 1;
        } else if (r.end == kRangeEllipsis) {     // N..
            begin = r.begin;
            end = num_tokens;
            if (begin < 0) begin += num_tokens + 1;
        } else {
            begin = r.begin;
            end = r.end;
            if (begin < 0) begin += num_tokens + 1;
            if (end < 0) end += num_tokens + 1;
        }
        min_idx = std::max(0, begin - 1);
        for (int idx = begin; idx <= end; ++idx) {
            if (idx >= 1 && idx <= num_tokens) part(idx - 1);
        }
    }
    return min_idx < num_tokens ? tokens[static_cast<size_t>(min_idx)].rune_offset : 0;
}

} // namespace

// fzf: tokenizer.go Transform
std::vector<Transformed> transform(const std::vector<Token>& tokens,
                                   const std::vector<Range>& ranges) {
    std::vector<Transformed> out;
    out.reserve(ranges.size());
    for (const Range& r : ranges) {
        Transformed t;
        size_t total = 0;
        transform_range(tokens, r, [&](int i) { total += tokens[static_cast<size_t>(i)].text.size(); });
        t.text.reserve(total);
        t.rune_offset = transform_range(tokens, r, [&](int i) {
            t.text.append(tokens[static_cast<size_t>(i)].text);
        });
        out.push_back(std::move(t));
    }
    return out;
}

// fzf: JoinTokens(Transform(tokens, ranges))
std::string transform_join(const std::vector<Token>& tokens,
                           const std::vector<Range>& ranges) {
    size_t total = 0;
    for (const Range& r : ranges) {
        transform_range(tokens, r, [&](int i) { total += tokens[static_cast<size_t>(i)].text.size(); });
    }
    std::string out;
    out.reserve(total);
    for (const Range& r : ranges) {
        transform_range(tokens, r, [&](int i) { out.append(tokens[static_cast<size_t>(i)].text); });
    }
    return out;
}

// fzf: tokenizer.go StripLastDelimiter
std::string strip_last_delimiter(std::string s, const Delimiter& d) {
    if (d.awk) {
        s.resize(trim_right_space(s));
        return s;
    }
    if (!d.is_regex) {
        if (!d.pattern.empty() && has_suffix(s, d.pattern)) s.resize(s.size() - d.pattern.size());
        return s;
    }
    const std::regex* re = delimiter_regex(d.pattern);
    if (!re) {
        if (!d.pattern.empty() && has_suffix(s, d.pattern)) s.resize(s.size() - d.pattern.size());
        return s;
    }
    size_t last_begin = 0, last_end = 0;
    bool found = false;
    for (std::cregex_iterator it(s.data(), s.data() + s.size(), *re), last; it != last; ++it) {
        const std::cmatch& m = *it;
        last_begin = static_cast<size_t>(m.position(0));
        last_end = last_begin + static_cast<size_t>(m.length(0));
        found = true;
    }
    if (found && last_end == s.size()) s.resize(last_begin);
    return s;
}

// fzf: tokenizer.go GetLastDelimiter
std::string_view get_last_delimiter(std::string_view s, const Delimiter& d) {
    if (d.awk) return std::string_view();
    if (!d.is_regex) {
        if (!d.pattern.empty() && has_suffix(s, d.pattern)) return s.substr(s.size() - d.pattern.size());
        return std::string_view();
    }
    const std::regex* re = delimiter_regex(d.pattern);
    if (!re) return std::string_view();
    size_t last_begin = 0, last_end = 0;
    bool found = false;
    for (std::cregex_iterator it(s.data(), s.data() + s.size(), *re), last; it != last; ++it) {
        const std::cmatch& m = *it;
        last_begin = static_cast<size_t>(m.position(0));
        last_end = last_begin + static_cast<size_t>(m.length(0));
        found = true;
    }
    if (found && last_end == s.size()) return s.substr(last_begin);
    return std::string_view();
}

// fzf: options.go nthTransformer
NthTransformer::NthTransformer(const std::string& expr) : expr_(expr) {
    // Plain range list: "2..", "1,3", "-1".
    if (is_range_chars(expr)) {
        plain_ = true;
        ranges_ = parse_nth(expr);      // throws OptionError like splitNth
        return;
    }

    // Template: "{...} {...} ...", where a placeholder is fzf's
    // regexp "{[0-9,-.]+}|{n}". Anything else is literal text, including a
    // brace pair whose content is not a range list.
    size_t i = 0;
    size_t literal_from = 0;
    bool any_placeholder = false;
    while (i < expr.size()) {
        if (expr[i] != '{') { ++i; continue; }
        size_t close = expr.find('}', i + 1);
        if (close == std::string::npos) break;
        std::string_view content(expr.data() + i + 1, close - i - 1);
        bool index = (content == "n");
        if (!index && !is_range_chars(content)) { ++i; continue; }

        if (literal_from < i) {
            Part p;
            p.literal = expr.substr(literal_from, i - literal_from);
            parts_.push_back(std::move(p));
        }
        any_placeholder = true;
        if (index) {
            Part p;
            p.is_index = true;
            parts_.push_back(std::move(p));
        } else {
            // fzf drops a placeholder whose ranges do not parse (its
            // `else if nth, err := splitNth(expr); err == nil` appends
            // nothing on error) -- so does this.
            try {
                Part p;
                p.nth = parse_nth(std::string(content));
                p.is_nth = true;
                parts_.push_back(std::move(p));
            } catch (const OptionError&) {
            }
        }
        i = close + 1;
        literal_from = i;
    }
    if (!any_placeholder) {
        throw OptionError{"template should include at least 1 placeholder: " + expr};
    }
    if (literal_from < expr.size()) {
        Part p;
        p.literal = expr.substr(literal_from);
        parts_.push_back(std::move(p));
    }
}

// fzf: the closure returned by nthTransformer
std::string NthTransformer::apply_raw(const std::vector<Token>& tokens, int32_t index,
                                      const Delimiter& d) const {
    if (plain_) return transform_join(tokens, ranges_);

    std::string out;
    for (const Part& p : parts_) {
        if (p.is_nth) {
            out += strip_last_delimiter(transform_join(tokens, p.nth), d);
        } else if (p.is_index) {
            if (index >= 0) out += std::to_string(index);
        } else {
            out += p.literal;
        }
    }
    return out;
}

// fzf: item.go acceptNth -- the transformer output with the trailing
// delimiter removed, which is what --accept-nth prints.
std::string NthTransformer::apply(const std::vector<Token>& tokens, int32_t index,
                                  const Delimiter& d) const {
    return strip_last_delimiter(apply_raw(tokens, index, d), d);
}

} // namespace fzf
