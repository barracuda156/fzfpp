#include "options.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <cctype>

namespace fzf {

// Split a string into shell-like words, honoring single quotes, double quotes,
// and backslash escaping. Used to parse $FZF_DEFAULT_OPTS the way fzf does.
static std::vector<std::string> shell_split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_word = false;
    enum { NONE, SINGLE, DOUBLE } q = NONE;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q == SINGLE) {
            if (c == '\'') q = NONE;
            else cur += c;
            in_word = true;
        } else if (q == DOUBLE) {
            if (c == '"') {
                q = NONE;
            } else if (c == '\\' && i + 1 < s.size() &&
                       (s[i+1] == '"' || s[i+1] == '\\' || s[i+1] == '$' || s[i+1] == '`')) {
                cur += s[++i];
            } else {
                cur += c;
            }
            in_word = true;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (in_word) { out.push_back(cur); cur.clear(); in_word = false; }
        } else if (c == '\'') {
            q = SINGLE; in_word = true;
        } else if (c == '"') {
            q = DOUBLE; in_word = true;
        } else if (c == '\\' && i + 1 < s.size()) {
            cur += s[++i]; in_word = true;
        } else {
            cur += c; in_word = true;
        }
    }
    if (in_word) out.push_back(cur);
    return out;
}

// Parse an fzf nth-spec (the value of --with-nth / --accept-nth) into ranges.
// The spec is a comma-separated list of terms, each of which is one of:
//   N        a single field (1-based; negative counts from the end, -1 = last)
//   N..M     an inclusive range
//   N..      from field N through the last field
//   ..M      from the first field through field M
//   ..       every field
// Invalid terms are skipped with a warning, matching the tolerant spirit of the
// rest of option parsing.
static std::vector<FieldRange> parse_nth_spec(const std::string& spec) {
    std::vector<FieldRange> ranges;
    std::stringstream ss(spec);
    std::string term;

    auto to_int = [](const std::string& s, int& out) -> bool {
        if (s.empty()) return false;
        try {
            size_t consumed = 0;
            int v = std::stoi(s, &consumed);
            if (consumed != s.size() || v == 0) return false;  // fzf fields are 1-based
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    };

    while (std::getline(ss, term, ',')) {
        // trim surrounding whitespace
        size_t a = term.find_first_not_of(" \t");
        size_t b = term.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        term = term.substr(a, b - a + 1);

        // Tolerate a brace-wrapped spec: yt-x passes --with-nth/--accept-nth as
        // "{2..}" (with the placeholder braces), and fzf accepts that as the
        // field range 2... Without stripping the braces, to_int("{2") failed and
        // the whole term was rejected ("Invalid field range: {2..}"), so
        // --accept-nth was ignored and fzf echoed the full delimited line
        // (field1|title) instead of just the title — breaking yt-x downstream.
        if (term.size() >= 2 && term.front() == '{' && term.back() == '}') {
            term = term.substr(1, term.size() - 2);
        }
        if (term.empty()) continue;

        FieldRange r;
        size_t dots = term.find("..");
        if (dots == std::string::npos) {
            int v;
            if (!to_int(term, v)) {
                std::cerr << "Invalid field spec: " << term << std::endl;
                continue;
            }
            r.begin = r.end = v;
        } else {
            std::string lhs = term.substr(0, dots);
            std::string rhs = term.substr(dots + 2);
            bool ok = true;
            if (lhs.empty()) { r.open_begin = true; }
            else ok = to_int(lhs, r.begin);
            if (rhs.empty()) { r.open_end = true; }
            else if (ok) ok = to_int(rhs, r.end);
            if (!ok) {
                std::cerr << "Invalid field range: " << term << std::endl;
                continue;
            }
        }
        ranges.push_back(r);
    }
    return ranges;
}

// fzf's key names are case-insensitive (`Ctrl-A`, `BTab`, `ALT-b` all work),
// but this port's bindings map and terminal.cpp's lookups are all lowercase
// (see event_to_bind_key / the "tab","home","end","btab" etc. literals).
// Normalize to lowercase, with one exception: the letter immediately after
// "alt-" must keep its original case, since fzf's alt-<letter> encodes the
// literal Meta-shifted byte (alt-B and alt-b are genuinely different keys —
// terminal.cpp's alt handling forwards the raw second byte unchanged).
static std::string normalize_bind_key(const std::string& key) {
    std::string lower = key;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // "alt-" is 4 chars; if that's really what precedes the remainder in the
    // ORIGINAL (case-sensitive) string, restore the original casing of the
    // single letter that follows.
    if (lower.size() > 4 && lower.compare(0, 4, "alt-") == 0) {
        lower[4] = key[4];
    }
    return lower;
}

Options parse_options(int argc, char* argv[]) {
    Options opts;

    // Build the effective argument list: $FZF_DEFAULT_OPTS first (so real
    // command-line args override it), then argv. Handle +i and +m here too
    // (CLI11 doesn't support the + prefix), from either source.
    bool no_multi_flag = false;
    std::vector<std::string> arg_storage;

    // Case-mode precedence in real fzf is LAST-OCCURRENCE-WINS across -i,
    // +i, and --case=X, in command-line order (after FZF_DEFAULT_OPTS
    // prepending, so a per-invocation flag overrides a default-opts one).
    // CLI11 gives us each option's own last value but not their relative
    // order against each other, so track the winner directly while walking
    // the merged, already-ordered raw_args below.
    bool case_mode_seen = false;
    CaseMode last_case_mode = CaseMode::Smart;

    arg_storage.push_back(argv[0]);

    std::vector<std::string> raw_args;
    if (const char* default_opts = std::getenv("FZF_DEFAULT_OPTS")) {
        for (auto& tok : shell_split(default_opts)) {
            raw_args.push_back(std::move(tok));
        }
    }
    for (int i = 1; i < argc; ++i) {
        raw_args.push_back(argv[i]);
    }

    for (size_t ai = 0; ai < raw_args.size(); ++ai) {
        const auto& arg = raw_args[ai];
        if (arg == "-i") {
            case_mode_seen = true;
            last_case_mode = CaseMode::Ignore;
        } else if (arg == "+i") {
            case_mode_seen = true;
            last_case_mode = CaseMode::Respect;
        } else if (arg.compare(0, 7, "--case=") == 0) {
            std::string v = arg.substr(7);
            if (v == "smart") { case_mode_seen = true; last_case_mode = CaseMode::Smart; }
            else if (v == "ignore") { case_mode_seen = true; last_case_mode = CaseMode::Ignore; }
            else if (v == "respect") { case_mode_seen = true; last_case_mode = CaseMode::Respect; }
        } else if (arg == "--case" && ai + 1 < raw_args.size()) {
            const std::string& v = raw_args[ai + 1];
            if (v == "smart") { case_mode_seen = true; last_case_mode = CaseMode::Smart; }
            else if (v == "ignore") { case_mode_seen = true; last_case_mode = CaseMode::Ignore; }
            else if (v == "respect") { case_mode_seen = true; last_case_mode = CaseMode::Respect; }
        }

        if (arg == "+m") {
            no_multi_flag = true;
        } else if (arg.size() > 2 && arg.compare(0, 2, "--") == 0 &&
                   arg.back() == '=' &&
                   arg.find('=') == arg.size() - 1) {
            // `--opt=` with an empty right-hand side is an explicit empty-string
            // assignment in fzf (e.g. yt-x's `--border-label=''`). CLI11 2.6.2
            // treats an empty `=value` as "no value given" and then reaches
            // forward to swallow the NEXT argument as the value — so
            // `--border-label= -f query` ate the `-f`, silently disabling filter
            // mode and aborting on /dev/tty. Split it into the option and a
            // separate empty-string token, which CLI11 consumes as the value.
            arg_storage.push_back(arg.substr(0, arg.size() - 1));
            arg_storage.push_back(std::string());
        } else {
            arg_storage.push_back(arg);
        }
    }

    std::vector<char*> new_argv;
    new_argv.reserve(arg_storage.size());
    for (auto& s : arg_storage) {
        new_argv.push_back(const_cast<char*>(s.c_str()));
    }
    int new_argc = static_cast<int>(new_argv.size());

    CLI::App app{"fzf++ - Command-line fuzzy finder (C++ implementation)"};

    // fzf accepts many flags this port does not implement; app configs such as
    // $FZF_DEFAULT_OPTS routinely pass them. Collect unrecognized flags (and any
    // stray values) instead of aborting, so those invocations still run. Values
    // in the --flag=value form are self-contained; a lone value after an unknown
    // flag is simply dropped, which is harmless as there is no positional arg.
    app.allow_extras();

    // fzf's semantics are "last occurrence wins" for scalar options: an option
    // set in $FZF_DEFAULT_OPTS and again on the command line (or twice on the
    // command line) is not an error — the later value overrides. yt-x relies on
    // this, setting --prompt in FZF_DEFAULT_OPTS and again per-invocation, and
    // repeating --border in its default opts. CLI11 otherwise caps an option's
    // total value count across all occurrences and aborts ("--prompt: At most 1
    // required but received 2"). Default every option to TakeLast; the few that
    // genuinely accumulate (--bind, --color) opt back into TakeAll below.
    app.option_defaults()->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);

    bool version = false;
    app.add_flag("-v,--version", version, "Show version");

    std::string case_str;
    app.add_option("--case", case_str, "Case sensitivity (smart/ignore/respect)")
        ->check(CLI::IsMember({"smart", "ignore", "respect"}));

    bool case_insensitive_flag = false;
    app.add_flag("-i", case_insensitive_flag, "Case insensitive");

    app.add_flag("-e,--exact", [&opts](int64_t) {
        opts.fuzzy = false;
    }, "Enable exact match (disable fuzzy matching)");

    app.add_flag("--fuzzy", [&opts](int64_t) {
        opts.fuzzy = true;
    }, "Enable fuzzy matching (default)");

    app.add_flag("-x,--extended", opts.extended,
                 "Enable extended search mode (default: true)");

    app.add_flag("--ansi", opts.ansi,
                 "Enable processing of ANSI color codes");

    std::string height_str;
    app.add_option("--height", height_str,
                   "Display height (lines or %, 0 for fullscreen)");

    std::string layout_str;
    app.add_option("--layout", layout_str, "Layout type (default/reverse/reverse-list)")
        ->check(CLI::IsMember({"default", "reverse", "reverse-list"}));

    app.add_flag("--reverse", [&opts](int64_t) {
        opts.layout = LayoutType::Reverse;
    }, "Shorthand for --layout=reverse");

    app.add_flag("--no-reverse", [&opts](int64_t) {
        opts.layout = LayoutType::Default;
    }, "Shorthand for --layout=default");

    app.add_option("--prompt", opts.prompt, "Input prompt string");

    app.add_option("--header", opts.header, "Header line to display");

    app.add_flag("--header-first", opts.header_first, "Print header before prompt line");

    app.add_flag("--no-header-first", [&opts](int64_t) {
        opts.header_first = false;
    }, "Print header after prompt line (default)");

    // fzf's --border takes an OPTIONAL style value: bare `--border` enables a
    // (rounded) border, `--border=STYLE` selects a style. Declaring it as a plain
    // flag made CLI11 try to convert the "=rounded" value to bool and abort
    // ("Could not convert: --border = true,rounded"), which broke yt-x and viu.
    //
    // fzf also lets the option be REPEATED, with the last occurrence winning —
    // yt-x's FZF_DEFAULT_OPTS literally carries both `--border` and
    // `--border=rounded`. CLI11 caps an option's total value count at its
    // `expected` max across all occurrences, so `expected(0,1)` rejected the
    // second one ("At most 1 required but received 2"). Accept any number of
    // values (0 or 1 per occurrence, unbounded occurrences) and use the last
    // non-empty style seen.
    app.add_option_function<std::vector<std::string>>(
        "--border",
        [&opts](const std::vector<std::string>& vals) {
            opts.border = true;
            for (auto it = vals.rbegin(); it != vals.rend(); ++it) {
                if (!it->empty()) {
                    opts.border_style = *it;
                    break;
                }
            }
        },
        "Draw border around interface (optional style, e.g. rounded/sharp/none)")
        ->expected(0, -1);


    app.add_flag("--wrap", opts.wrap, "Enable line wrapping");

    app.add_flag("--no-mouse", opts.no_mouse, "Disable mouse");

    app.add_flag("--no-unicode", opts.no_unicode, "Disable unicode");


    std::string margin_str;
    app.add_option("--margin", margin_str, "Margins (top,right,bottom,left)");

    std::string info_str;
    app.add_option("--info", info_str, "Info display mode (default/inline/hidden)");

    std::vector<std::string> color_specs;
    app.add_option("--color", color_specs, "Color scheme")
        ->allow_extra_args()
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);

    app.add_option("--border-label", opts.border_label, "Border label text");
    app.add_option("--marker", opts.marker, "Multi-select marker");
    app.add_option("--pointer", opts.pointer, "Pointer to current line");
    app.add_option("--separator", opts.separator, "Separator character");
    app.add_option("--scrollbar", opts.scrollbar, "Scrollbar character");
    app.add_option("--tabstop", opts.tabstop, "Tab stop width");

    app.add_flag("-m,--multi", opts.multi, "Enable multi-select mode");

    app.add_flag("--no-multi", [&opts](int64_t) {
        opts.multi = false;
    }, "Disable multi-select mode");

    app.add_flag("--cycle", opts.cycle, "Enable cyclic scrolling");

    app.add_option("-q,--query", opts.query, "Initial query string");

    std::string filter_query;
    CLI::Option* filter_opt =
        app.add_option("-f,--filter", filter_query,
                       "Filter mode (non-interactive, print matches)");

    app.add_flag("-1,--select-1", opts.select_1,
                 "Auto-select if only one match");

    app.add_flag("-0,--exit-0", opts.exit_0,
                 "Exit immediately if no match");

    app.add_flag("--print-query", opts.print_query,
                 "Print query before results");

    app.add_flag("--no-sort", [&opts](int64_t) {
        opts.sort = false;
    }, "Disable sorting");

    app.add_option("-d,--delimiter", opts.delimiter,
                   "Field delimiter (regex)");

    std::string with_nth_str;
    app.add_option("--with-nth", with_nth_str,
                   "Display only specified fields (e.g. 2,3 or 2.. or -1)");

    std::string accept_nth_str;
    app.add_option("--accept-nth", accept_nth_str,
                   "Print only specified fields on accept (e.g. 2.. or -1)");

    std::vector<std::string> bind_specs;
    app.add_option("--bind", bind_specs,
                   "Custom key bindings (key:action)")
        ->allow_extra_args()
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);

    // fzf semantics: repeated --expect ACCUMULATE (each occurrence adds keys)
    // rather than the last one replacing the others. Opt into TakeAll like
    // --bind/--color and merge every occurrence's comma-separated keys below.
    std::vector<std::string> expect_specs;
    app.add_option("--expect", expect_specs,
                   "Comma-separated list of keys that trigger exit with key name")
        ->allow_extra_args()
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);

    app.add_option("--with-shell", opts.with_shell,
                   "Shell to use for execute actions");

    app.add_flag("--read0", opts.read_zero,
                 "Read null-delimited input");

    app.add_option("--preview", opts.preview_command,
                   "Preview command");

    app.add_option("--preview-window", opts.preview_window,
                   "Preview window options");

    app.add_flag("--disabled", opts.disabled,
                 "Do not filter by the query; only track it for {q}/change: events");

    try {
        app.parse(new_argc, new_argv.data());
    } catch (const CLI::ParseError &e) {
        std::exit(app.exit(e));
    }

    // Parse --with-nth / --accept-nth field specs (supports ranges like 2..)
    if (!with_nth_str.empty()) {
        opts.with_nth = parse_nth_spec(with_nth_str);
    }
    if (!accept_nth_str.empty()) {
        opts.accept_nth = parse_nth_spec(accept_nth_str);
    }

    // Key-taking actions whose argument extends to the END OF THE SPEC — fzf
    // does not comma-split inside them, unlike the parenthesized action form.
    // e.g. `--bind 'focus:transform-header:case $a in a,b) ... esac'` keeps
    // the comma in the shell case statement as part of the argument.
    static const std::vector<std::string> trailing_colon_actions = {
        "reload", "preview", "change-preview", "change-prompt", "change-header",
        "transform-header", "transform", "execute", "execute-silent",
        "become", "unbind", "rebind",
    };

    // Returns the length of a trailing-colon action name if the segment
    // starting at spec[seg_start] is exactly one of trailing_colon_actions
    // immediately followed by ':' at spec[seg_start+len], else 0. seg_start
    // is the start of the current action-name segment (after the key's
    // colon, or after a '+' composite separator) — NOT the start of the
    // whole key:action pair.
    auto match_trailing_colon_action = [](const std::string& s, size_t seg_start) -> size_t {
        for (const auto& name : trailing_colon_actions) {
            size_t n = name.size();
            if (seg_start + n < s.size() && s.compare(seg_start, n, name) == 0 &&
                s[seg_start + n] == ':') {
                return n;
            }
        }
        return 0;
    };

    // Parse --bind specifications. A single --bind argument may itself hold
    // multiple comma-separated key:action pairs (e.g. yt-x's and viu's
    // "ctrl-/:toggle-preview,ctrl-space:toggle-wrap+toggle-preview-wrap"), so
    // split on top-level commas first. Commas inside an action's
    // parenthesized/bracketed argument (e.g. execute(echo a,b), or fzf's
    // alternate execute[...]/execute{...} delimiters) must not be split on,
    // so track paren/bracket depth while scanning. Additionally, once we
    // recognize a trailing-colon arg-taking action (NAME: with no bracket,
    // e.g. focus:transform-header:CMD), the rest of the ENTIRE spec string
    // is that action's argument — stop splitting altogether for the
    // remainder, including any commas inside it.
    for (const auto& spec : bind_specs) {
        size_t start = 0;      // start of the current key:action pair
        size_t seg_start = 0;  // start of the current action-NAME segment
        bool seg_start_valid = false;  // false while still scanning the key part
        int depth = 0;
        bool in_trailing_arg = false;  // rest of spec is one action's argument
        for (size_t i = 0; i <= spec.size(); ++i) {
            bool at_end = (i == spec.size());
            char c = at_end ? '\0' : spec[i];

            if (!in_trailing_arg) {
                if (c == '(' || c == '[' || c == '{') {
                    depth++;
                } else if (c == ')' || c == ']' || c == '}') {
                    if (depth > 0) depth--;
                } else if (c == ':' && depth == 0) {
                    if (!seg_start_valid) {
                        // This is the key/action separator; the action-name
                        // segment begins right after it.
                        seg_start_valid = true;
                        seg_start = i + 1;
                    } else {
                        // A second top-level ':' — check whether the segment
                        // since the last '+' (or since the key colon) is one
                        // of the arg-taking action names.
                        size_t n = match_trailing_colon_action(spec, seg_start);
                        if (n > 0 && seg_start + n == i) {
                            in_trailing_arg = true;
                        }
                    }
                } else if (c == '+' && depth == 0 && seg_start_valid) {
                    // Composite separator between actions (e.g.
                    // "toggle+preview:CMD" is not real fzf syntax for
                    // trailing-colon actions, but keep the segment tracking
                    // consistent so a future action name after '+' is still
                    // recognized at its own boundary).
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
                    if (!key.empty()) {
                        opts.bindings[normalize_bind_key(key)] = action;
                    }
                }
                if (in_trailing_arg) break;  // remainder already consumed as one pair
            }
        }
    }

    // fzf binds ctrl-c/ctrl-g/ctrl-q (and esc) to abort by default. Under raw
    // mode ^C no longer raises SIGINT, so without an explicit binding the
    // event loop just repaints and the app looks frozen — real consumers
    // (ytsurf) hit this. Seed the defaults only where the user hasn't already
    // bound the key, so an explicit `--bind ctrl-c:...` still wins. (esc is
    // handled directly in the event loop, so it isn't seeded here.)
    for (const char* key : {"ctrl-c", "ctrl-g", "ctrl-q"}) {
        opts.bindings.emplace(key, "abort");
    }

    // Seed fzf's remaining default keymap (readline-style line editing plus
    // the standard scroll/toggle keys). emplace() is a no-op where the key
    // is already bound, so an explicit --bind for any of these still wins.
    // terminal.cpp implements the actual key->action dispatch separately;
    // unknown action strings are currently no-ops there, so landing the
    // bindings here first is safe even before that dispatch exists.
    static const std::pair<const char*, const char*> default_binds[] = {
        {"ctrl-j", "down"},
        {"ctrl-k", "up"},
        {"ctrl-p", "up"},
        {"ctrl-n", "down"},
        {"ctrl-u", "unix-line-discard"},
        {"ctrl-w", "unix-word-rubout"},
        {"ctrl-a", "beginning-of-line"},
        {"ctrl-e", "end-of-line"},
        {"ctrl-b", "backward-char"},
        {"ctrl-f", "forward-char"},
        {"ctrl-d", "delete-char/eof"},
        {"ctrl-h", "backward-delete-char"},
        {"alt-b", "backward-word"},
        {"alt-f", "forward-word"},
        {"alt-d", "kill-word"},
        {"alt-bs", "backward-kill-word"},
        {"btab", "toggle+up"},
        {"tab", "toggle+down"},
        {"home", "first"},
        {"end", "last"},
    };
    for (const auto& [key, action] : default_binds) {
        opts.bindings.emplace(key, action);
    }

    if (version) {
        std::cout << "fzf++ version 0.2.1 (C++20 implementation)" << std::endl;
        std::cout << "Compatible with fzf" << std::endl;
        std::exit(0);
    }

    // Detect PRESENCE of -f/--filter, not string emptiness: `fzf -f ''` is a
    // valid (and common) way to ask for filter mode with an empty query
    // (print every line non-interactively), and must not fall through to the
    // interactive TUI.
    if (filter_opt->count() > 0) {
        opts.filter = true;
        opts.query = filter_query;
    }

    // fzf's --margin takes 1, 2, or 4 comma-separated values (each an
    // absolute line/col count or a percentage): 1 = all sides; 2 =
    // vertical,horizontal; 4 = top,right,bottom,left.
    if (!margin_str.empty()) {
        std::stringstream ss(margin_str);
        std::string value;
        std::vector<Options::Margin> margins;
        while (std::getline(ss, value, ',')) {
            Options::Margin m;
            std::string num_str = value;
            if (!num_str.empty() && num_str.back() == '%') {
                m.percent = true;
                num_str.pop_back();
            }
            try {
                size_t consumed = 0;
                m.value = std::stoi(num_str, &consumed);
                if (consumed != num_str.size()) throw std::invalid_argument("trailing");
            } catch (...) {
                std::cerr << "Invalid margin value: " << value << std::endl;
                continue;
            }
            margins.push_back(m);
        }
        if (margins.size() == 1) {
            opts.margin_top = opts.margin_right = opts.margin_bottom = opts.margin_left = margins[0];
        } else if (margins.size() == 2) {
            opts.margin_top = opts.margin_bottom = margins[0];
            opts.margin_right = opts.margin_left = margins[1];
        } else if (margins.size() == 4) {
            opts.margin_top = margins[0];
            opts.margin_right = margins[1];
            opts.margin_bottom = margins[2];
            opts.margin_left = margins[3];
        } else if (!margins.empty()) {
            std::cerr << "Invalid --margin: expected 1, 2, or 4 values, got "
                      << margins.size() << std::endl;
        }
    }

    // Case-mode: last occurrence among -i / +i / --case wins, in the order
    // they appeared across FZF_DEFAULT_OPTS + argv (tracked while walking
    // raw_args above), NOT a fixed -i-then-+i-then---case precedence.
    if (case_mode_seen) {
        opts.case_mode = last_case_mode;
    }

    if (no_multi_flag) {
        opts.multi = false;
    }

    if (!layout_str.empty()) {
        if (layout_str == "default") {
            opts.layout = LayoutType::Default;
        } else if (layout_str == "reverse") {
            opts.layout = LayoutType::Reverse;
        } else if (layout_str == "reverse-list") {
            opts.layout = LayoutType::Reverse;
        }
    }

    if (!height_str.empty()) {
        // fzf's "adaptive height" prefix (--height=~40%): grow/shrink to fit
        // content up to the given cap. This port has no adaptive-resize
        // logic, so approximate by treating it as the same fixed size cap —
        // silently (no warning), same as the rest of this tolerant parser.
        std::string h = height_str;
        if (!h.empty() && h.front() == '~') {
            h.erase(0, 1);
        }
        bool is_percent = !h.empty() && h.back() == '%';
        std::string num_str = is_percent ? h.substr(0, h.size() - 1) : h;
        try {
            if (num_str.empty()) throw std::invalid_argument("empty");
            size_t consumed = 0;
            int v = std::stoi(num_str, &consumed);
            if (consumed != num_str.size()) throw std::invalid_argument("trailing");
            opts.height = v;
            opts.height_is_percent = is_percent;
            if (is_percent) {
                opts.height = std::clamp(opts.height, 0, 100);
            } else if (opts.height < 0) {
                opts.height = 0;
            }
        } catch (...) {
            // Unparseable height: fall back to fullscreen cleanly, without
            // printing a warning (would land in the alt screen / corrupt the
            // TUI frame rather than reach a terminal the user can read).
            opts.height = 0;
            opts.height_is_percent = false;
        }
    }

    if (!info_str.empty()) {
        if (info_str == "hidden") {
            opts.info_hidden = true;
        }
    }

    // Parse --expect: each occurrence is a comma-separated key list; fzf
    // merges every occurrence (repeated --expect accumulates) rather than
    // the last one replacing the others. Trim whitespace and dedupe while
    // preserving first-seen order.
    for (const auto& expect_str : expect_specs) {
        std::stringstream ss(expect_str);
        std::string key;
        while (std::getline(ss, key, ',')) {
            size_t a = key.find_first_not_of(" \t");
            size_t b = key.find_last_not_of(" \t");
            if (a == std::string::npos) continue;
            key = key.substr(a, b - a + 1);
            if (key.empty()) continue;
            if (std::find(opts.expect_keys.begin(), opts.expect_keys.end(), key) ==
                opts.expect_keys.end()) {
                opts.expect_keys.push_back(key);
            }
        }
    }

    // Process --preview-window. fzf's classic syntax is actually
    // colon-separated (e.g. "up:60%:wrap"), though this port has
    // historically also accepted commas (e.g. the clifm-style
    // "border-rounded,left,35%,wrap" yt-x passes) — tokenize on BOTH so
    // either form (or a mix) works, matching real fzf's tolerance.
    if (!opts.preview_window.empty()) {
        std::string spec = opts.preview_window;
        // Normalize ':' to ',' then split once on ',', to avoid a second
        // stringstream pass.
        for (char& c : spec) {
            if (c == ':') c = ',';
        }
        std::stringstream ss(spec);
        std::string part;
        while (std::getline(ss, part, ',')) {
            if (part.empty()) continue;
            if (part == "left" || part == "right" || part == "up" || part == "down") {
                opts.preview_position = part;
            }
            else if (part == "wrap") {
                opts.preview_wrap = true;
            }
            else if (part == "nowrap") {
                opts.preview_wrap = false;
            }
            else if (part == "hidden") {
                opts.preview_hidden = true;
            }
            else if (part == "follow") {
                opts.preview_follow = true;
            }
            else if (part == "nofollow") {
                opts.preview_follow = false;
            }
            else if (part == "cycle") {
                // No-op: this port has no separate preview-window cycle mode.
            }
            else if (part.compare(0, 7, "border-") == 0) {
                // No-op: preview border style is not independently
                // configurable from the main --border yet.
            }
            else if (part.back() == '%') {
                std::string num_str = part.substr(0, part.size() - 1);
                try {
                    size_t consumed = 0;
                    int v = std::stoi(num_str, &consumed);
                    if (consumed != num_str.size()) throw std::invalid_argument("trailing");
                    opts.preview_size_percent = std::clamp(v, 0, 100);
                    opts.preview_size_is_percent = true;
                } catch (...) {
                    // Unknown/malformed token: skip silently, matching fzf's
                    // tolerance of unrecognized --preview-window components.
                }
            }
            else {
                // Bare N: an absolute line/column count rather than a
                // percentage (fzf: "--preview-window=up,10" == 10 lines).
                try {
                    size_t consumed = 0;
                    int v = std::stoi(part, &consumed);
                    if (consumed != part.size()) throw std::invalid_argument("trailing");
                    opts.preview_size_percent = v;
                    opts.preview_size_is_percent = false;
                } catch (...) {
                    // Unrecognized token (e.g. a future fzf keyword this
                    // port doesn't implement yet): skip silently like fzf.
                }
            }
        }
    }

    // Each --color argument is itself a comma-separated list of "name:value"
    // pairs (e.g. "bg+:6,fg+:3"); split on commas first, then take the
    // FIRST colon per pair (values may legitimately contain further colons
    // in fzf's extended color specs). A bare scheme name with no colon
    // (dark/light/16/16m/bw) is a recognized standalone token — record it
    // under its own key so a future renderer can pick it up, rather than
    // silently dropping it.
    static const std::vector<std::string> color_schemes = {"dark", "light", "16", "16m", "bw"};
    for (const auto& spec : color_specs) {
        std::stringstream ss(spec);
        std::string pair;
        while (std::getline(ss, pair, ',')) {
            if (pair.empty()) continue;
            size_t colon = pair.find(':');
            if (colon == std::string::npos) {
                if (std::find(color_schemes.begin(), color_schemes.end(), pair) !=
                    color_schemes.end()) {
                    opts.colors["scheme"] = pair;
                }
                // Unknown bare token: skip silently, matching fzf's
                // tolerance of unrecognized --color components.
                continue;
            }
            std::string key = pair.substr(0, colon);
            std::string value = pair.substr(colon + 1);
            if (!key.empty()) {
                opts.colors[key] = value;
            }
        }
    }

    return opts;
}

} // namespace fzf
