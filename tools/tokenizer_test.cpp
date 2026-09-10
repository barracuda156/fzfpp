// Unit tests for src/tokenizer.cpp (T1.3).
//
// The first three blocks are line-by-line ports of fzf 0.74's
// src/tokenizer_test.go (TestParseRange, TestTokenize, TestTransform,
// TestTransformIndexOutOfBounds) with the same inputs, expected strings and
// prefix lengths. The rest covers the T1.3 acceptance list in docs/TASKS.md at
// the tokenizer level (--nth / --with-nth / --accept-nth behaviour) plus rune
// offsets for non-ASCII lines.
//
// Build: part of the default CMake build; run via ctest or ./tokenizer_test.

#include "tokenizer.hpp"
#include "options.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace fzf;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Delimiter awk_delimiter() { return Delimiter{}; }

// Note the string_view: Token::text points into the line, so the caller must
// keep it alive. Passing a std::string temporary here would dangle.
static std::vector<Token> tokenize(std::string_view line, const Delimiter& d) {
    return Tokenizer(d).tokenize(line);
}

static std::string text_of(const Token& t) { return std::string(t.text); }

// Returns true if parse_nth accepts the expression.
static bool nth_ok(const std::string& s, Range* out = nullptr) {
    try {
        auto r = parse_nth(s);
        if (out && r.size() == 1) *out = r[0];
        return true;
    } catch (const OptionError&) {
        return false;
    }
}

// fzf: tokenizer_test.go TestParseRange (via splitNth, which is parse_nth here)
static void test_parse_range() {
    Range r{};
    CHECK(nth_ok("..", &r) && r.begin == kRangeEllipsis && r.end == kRangeEllipsis);
    CHECK(nth_ok("3..", &r) && r.begin == 3 && r.end == kRangeEllipsis);
    CHECK(nth_ok("3..5", &r) && r.begin == 3 && r.end == 5);
    CHECK(nth_ok("-3..-5", &r) && r.begin == -3 && r.end == -5);
    CHECK(nth_ok("3", &r) && r.begin == 3 && r.end == 3);
    CHECK(!nth_ok("1..3..5"));
    CHECK(!nth_ok("-3..3"));
}

// fzf: tokenizer_test.go TestTokenize
static void test_tokenize() {
    const std::string input = "  abc: \n\t def:  ghi  ";

    // AWK-style
    auto tokens = tokenize(input, awk_delimiter());
    CHECK(tokens.size() == 3);
    CHECK(text_of(tokens[0]) == "abc: \n\t " && tokens[0].rune_offset == 2);

    // With delimiter
    tokens = tokenize(input, parse_delimiter(":"));
    CHECK(text_of(tokens[0]) == "  abc:" && tokens[0].rune_offset == 0);

    // With delimiter regex
    tokens = tokenize(input, parse_delimiter("\\s+"));
    CHECK(tokens.size() == 4);
    CHECK(text_of(tokens[0]) == "  " && tokens[0].rune_offset == 0);
    CHECK(text_of(tokens[1]) == "abc: \n\t " && tokens[1].rune_offset == 2);
    CHECK(text_of(tokens[2]) == "def:  " && tokens[2].rune_offset == 10);
    CHECK(text_of(tokens[3]) == "ghi  " && tokens[3].rune_offset == 16);
}

// fzf: tokenizer_test.go TestTransform
static void test_transform() {
    const std::string input = "  abc:  def:  ghi:  jkl";
    {
        auto tokens = tokenize(input, awk_delimiter());
        {
            auto tx = transform(tokens, parse_nth("1,2,3"));
            CHECK(transform_join(tokens, parse_nth("1,2,3")) == "abc:  def:  ghi:  ");
            CHECK(tx.size() == 3);
        }
        {
            auto ranges = parse_nth("1..2,3,2..,1");
            auto tx = transform(tokens, ranges);
            CHECK(transform_join(tokens, ranges) == "abc:  def:  ghi:  def:  ghi:  jklabc:  ");
            CHECK(tx.size() == 4);
            CHECK(tx[0].text == "abc:  def:  " && tx[0].rune_offset == 2);
            CHECK(tx[1].text == "ghi:  " && tx[1].rune_offset == 14);
            CHECK(tx[2].text == "def:  ghi:  jkl" && tx[2].rune_offset == 8);
            CHECK(tx[3].text == "abc:  " && tx[3].rune_offset == 2);
        }
    }
    {
        auto tokens = tokenize(input, parse_delimiter(":"));
        {
            auto ranges = parse_nth("1..2,3,2..,1");
            auto tx = transform(tokens, ranges);
            CHECK(transform_join(tokens, ranges) == "  abc:  def:  ghi:  def:  ghi:  jkl  abc:");
            CHECK(tx.size() == 4);
            CHECK(tx[0].text == "  abc:  def:" && tx[0].rune_offset == 0);
            CHECK(tx[1].text == "  ghi:" && tx[1].rune_offset == 12);
            CHECK(tx[2].text == "  def:  ghi:  jkl" && tx[2].rune_offset == 6);
            CHECK(tx[3].text == "  abc:" && tx[3].rune_offset == 0);
        }
    }
}

// fzf: tokenizer_test.go TestTransformIndexOutOfBounds -- must not crash.
static void test_transform_out_of_bounds() {
    std::vector<Token> none;
    auto tx = transform(none, parse_nth("1"));
    CHECK(tx.size() == 1 && tx[0].text.empty() && tx[0].rune_offset == 0);
    tx = transform(none, parse_nth("-1,..,2..3"));
    CHECK(tx.size() == 3 && tx[0].text.empty() && tx[1].text.empty() && tx[2].text.empty());
}

// join_tokens is the identity on a tokenized line (fzf: JoinTokens).
static void test_join_tokens() {
    const std::string input = "  abc: \n\t def:  ghi  ";
    CHECK(join_tokens(tokenize(input, awk_delimiter())) == "abc: \n\t def:  ghi  ");  // leading run dropped
    CHECK(join_tokens(tokenize(input, parse_delimiter(":"))) == input);
    CHECK(join_tokens(tokenize(input, parse_delimiter("\\s+"))) == input);
}

// fzf: StripLastDelimiter
static void test_strip_last_delimiter() {
    CHECK(strip_last_delimiter("b  ", awk_delimiter()) == "b");
    CHECK(strip_last_delimiter("b \n\t ", awk_delimiter()) == "b");
    CHECK(strip_last_delimiter("b", awk_delimiter()) == "b");
    CHECK(strip_last_delimiter("a:c:", parse_delimiter(":")) == "a:c");
    CHECK(strip_last_delimiter("a:c", parse_delimiter(":")) == "a:c");
    CHECK(strip_last_delimiter("a\tb\t", parse_delimiter("\\t")) == "a\tb");
    CHECK(strip_last_delimiter("a:b ", parse_delimiter("[: ]")) == "a:b");    // regex, match at end
    CHECK(strip_last_delimiter("a:b", parse_delimiter("[: ]")) == "a:b");     // regex, no match at end
    // Unicode whitespace: fzf trims with unicode.IsSpace, not just ASCII.
    CHECK(strip_last_delimiter("b\xC2\xA0", awk_delimiter()) == "b");          // U+00A0
    CHECK(strip_last_delimiter("b\xE3\x80\x80", awk_delimiter()) == "b");      // U+3000
    CHECK(strip_last_delimiter("b\xC3\xA9", awk_delimiter()) == "b\xC3\xA9");  // e-acute stays
    // fzf: GetLastDelimiter
    CHECK(get_last_delimiter("a:c:", parse_delimiter(":")) == ":");
    CHECK(get_last_delimiter("a:c", parse_delimiter(":")) == "");
    CHECK(get_last_delimiter("a:b ", parse_delimiter("[: ]")) == " ");
}

// docs/TASKS.md T1.3 acceptance, at the tokenizer level.
//   printf 'a b c\n' | fzf -f a --accept-nth 2          -> b
//   printf 'a\tb\n'  | fzf -f a -d '\t' --accept-nth 2  -> b
//   -d : --accept-nth 1,3 on a:b:c                      -> a:c
static void test_accept_nth() {
    {
        Delimiter d = awk_delimiter();
        auto tokens = tokenize("a b c", d);
        CHECK(tokens.size() == 3);
        CHECK(text_of(tokens[0]) == "a " && text_of(tokens[1]) == "b " && text_of(tokens[2]) == "c");
        // fzf's nthTransformer keeps the trailing delimiter (JoinTokens of
        // Transform); item.acceptNth then strips it, so what fzf prints is
        // "b". NthTransformer::apply is that pair fused; apply_raw is the
        // transformer alone, which is what --with-nth feeds to the display.
        NthTransformer t("2");
        CHECK(t.is_plain_ranges());
        CHECK(t.ranges().size() == 1 && t.ranges()[0] == (Range{2, 2}));
        CHECK(t.apply(tokens, -1, d) == "b");
        CHECK(t.apply_raw(tokens, -1, d) == "b ");
        CHECK(NthTransformer("..").apply(tokens, -1, d) == "a b c");
        CHECK(NthTransformer("-1").apply(tokens, -1, d) == "c");
        CHECK(NthTransformer("2..").apply(tokens, -1, d) == "b c");
    }
    {
        Delimiter d = parse_delimiter("\\t");    // -d '\t'
        CHECK(!d.awk && !d.is_regex && d.pattern == "\t");
        auto tokens = tokenize("a\tb", d);
        CHECK(tokens.size() == 2 && text_of(tokens[0]) == "a\t" && text_of(tokens[1]) == "b");
        CHECK(NthTransformer("2").apply(tokens, -1, d) == "b");
        CHECK(NthTransformer("1").apply(tokens, -1, d) == "a");   // trailing tab stripped
    }
    {
        Delimiter d = parse_delimiter(":");
        auto tokens = tokenize("a:b:c", d);
        CHECK(tokens.size() == 3);
        CHECK(text_of(tokens[0]) == "a:" && text_of(tokens[1]) == "b:" && text_of(tokens[2]) == "c");
        CHECK(transform_join(tokens, parse_nth("1,3")) == "a:c");
        CHECK(transform_join(tokens, parse_nth("2..3")) == "b:c");
        CHECK(transform_join(tokens, parse_nth("-1")) == "c");
        CHECK(transform_join(tokens, parse_nth("..")) == "a:b:c");
        CHECK(NthTransformer("1,3").apply(tokens, -1, d) == "a:c");
        CHECK(NthTransformer("1").apply(tokens, -1, d) == "a");        // one ":" stripped
        CHECK(NthTransformer("1").apply_raw(tokens, -1, d) == "a:");   // ... but not by Transform
        // A line ending with the delimiter gets a trailing empty token, like
        // Go's strings.SplitAfter.
        auto trailing = tokenize("a:b:", d);
        CHECK(trailing.size() == 3 && text_of(trailing[2]).empty());
        CHECK(NthTransformer("-1").apply(trailing, -1, d).empty());
    }
    {
        // Regex delimiter: "a:b c" splits on either character.
        Delimiter d = parse_delimiter("[: ]");
        CHECK(d.is_regex);
        auto tokens = tokenize("a:b c", d);
        CHECK(tokens.size() == 3);
        CHECK(text_of(tokens[0]) == "a:" && text_of(tokens[1]) == "b " && text_of(tokens[2]) == "c");
        CHECK(NthTransformer("2").apply(tokens, -1, d) == "b");
        CHECK(NthTransformer("1,3").apply(tokens, -1, d) == "a:c");
    }
}

// fzf: options.go nthTransformer, template branch.
static void test_nth_template() {
    Delimiter d = awk_delimiter();
    auto tokens = tokenize("a b c", d);

    NthTransformer t("{1} - {2..}");
    CHECK(!t.is_plain_ranges());
    CHECK(t.apply(tokens, -1, d) == "a - b c");
    CHECK(t.apply(tokens, 7, d) == "a - b c");

    // {n} is the item's input ordinal; -1 (unknown) prints nothing.
    CHECK(NthTransformer("{n}").apply(tokens, 7, d) == "7");
    CHECK(NthTransformer("{n}").apply(tokens, 0, d) == "0");
    CHECK(NthTransformer("{n}").apply(tokens, -1, d).empty());
    CHECK(NthTransformer("[{n}] {1}").apply(tokens, 3, d) == "[3] a");

    // Each placeholder is stripped of one trailing delimiter on its own.
    {
        Delimiter c = parse_delimiter(":");
        auto colon_tokens = tokenize("a:b:c", c);
        CHECK(NthTransformer("{1}/{2}").apply(colon_tokens, -1, c) == "a/b");
        CHECK(NthTransformer("{2..}").apply(colon_tokens, -1, c) == "b:c");
    }

    // A template needs at least one placeholder.
    bool threw = false;
    try {
        NthTransformer("abc");
    } catch (const OptionError& e) {
        threw = true;
        CHECK(e.message == "template should include at least 1 placeholder: abc");
    }
    CHECK(threw);
    threw = false;
    try {
        NthTransformer("{}");                // not a placeholder: no ranges inside
    } catch (const OptionError&) {
        threw = true;
    }
    CHECK(threw);

    // Braces that are not placeholders stay literal (fzf's placeholder regex
    // simply does not match them), and a placeholder whose ranges do not parse
    // is dropped, exactly as fzf's `else if err == nil` does.
    CHECK(NthTransformer("{a}{1}").apply(tokens, -1, d) == "{a}a");
    CHECK(NthTransformer("{1{2}").apply(tokens, -1, d) == "{1b");
    CHECK(NthTransformer("{1..2..3}{2}").apply(tokens, -1, d) == "b");
}

// Rune offsets: fzf's prefixLength counts codepoints, not bytes.
static void test_rune_offsets() {
    Delimiter d = awk_delimiter();
    auto tokens = tokenize("h\xC3\xA9llo w\xC3\xB6rld x", d);   // "héllo wörld x"
    CHECK(tokens.size() == 3);
    CHECK(tokens[0].byte_offset == 0 && tokens[0].rune_offset == 0);
    CHECK(tokens[1].byte_offset == 7 && tokens[1].rune_offset == 6);
    CHECK(tokens[2].byte_offset == 14 && tokens[2].rune_offset == 12);
    CHECK(text_of(tokens[1]) == "w\xC3\xB6rld ");

    // Leading whitespace belongs to the first token's prefix.
    tokens = tokenize("   \xC3\xA9x y", d);
    CHECK(tokens.size() == 2);
    CHECK(tokens[0].byte_offset == 3 && tokens[0].rune_offset == 3);
    CHECK(tokens[1].byte_offset == 7 && tokens[1].rune_offset == 6);

    // Same accounting with an explicit delimiter.
    Delimiter c = parse_delimiter(":");
    tokens = tokenize("\xC3\xA9""a:b\xE3\x80\x80:c", c);
    CHECK(tokens.size() == 3);
    CHECK(tokens[1].byte_offset == 4 && tokens[1].rune_offset == 3);
    CHECK(tokens[2].byte_offset == 9 && tokens[2].rune_offset == 6);

    // Transform carries the rune offset of the first selected token.
    auto tx = transform(tokens, parse_nth("2..3"));
    CHECK(tx.size() == 1 && tx[0].rune_offset == 3);
}

// Degenerate inputs must behave like Go's.
static void test_edge_cases() {
    Delimiter d = awk_delimiter();
    CHECK(tokenize("", d).empty());
    CHECK(tokenize("   ", d).empty());          // whitespace only: no tokens
    auto tokens = tokenize("  x", d);
    CHECK(tokens.size() == 1 && text_of(tokens[0]) == "x" && tokens[0].rune_offset == 2);

    Delimiter c = parse_delimiter(":");
    tokens = tokenize("", c);
    CHECK(tokens.size() == 1 && tokens[0].text.empty());   // SplitAfter("", ":") == [""]
    tokens = tokenize("::", c);
    CHECK(tokens.size() == 3 && text_of(tokens[0]) == ":" && text_of(tokens[2]).empty());

    Delimiter re = parse_delimiter("[: ]");
    CHECK(tokenize("", re).empty());            // the regex branch yields nothing

    // Out-of-range fields select nothing but keep their place in the output.
    auto three = tokenize("a b c", d);
    CHECK(NthTransformer("9").apply(three, -1, d).empty());
    CHECK(NthTransformer("-9").apply(three, -1, d).empty());
    CHECK(NthTransformer("2..9").apply(three, -1, d) == "b c");
    CHECK(transform(three, parse_nth("9")).size() == 1);

    // An invalid regex delimiter falls back to a literal match.
    Delimiter bad;
    bad.awk = false;
    bad.is_regex = true;
    bad.pattern = "[[";
    Tokenizer tk(bad);
    CHECK(!tk.uses_regex());
    auto lit = tk.tokenize("a[[b");
    CHECK(lit.size() == 2 && text_of(lit[0]) == "a[[" && text_of(lit[1]) == "b");

    // The buffer-reusing overload clears its output.
    std::vector<Token> buf;
    Tokenizer awk(d);
    awk.tokenize("a b", buf);
    awk.tokenize("z", buf);
    CHECK(buf.size() == 1 && text_of(buf[0]) == "z");
}

int main() {
    test_parse_range();
    test_tokenize();
    test_transform();
    test_transform_out_of_bounds();
    test_join_tokens();
    test_strip_last_delimiter();
    test_accept_nth();
    test_nth_template();
    test_rune_offsets();
    test_edge_cases();
    std::printf("tokenizer_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
