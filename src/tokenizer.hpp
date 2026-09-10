#pragma once

// Field tokenizer and --nth / --with-nth / --accept-nth transformation.
//
// Hand-written port of fzf's src/tokenizer.go (Tokenize, awkTokenizer,
// withPrefixLengths, Transform, JoinTokens, StripLastDelimiter) and of the
// --with-nth/--accept-nth template compiler in src/options.go
// (nthTransformer). See docs/DESIGN.md section 5.
//
// Range parsing itself lives in options.cpp (fzf: ParseRange / splitNth);
// this file only consumes the parsed `Range` values.

#include "options.hpp"

#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace fzf {

// One field of a line. `text` includes the trailing delimiter (awk: the
// trailing whitespace run), like fzf's Token. Offsets locate the token in the
// original line: byte offset, and codepoint (rune) offset for mapping match
// positions back, like fzf's prefixLength.
//
// `text` is a view into the line passed to Tokenizer::tokenize(); it stays
// valid only as long as that line does.
struct Token {
    std::string_view text;
    uint32_t byte_offset = 0;
    uint32_t rune_offset = 0;
};

// fzf: tokenizer.go Tokenize (with the Delimiter kinds of options.go
// delimiterRegexp). The regex, when the delimiter needs one, is compiled once
// here instead of per line.
class Tokenizer {
public:
    explicit Tokenizer(const Delimiter& d);

    std::vector<Token> tokenize(std::string_view line) const;
    // Same, appending into a caller-owned buffer (cleared first) so that a hot
    // loop can reuse one vector. No per-token string is allocated either way.
    void tokenize(std::string_view line, std::vector<Token>& out) const;

    const Delimiter& delimiter() const { return delim_; }
    // False when a regex delimiter failed to compile and we fell back to
    // treating the pattern literally (fzf: delimiterRegexp case 3).
    bool uses_regex() const { return use_regex_; }

private:
    Delimiter delim_;
    bool use_regex_ = false;
    std::regex re_;
};

// fzf: tokenizer.go JoinTokens -- concatenation of the token texts.
std::string join_tokens(const std::vector<Token>& tokens);

// fzf: tokenizer.go Transform -- select fields by ranges; each output part is
// the concatenation of the selected tokens (with their delimiters) and carries
// the rune offset of its first selected token (fzf's prefixLength, used later
// to map --nth match positions back onto the line).
struct Transformed {
    std::string text;
    uint32_t rune_offset = 0;
};
std::vector<Transformed> transform(const std::vector<Token>& tokens,
                                   const std::vector<Range>& ranges);

// Concatenation of the parts produced by transform(); the equivalent of fzf's
// JoinTokens(Transform(tokens, ranges)) without materializing the parts.
std::string transform_join(const std::vector<Token>& tokens,
                           const std::vector<Range>& ranges);

// The parts of transform() as codepoint spans of the tokenized line (start =
// fzf's prefixLength, len = rune count of the part), one per range, appended
// to `out` (cleared first) without materializing any string. This is what
// --nth stores per item so the matcher can restrict itself to those spans.
void transform_spans(const std::vector<Token>& tokens,
                     const std::vector<Range>& ranges,
                     std::vector<RuneRange>& out);

// fzf: util.Chars.TrimTrailingWhitespaces / strings.TrimRightFunc(s,
// unicode.IsSpace) -- the byte length of `s` without its trailing Unicode
// whitespace.
size_t trim_trailing_whitespace(std::string_view s);

// fzf: tokenizer.go StripLastDelimiter -- remove one trailing delimiter
// (awk: the trailing whitespace) from a joined string.
std::string strip_last_delimiter(std::string s, const Delimiter& d);

// fzf: tokenizer.go GetLastDelimiter -- the trailing delimiter itself, or ""
// (used by --freeze-left/--freeze-right when computing hscroll offsets).
std::string_view get_last_delimiter(std::string_view s, const Delimiter& d);

// fzf: options.go nthTransformer -- a compiled --with-nth / --accept-nth
// expression: either plain ranges ("2..", "1,3") or a template with
// {N..M}-style and {n} placeholders ("{1} - {2..}"). Throws OptionError like
// fzf for a template without placeholders, and for a bad range list.
class NthTransformer {
public:
    explicit NthTransformer(const std::string& expr);

    // fzf: item.go acceptNth -- the transformer's output with one trailing
    // delimiter removed. This is what fzf prints for --accept-nth, so
    // `--accept-nth 2` on "a b c" yields "b", not "b ".
    // `index` is the item's input ordinal for {n}; -1 means "unknown" (fzf
    // prints nothing for {n} then).
    std::string apply(const std::vector<Token>& tokens, int32_t index,
                      const Delimiter& d) const;

    // fzf's nthTransformer output verbatim, i.e. without that final strip:
    // what --with-nth feeds to the display text, which core.go then trims of
    // trailing whitespace (TrimTrailingWhitespaces) rather than of a
    // delimiter. Use this one for --with-nth, apply() for --accept-nth.
    std::string apply_raw(const std::vector<Token>& tokens, int32_t index,
                          const Delimiter& d) const;

    bool is_plain_ranges() const { return plain_; }
    const std::vector<Range>& ranges() const { return ranges_; }   // plain only
    const std::string& expr() const { return expr_; }

private:
    // fzf: nthTransformer's NthParts.
    struct Part {
        std::string literal;      // used when !is_index && nth.empty()
        std::vector<Range> nth;
        bool is_index = false;    // {n}
        bool is_nth = false;
    };

    std::string expr_;
    bool plain_ = false;
    std::vector<Range> ranges_;
    std::vector<Part> parts_;
};

} // namespace fzf
