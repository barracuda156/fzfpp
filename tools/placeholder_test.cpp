// Unit tests for src/placeholder.cpp (T1.6): fzf's TestReplacePlaceholder
// and TestQuoteEntry fixtures (src/terminal_test.go), the placeholder
// scanner, hasPreviewFlags, temp files, and the Executor quoting rules.

#include "placeholder.hpp"

#include "ansi.hpp"
#include "options.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace fzf;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void check_eq(const std::string& actual, const std::string& expected, int line) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s:%d:\n  expected: %s\n  actual:   %s\n", __FILE__, line,
                     expected.c_str(), actual.c_str());
    }
}
#define CHECK_EQ(a, b) check_eq((a), (b), __LINE__)

// fzf: terminal_test.go newItem -- origText is the raw line, text the
// ANSI-stripped one; AsString(stripAnsi) picks between them.
struct TestItem {
    std::string raw;
    std::string stripped;
    int32_t index = 0;
};
static TestItem new_item(const std::string& s, int32_t index = 0) {
    return TestItem{s, strip_ansi(s), index};
}

struct Lists {
    std::vector<TestItem> current, selected, matched;
    bool current_nil = true, selected_nil = true, matched_nil = true;
};

static Lists items_of(std::vector<TestItem> current, std::vector<TestItem> selected,
                      std::vector<TestItem> matched, bool current_nil = false,
                      bool selected_nil = false, bool matched_nil = true) {
    Lists l;
    l.current = std::move(current);
    l.selected = std::move(selected);
    l.matched = std::move(matched);
    l.current_nil = current_nil;
    l.selected_nil = selected_nil;
    l.matched_nil = matched_nil;
    return l;
}

static std::vector<PlaceholderItem> to_items(const std::vector<TestItem>& in, bool strip_ansi_flag) {
    std::vector<PlaceholderItem> out;
    for (const auto& it : in) out.push_back(PlaceholderItem{strip_ansi_flag ? it.stripped : it.raw, it.index});
    return out;
}

// fzf: replacePlaceholderTest (with $SHELL pinned to a POSIX shell, so the
// expected strings use the Unix quoting style).
static Expansion expand(const std::string& tmpl, bool strip, const Delimiter& delim,
                        const std::string& printsep, bool force_plus, const std::string& query,
                        const Lists& lists) {
    static Executor ex = Executor::create("");
    std::vector<PlaceholderItem> cur = to_items(lists.current, strip);
    std::vector<PlaceholderItem> sel = to_items(lists.selected, strip);
    std::vector<PlaceholderItem> all = to_items(lists.matched, strip);
    PlaceholderParams p;
    p.tmpl = tmpl;
    p.delimiter = &delim;
    p.printsep = printsep;
    p.force_plus = force_plus;
    p.query = query;
    p.current = lists.current_nil ? nullptr : &cur;
    p.selected = lists.selected_nil ? nullptr : &sel;
    p.matched = lists.matched_nil ? nullptr : &all;
    p.last_action = "backward-delete-char-eof";
    p.prompt = "prompt";
    p.executor = &ex;
    return replace_placeholder(p);
}

// Expands the {{.O}} / {{.I}} / {{.S}} template markers of the Go tests
// with the Unix quoting style.
static std::string fmt(const std::string& format) {
    std::string out;
    for (size_t i = 0; i < format.size();) {
        if (format.compare(i, 6, "{{.O}}") == 0) { out += "'"; i += 6; }
        else if (format.compare(i, 6, "{{.I}}") == 0) { out += "'\\''"; i += 6; }
        else if (format.compare(i, 6, "{{.S}}") == 0) { out += "\n"; i += 6; }
        else { out += format[i]; ++i; }
    }
    return out;
}

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static Delimiter str_delim(const std::string& s) {
    Delimiter d;
    d.awk = false;
    d.is_regex = false;
    d.pattern = s;
    return d;
}
static Delimiter regex_delim(const std::string& s) {
    Delimiter d;
    d.awk = false;
    d.is_regex = true;
    d.pattern = s;
    return d;
}

// fzf: TestReplacePlaceholder
static void test_replace_placeholder() {
    setenv("SHELL", "sh", 1);
    TestItem item1 = new_item("  foo'bar \x1b[31mbaz\x1b[m");
    Lists items1 = items_of({item1}, {item1}, {});
    Lists items2 = items_of({new_item("foo'bar \x1b[31mbaz\x1b[m")},
                            {new_item("foo'bar \x1b[31mbaz\x1b[m"), new_item("FOO'BAR \x1b[31mBAZ\x1b[m")}, {});
    Delimiter none;
    Delimiter delim = str_delim("'");
    const std::string printsep = "\n";

    // {}, preserve ansi
    CHECK_EQ(expand("echo {}", false, none, printsep, false, "query", items1).command,
             fmt("echo {{.O}}  foo{{.I}}bar \x1b[31mbaz\x1b[m{{.O}}"));
    // {}, strip ansi
    CHECK_EQ(expand("echo {}", true, none, printsep, false, "query", items1).command,
             fmt("echo {{.O}}  foo{{.I}}bar baz{{.O}}"));
    // {r}, strip ansi
    CHECK_EQ(expand("echo {r}", true, none, printsep, false, "query", items1).command,
             fmt("echo   foo'bar baz"));
    // {r..}, strip ansi
    CHECK_EQ(expand("echo {r..}", true, none, printsep, false, "query", items1).command,
             fmt("echo foo'bar baz"));
    // {}, with multiple items
    CHECK_EQ(expand("echo {}", true, none, printsep, false, "query", items2).command,
             fmt("echo {{.O}}foo{{.I}}bar baz{{.O}}"));
    // {..}, strip leading whitespaces, preserve ansi
    CHECK_EQ(expand("echo {..}", false, none, printsep, false, "query", items1).command,
             fmt("echo {{.O}}foo{{.I}}bar \x1b[31mbaz\x1b[m{{.O}}"));
    // {..}, strip leading whitespaces, strip ansi
    CHECK_EQ(expand("echo {..}", true, none, printsep, false, "query", items1).command,
             fmt("echo {{.O}}foo{{.I}}bar baz{{.O}}"));
    // {q}
    CHECK_EQ(expand("echo {} {q}", true, none, printsep, false, "query", items1).command,
             fmt("echo {{.O}}  foo{{.I}}bar baz{{.O}} {{.O}}query{{.O}}"));
    // {q}, multiple items
    CHECK_EQ(expand("echo {+}{q}{+}", true, none, printsep, false, "query 'string'", items2).command,
             fmt("echo {{.O}}foo{{.I}}bar baz{{.O}} {{.O}}FOO{{.I}}BAR BAZ{{.O}}{{.O}}query {{.I}}string{{.I}}{{.O}}{{.O}}foo{{.I}}bar baz{{.O}} {{.O}}FOO{{.I}}BAR BAZ{{.O}}"));
    CHECK_EQ(expand("echo {}{q}{}", true, none, printsep, false, "query 'string'", items2).command,
             fmt("echo {{.O}}foo{{.I}}bar baz{{.O}}{{.O}}query {{.I}}string{{.I}}{{.O}}{{.O}}foo{{.I}}bar baz{{.O}}"));
    CHECK_EQ(expand("echo {1}/{2}/{2,1}/{-1}/{-2}/{}/{..}/{n.t}/\\{}/\\{1}/\\{q}/{3}", true, none, printsep, false, "query", items1).command,
             fmt("echo {{.O}}foo{{.I}}bar{{.O}}/{{.O}}baz{{.O}}/{{.O}}bazfoo{{.I}}bar{{.O}}/{{.O}}baz{{.O}}/{{.O}}foo{{.I}}bar{{.O}}/{{.O}}  foo{{.I}}bar baz{{.O}}/{{.O}}foo{{.I}}bar baz{{.O}}/{n.t}/{}/{1}/{q}/{{.O}}{{.O}}"));
    CHECK_EQ(expand("echo {1}/{2}/{-1}/{-2}/{..}/{n.t}/\\{}/\\{1}/\\{q}/{3}", true, none, printsep, false, "query", items2).command,
             fmt("echo {{.O}}foo{{.I}}bar{{.O}}/{{.O}}baz{{.O}}/{{.O}}baz{{.O}}/{{.O}}foo{{.I}}bar{{.O}}/{{.O}}foo{{.I}}bar baz{{.O}}/{n.t}/{}/{1}/{q}/{{.O}}{{.O}}"));
    CHECK_EQ(expand("echo {+1}/{+2}/{+-1}/{+-2}/{+..}/{n.t}/\\{}/\\{1}/\\{q}/{+3}", true, none, printsep, false, "query", items2).command,
             fmt("echo {{.O}}foo{{.I}}bar{{.O}} {{.O}}FOO{{.I}}BAR{{.O}}/{{.O}}baz{{.O}} {{.O}}BAZ{{.O}}/{{.O}}baz{{.O}} {{.O}}BAZ{{.O}}/{{.O}}foo{{.I}}bar{{.O}} {{.O}}FOO{{.I}}BAR{{.O}}/{{.O}}foo{{.I}}bar baz{{.O}} {{.O}}FOO{{.I}}BAR BAZ{{.O}}/{n.t}/{}/{1}/{q}/{{.O}}{{.O}} {{.O}}{{.O}}"));
    // forcePlus
    CHECK_EQ(expand("echo {1}/{2}/{-1}/{-2}/{..}/{n.t}/\\{}/\\{1}/\\{q}/{3}", true, none, printsep, true, "query", items2).command,
             fmt("echo {{.O}}foo{{.I}}bar{{.O}} {{.O}}FOO{{.I}}BAR{{.O}}/{{.O}}baz{{.O}} {{.O}}BAZ{{.O}}/{{.O}}baz{{.O}} {{.O}}BAZ{{.O}}/{{.O}}foo{{.I}}bar{{.O}} {{.O}}FOO{{.I}}BAR{{.O}}/{{.O}}foo{{.I}}bar baz{{.O}} {{.O}}FOO{{.I}}BAR BAZ{{.O}}/{n.t}/{}/{1}/{q}/{{.O}}{{.O}} {{.O}}{{.O}}"));

    // Whitespace preserving flag with "'" delimiter
    CHECK_EQ(expand("echo {s1}", true, delim, printsep, false, "query", items1).command, fmt("echo {{.O}}  foo{{.O}}"));
    CHECK_EQ(expand("echo {s2}", true, delim, printsep, false, "query", items1).command, fmt("echo {{.O}}bar baz{{.O}}"));
    CHECK_EQ(expand("echo {s}", true, delim, printsep, false, "query", items1).command, fmt("echo {{.O}}  foo{{.I}}bar baz{{.O}}"));
    CHECK_EQ(expand("echo {s..}", true, delim, printsep, false, "query", items1).command, fmt("echo {{.O}}  foo{{.I}}bar baz{{.O}}"));

    // Whitespace preserving flag with regex delimiter
    Delimiter regex = regex_delim("\\w+");
    CHECK_EQ(expand("echo {s1}", true, regex, printsep, false, "query", items1).command, fmt("echo {{.O}}  {{.O}}"));
    CHECK_EQ(expand("echo {s2}", true, regex, printsep, false, "query", items1).command, fmt("echo {{.O}}{{.I}}{{.O}}"));
    CHECK_EQ(expand("echo {s3}", true, regex, printsep, false, "query", items1).command, fmt("echo {{.O}} {{.O}}"));

    // No match
    Lists nothing;
    CHECK_EQ(expand("echo {}/{+}", true, none, printsep, false, "query", nothing).command, "echo /");
    // No match, but with selections
    Lists sel_only = items_of({}, {item1}, {}, true, false, true);
    CHECK_EQ(expand("echo {}/{+}", true, none, printsep, false, "query", sel_only).command,
             fmt("echo /{{.O}}  foo{{.I}}bar baz{{.O}}"));

    // String delimiter
    CHECK_EQ(expand("echo {}/{1}/{2}", true, delim, printsep, false, "query", items1).command,
             fmt("echo {{.O}}  foo{{.I}}bar baz{{.O}}/{{.O}}foo{{.O}}/{{.O}}bar baz{{.O}}"));
    // Regex delimiter
    regex = regex_delim("[oa]+");
    CHECK_EQ(expand("echo {}/{1}/{3}/{2..3}", true, regex, printsep, false, "query", items1).command,
             fmt("echo {{.O}}  foo{{.I}}bar baz{{.O}}/{{.O}}f{{.O}}/{{.O}}r b{{.O}}/{{.O}}{{.I}}bar b{{.O}}"));

    // Single placeholders, focus on flags (fzf: items3)
    Lists items3 = items_of({new_item("1a 1b 1c 1d 1e 1f")},
                            {new_item("1a 1b 1c 1d 1e 1f"), new_item("2a 2b 2c 2d 2e 2f"),
                             new_item("3a 3b 3c 3d 3e 3f"), new_item("4a 4b 4c 4d 4e 4f"),
                             new_item("5a 5b 5c 5d 5e 5f"), new_item("6a 6b 6c 6d 6e 6f"),
                             new_item("7a 7b 7c 7d 7e 7f")}, {});
    const bool strip = false;
    const bool force_plus = false;
    const std::string query = "sample query";
    std::map<std::string, std::string> to_output;
    std::map<std::string, std::string> to_file;
    // I. item type placeholder
    to_output["{}"] = "{{.O}}1a 1b 1c 1d 1e 1f{{.O}}";
    to_output["{+}"] = "{{.O}}1a 1b 1c 1d 1e 1f{{.O}} {{.O}}2a 2b 2c 2d 2e 2f{{.O}} {{.O}}3a 3b 3c 3d 3e 3f{{.O}} {{.O}}4a 4b 4c 4d 4e 4f{{.O}} {{.O}}5a 5b 5c 5d 5e 5f{{.O}} {{.O}}6a 6b 6c 6d 6e 6f{{.O}} {{.O}}7a 7b 7c 7d 7e 7f{{.O}}";
    to_output["{n}"] = "0";
    to_output["{+n}"] = "0 0 0 0 0 0 0";
    to_file["{f}"] = "1a 1b 1c 1d 1e 1f{{.S}}";
    to_file["{+f}"] = "1a 1b 1c 1d 1e 1f{{.S}}2a 2b 2c 2d 2e 2f{{.S}}3a 3b 3c 3d 3e 3f{{.S}}4a 4b 4c 4d 4e 4f{{.S}}5a 5b 5c 5d 5e 5f{{.S}}6a 6b 6c 6d 6e 6f{{.S}}7a 7b 7c 7d 7e 7f{{.S}}";
    to_file["{nf}"] = "0{{.S}}";
    to_file["{+nf}"] = "0{{.S}}0{{.S}}0{{.S}}0{{.S}}0{{.S}}0{{.S}}0{{.S}}";
    // II. token type placeholders
    to_output["{..}"] = to_output["{}"];
    to_output["{1..}"] = to_output["{}"];
    to_output["{..2}"] = "{{.O}}1a 1b{{.O}}";
    to_output["{1..2}"] = to_output["{..2}"];
    to_output["{-2..-1}"] = "{{.O}}1e 1f{{.O}}";
    to_output["{1}"] = "{{.O}}1a{{.O}}";
    to_output["{1..1}"] = to_output["{1}"];
    to_output["{-6}"] = to_output["{1}"];
    to_output["{1,2}"] = to_output["{1..2}"];
    to_output["{1,2,4}"] = "{{.O}}1a 1b 1d{{.O}}";
    to_output["{1,2..4}"] = "{{.O}}1a 1b 1c 1d{{.O}}";
    to_output["{1..2,-4..-3}"] = "{{.O}}1a 1b 1c 1d{{.O}}";
    to_output["{+1}"] = "{{.O}}1a{{.O}} {{.O}}2a{{.O}} {{.O}}3a{{.O}} {{.O}}4a{{.O}} {{.O}}5a{{.O}} {{.O}}6a{{.O}} {{.O}}7a{{.O}}";
    to_output["{+-1}"] = "{{.O}}1f{{.O}} {{.O}}2f{{.O}} {{.O}}3f{{.O}} {{.O}}4f{{.O}} {{.O}}5f{{.O}} {{.O}}6f{{.O}} {{.O}}7f{{.O}}";
    to_output["{s1}"] = "{{.O}}1a {{.O}}";
    to_file["{f1}"] = "1a{{.S}}";
    to_output["{+s1..2}"] = "{{.O}}1a 1b {{.O}} {{.O}}2a 2b {{.O}} {{.O}}3a 3b {{.O}} {{.O}}4a 4b {{.O}} {{.O}}5a 5b {{.O}} {{.O}}6a 6b {{.O}} {{.O}}7a 7b {{.O}}";
    to_file["{+sf1..2}"] = "1a 1b {{.S}}2a 2b {{.S}}3a 3b {{.S}}4a 4b {{.S}}5a 5b {{.S}}6a 6b {{.S}}7a 7b {{.S}}";
    // III. query type placeholder
    to_output["{q}"] = "{{.O}}" + query + "{{.O}}";
    to_output["{fzf:query}"] = "{{.O}}" + query + "{{.O}}";
    to_output["{fzf:action} {fzf:prompt}"] = "backward-delete-char-eof {{.O}}prompt{{.O}}";
    // IV. escaping placeholder
    to_output["\\{}"] = "{}";
    to_output["\\{q}"] = "{q}";
    to_output["\\{fzf:query}"] = "{fzf:query}";
    to_output["\\{fzf:action}"] = "{fzf:action}";
    to_output["\\{++}"] = "{++}";
    to_output["{++}"] = to_output["{+}"];

    for (const auto& [tmpl, want] : to_output) {
        CHECK_EQ(expand(tmpl, strip, none, printsep, force_plus, query, items3).command, fmt(want));
    }
    for (const auto& [tmpl, want] : to_file) {
        Expansion e = expand(tmpl, strip, none, printsep, force_plus, query, items3);
        CHECK(e.temp_files.size() == 1 && e.temp_files[0] == e.command);
        CHECK_EQ(read_file(e.command), fmt(want));
        remove_files(e.temp_files);
        CHECK(access(e.command.c_str(), F_OK) != 0);
    }

    // {q:N} selects awk fields of the query
    CHECK_EQ(expand("{q:1} {q:2..} {q:s2}", true, none, printsep, false, "a b  c", items3).command,
             "'a' 'b  c' 'b  '");
    // --print0 uses NUL as the temp-file separator
    {
        Expansion e = expand("{+f}", true, none, std::string("\0", 1), false, query, items2);
        CHECK_EQ(read_file(e.command), std::string("foo'bar baz\0FOO'BAR BAZ\0", 24));
        remove_files(e.temp_files);
    }
}

// fzf: TestQuoteEntry (Unix style)
static void test_quote_entry() {
    setenv("SHELL", "sh", 1);
    Executor ex = Executor::create("");
    std::map<std::string, std::string> tests = {
        {"'", "'{{.SQ}}'"}, {"\"", "'\"'"}, {"\\", "'\\'"}, {"\\\"", "'\\\"'"},
        {"\"\\\\\\\"", "'\"\\\\\\\"'"},
        {"$", "'$'"}, {"$HOME", "'$HOME'"}, {"'$HOME'", "'{{.SQ}}$HOME{{.SQ}}'"},
        {"&", "'&'"}, {"|", "'|'"}, {"<", "'<'"}, {">", "'>'"}, {"(", "'('"}, {")", "')'"},
        {"@", "'@'"}, {"^", "'^'"}, {"%", "'%'"}, {"!", "'!'"},
        {"%USERPROFILE%", "'%USERPROFILE%'"},
        {"C:\\Program Files (x86)\\", "'C:\\Program Files (x86)\\'"},
        {"\"C:\\Program Files\"", "'\"C:\\Program Files\"'"},
    };
    for (const auto& [input, want] : tests) {
        std::string expected;
        for (size_t i = 0; i < want.size();) {
            if (want.compare(i, 7, "{{.SQ}}") == 0) { expected += "'\\''"; i += 7; }
            else { expected += want[i]; ++i; }
        }
        CHECK_EQ(ex.quote(input), expected);
    }

    // fish quoting and --with-shell argv
    Executor fish = Executor::create("/usr/bin/fish -c");
    CHECK(fish.fish && fish.shell == "/usr/bin/fish" && fish.args.size() == 1 && fish.args[0] == "-c");
    CHECK_EQ(fish.quote("it's \\ here"), "'it\\'s \\\\ here'");
    Executor py = Executor::create("python3 -u -c");
    CHECK(py.shell.size() >= 7 && py.shell.compare(py.shell.size() - 7, 7, "python3") == 0 && py.args.size() == 2 && !py.fish);
    auto argv = py.argv("print(1)");
    CHECK(argv.size() == 4 && argv[3] == "print(1)");
    setenv("SHELL", "/bin/bash", 1);
    Executor bash = Executor::create("");
    CHECK(bash.shell == "/bin/bash" && bash.args.size() == 1 && bash.args[0] == "-c");
}

static void test_scanner_and_flags() {
    size_t start = 0, len = 0;
    CHECK(find_placeholder("echo {} {+f1..2} {q:s2} {fzf:prompt} {+nf} \\{} x", 0, start, len) && start == 5 && len == 2);
    CHECK(find_placeholder("a {n.t} b {n}", 0, start, len) && start == 10 && len == 3);
    CHECK(!find_placeholder("no placeholders {x} {q:} {fzf:foo} \\{", 0, start, len));
    CHECK(find_placeholder("\\{1} tail", 0, start, len) && start == 0 && len == 4);
    // A backslash before a non-placeholder is plain text
    CHECK(find_placeholder("\\{zz} {}", 0, start, len) && start == 6 && len == 2);

    std::string stripped;
    PlaceholderFlags f;
    CHECK(!parse_placeholder("{+s1..2}", stripped, f) && stripped == "{1..2}" && f.plus && f.preserve_space && !f.file);
    CHECK(!parse_placeholder("{q:s2}", stripped, f) && stripped == "{q:2}" && f.preserve_space && f.force_update);
    CHECK(!parse_placeholder("{*nf}", stripped, f) && stripped == "{}" && f.asterisk && f.number && f.file);
    CHECK(!parse_placeholder("{fzf:action}", stripped, f) && stripped == "{fzf:action}" && f.force_update);
    CHECK(parse_placeholder("\\{}", stripped, f) && stripped == "{}");

    TemplateFlags t = has_preview_flags("echo {} {+} \\{*}");
    CHECK(t.slot && t.plus && !t.asterisk && !t.force_update);
    t = has_preview_flags("echo hi");
    CHECK(!t.slot && !t.plus);
    t = has_preview_flags("grep {q} {*}");
    CHECK(t.slot && t.asterisk && t.force_update);
    t = has_preview_flags("\\{}");
    CHECK(!t.slot);

    CHECK_EQ(trim_space("  a b \t\n"), "a b");
    CHECK_EQ(trim_space("\xC2\xA0x\xE3\x80\x80"), "x");   // NBSP / ideographic space
    CHECK_EQ(trim_space(""), "");
    CHECK_EQ(trim_space("   "), "");

    // {n} of fzf's minItem prints '' (quoted empty), other flags an empty item
    std::vector<PlaceholderItem> cur{PlaceholderItem{"", kMinItemIndex}};
    PlaceholderParams p;
    p.tmpl = "{n} {} {1}";
    p.current = &cur;
    CHECK_EQ(replace_placeholder(p).command, "'' '' ''");
}

int main() {
    test_replace_placeholder();
    test_quote_entry();
    test_scanner_and_flags();
    if (failures) {
        std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("placeholder_test: %d checks passed\n", checks);
    return 0;
}
