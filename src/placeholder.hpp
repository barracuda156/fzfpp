#pragma once

// Command-template placeholders (fzf: src/terminal.go placeholder regex,
// parsePlaceholder, hasPreviewFlags, replacePlaceholder; src/functions.go
// WriteTemporaryFile / removeFiles). See docs/DESIGN.md section 9.
//
// Grammar (the upstream regular expression, matched by a hand-written
// scanner):
//   \?(?:{[+*sfr]*[0-9,-.]*}|{q(?::s?[0-9,-.]+)?}|{fzf:(?:query|action|prompt)}|{[+*]?f?nf?})
//
//   {}            current item, single-quoted for the shell
//   {+}           every selected item (or the current one), space-joined
//   {*}           every matched item
//   {n} {+n}      input ordinals
//   {f} {+f}      path of a temp file holding the value(s), one per line
//   {r}           raw, unquoted
//   {s..}         keep leading/trailing whitespace of the field selection
//   {1} {-1} {2..} {1,3}   fields via the tokenizer (delimiter aware)
//   {q} {q:2}     the query (or its awk fields); {fzf:query} {fzf:action}
//                 {fzf:prompt}
//   \{}           a literal placeholder

#include "executor.hpp"
#include "options.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fzf {

// fzf: placeholderFlags
struct PlaceholderFlags {
    bool plus = false;
    bool asterisk = false;
    bool preserve_space = false;
    bool number = false;
    bool file = false;
    bool raw = false;
    bool force_update = false;
};

// fzf: hasPreviewFlags -- what a template needs from the item state.
struct TemplateFlags {
    bool slot = false;          // at least one non-escaped placeholder
    bool plus = false;          // {+...}
    bool asterisk = false;      // {*...}
    bool force_update = false;  // {q} / {fzf:*}: re-run even without a current item
};
TemplateFlags has_preview_flags(std::string_view tmpl);

// fzf: parsePlaceholder. `match` is one placeholder as found in the
// template (possibly with its leading backslash). Returns true when it is
// the escaped form; `stripped` receives the placeholder without its flag
// characters ("{+s1}" -> "{1}").
bool parse_placeholder(std::string_view match, std::string& stripped, PlaceholderFlags& flags);

// Locate the next placeholder at or after `pos`. Returns false when there
// is none; otherwise `start`/`len` delimit it (backslash included).
bool find_placeholder(std::string_view tmpl, size_t pos, size_t& start, size_t& len);

// One item as the expander sees it: fzf's Item.AsString(stripAnsi) plus
// Item.Index(). kMinItemIndex marks fzf's minItem (no current item but the
// template still runs because of {q}/{+} -- {} then expands to '' and {n}
// to '').
constexpr int32_t kMinItemIndex = INT32_MIN;
struct PlaceholderItem {
    std::string text;
    int32_t index = kMinItemIndex;
};

// fzf: replacePlaceholderParams. The three item lists mirror allItems:
// current (nil or one entry), selected, matched. Null pointers stand for
// nil slices.
struct PlaceholderParams {
    std::string_view tmpl;
    const Delimiter* delimiter = nullptr;      // null = awk
    std::string printsep = "\n";               // --print0: "\0"
    bool force_plus = false;
    std::string query;
    const std::vector<PlaceholderItem>* current = nullptr;
    const std::vector<PlaceholderItem>* selected = nullptr;
    const std::vector<PlaceholderItem>* matched = nullptr;
    std::string last_action;                   // {fzf:action}: kebab-case name
    std::string prompt;
    const Executor* executor = nullptr;        // null = plain POSIX quoting
};

struct Expansion {
    std::string command;
    std::vector<std::string> temp_files;   // written for {f}; caller removes them
};

// fzf: replacePlaceholder
Expansion replace_placeholder(const PlaceholderParams& params);

// fzf: WriteTemporaryFile -- join(data, printsep) + printsep into a new
// $TMPDIR/fzf-temp-XXXXXX file; returns "" when the file cannot be created.
std::string write_temporary_file(const std::vector<std::string>& data, const std::string& printsep);
// fzf: removeFiles
void remove_files(const std::vector<std::string>& files);

// Go's strings.TrimSpace (Unicode white space on both ends).
std::string trim_space(std::string_view s);

} // namespace fzf
