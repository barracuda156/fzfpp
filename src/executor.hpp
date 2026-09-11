#pragma once

// How child commands are run and how values are quoted for them
// (fzf: src/util/util_unix.go NewExecutor / ExecCommand / QuoteEntry).
//
// $SHELL -c is the default; --with-shell replaces the whole argv prefix
// ("bash -c", "python -c", ...). Values substituted into a command line are
// single-quoted for POSIX shells, with fish's own escaping when the shell
// is fish.

#include <string>
#include <vector>

namespace fzf {

struct Executor {
    std::string shell = "sh";
    std::vector<std::string> args{"-c"};
    bool fish = false;

    // fzf: util.NewExecutor(withShell)
    static Executor create(const std::string& with_shell);

    // fzf: Executor.QuoteEntry
    std::string quote(const std::string& entry) const {
        std::string out;
        out.reserve(entry.size() + 2);
        out += '\'';
        for (char c : entry) {
            if (fish) {
                // https://fishshell.com/docs/current/language.html#quotes
                if (c == '\\') out += "\\\\";
                else if (c == '\'') out += "\\'";
                else out += c;
            } else if (c == '\'') {
                out += "'\\''";
            } else {
                out += c;
            }
        }
        out += '\'';
        return out;
    }

    // argv for `command`: shell, args..., command.
    std::vector<std::string> argv(const std::string& command) const {
        std::vector<std::string> v;
        v.reserve(args.size() + 2);
        v.push_back(shell);
        for (const auto& a : args) v.push_back(a);
        v.push_back(command);
        return v;
    }
};

} // namespace fzf
