#include "placeholder.hpp"

#include "tokenizer.hpp"

#include <cstdlib>
#include <cstring>
#include <functional>

#include <unistd.h>

namespace fzf {

// ---------------------------------------------------------------------------
// Executor (fzf: util_unix.go NewExecutor)
// ---------------------------------------------------------------------------

Executor Executor::create(const std::string& with_shell) {
    Executor ex;
    std::vector<std::string> words;
    try {
        words = shell_split_words(with_shell);
    } catch (const OptionError&) {
        words.clear();
    }
    if (!words.empty()) {
        ex.shell = words[0];
        ex.args.assign(words.begin() + 1, words.end());
    } else {
        const char* shell = std::getenv("SHELL");
        ex.shell = (shell && *shell) ? shell : "sh";
        ex.args = {"-c"};
    }
    // fzf: exec.LookPath -- a bare name is resolved through $PATH once, so
    // the fork/exec paths can use execve with an explicit environment.
    if (ex.shell.find('/') == std::string::npos) {
        const char* path = std::getenv("PATH");
        std::string dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
        size_t start = 0;
        while (start <= dirs.size()) {
            size_t end = dirs.find(':', start);
            if (end == std::string::npos) end = dirs.size();
            std::string dir = dirs.substr(start, end - start);
            if (dir.empty()) dir = ".";
            std::string candidate = dir + "/" + ex.shell;
            if (access(candidate.c_str(), X_OK) == 0) {
                ex.shell = candidate;
                break;
            }
            start = end + 1;
        }
    }
    size_t slash = ex.shell.rfind('/');
    std::string base = slash == std::string::npos ? ex.shell : ex.shell.substr(slash + 1);
    ex.fish = (base == "fish");
    return ex;
}

// ---------------------------------------------------------------------------
// Scanner for the placeholder regular expression
// ---------------------------------------------------------------------------

namespace {

bool is_flag_char(char c) { return c == '+' || c == '*' || c == 's' || c == 'f' || c == 'r'; }
bool is_range_char(char c) { return (c >= '0' && c <= '9') || c == ',' || c == '-' || c == '.'; }

// Tries the four alternatives at tmpl[b] == '{'. Returns the end offset
// (exclusive) of the match or 0 when none matches. The alternatives are
// tried in the regex's order, which matters for "{n}" (alt 4 only) versus
// "{}" / "{+f1}" (alt 1).
size_t match_body(std::string_view t, size_t b) {
    const size_t n = t.size();
    // {[+*sfr]*[0-9,-.]*}
    {
        size_t k = b + 1;
        while (k < n && is_flag_char(t[k])) ++k;
        while (k < n && is_range_char(t[k])) ++k;
        if (k < n && t[k] == '}') return k + 1;
    }
    // {q(?::s?[0-9,-.]+)?}
    {
        size_t k = b + 1;
        if (k < n && t[k] == 'q') {
            ++k;
            if (k < n && t[k] == '}') return k + 1;
            if (k < n && t[k] == ':') {
                size_t m = k + 1;
                if (m < n && t[m] == 's') ++m;
                size_t digits = m;
                while (m < n && is_range_char(t[m])) ++m;
                if (m > digits && m < n && t[m] == '}') return m + 1;
            }
        }
    }
    // {fzf:(?:query|action|prompt)}
    {
        static const char* const kFzf[] = {"{fzf:query}", "{fzf:action}", "{fzf:prompt}"};
        for (const char* lit : kFzf) {
            size_t len = std::strlen(lit);
            if (t.substr(b, len) == lit) return b + len;
        }
    }
    // {[+*]?f?nf?}
    {
        size_t k = b + 1;
        if (k < n && (t[k] == '+' || t[k] == '*')) ++k;
        if (k < n && t[k] == 'f') ++k;
        if (k < n && t[k] == 'n') {
            ++k;
            if (k < n && t[k] == 'f') ++k;
            if (k < n && t[k] == '}') return k + 1;
        }
    }
    return 0;
}

bool is_space_byte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// Decodes the codepoint starting at s[i]; returns its byte length (1 for
// invalid input, treated as a non-space byte).
size_t decode_at(std::string_view s, size_t i, char32_t& cp) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { cp = c; return 1; }
    size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0 || i + len > s.size()) { cp = 0xFFFD; return 1; }
    cp = c & (0x7F >> len);
    for (size_t k = 1; k < len; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; return 1; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    return len;
}

} // namespace

std::string trim_space(std::string_view s) {
    size_t begin = 0;
    while (begin < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[begin]);
        if (c < 0x80) {
            if (!is_space_byte(c)) break;
            ++begin;
            continue;
        }
        char32_t cp;
        size_t len = decode_at(s, begin, cp);
        if (!is_unicode_space(cp)) break;
        begin += len;
    }
    size_t end = s.size();
    while (end > begin) {
        unsigned char c = static_cast<unsigned char>(s[end - 1]);
        if (c < 0x80) {
            if (!is_space_byte(c)) break;
            --end;
            continue;
        }
        // Walk back to the lead byte of the last codepoint.
        size_t start = end - 1;
        while (start > begin && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) --start;
        char32_t cp;
        decode_at(s, start, cp);
        if (!is_unicode_space(cp)) break;
        end = start;
    }
    return std::string(s.substr(begin, end - begin));
}

bool find_placeholder(std::string_view tmpl, size_t pos, size_t& start, size_t& len) {
    const size_t n = tmpl.size();
    for (size_t i = pos; i < n; ++i) {
        size_t body;
        if (tmpl[i] == '\\' && i + 1 < n && tmpl[i + 1] == '{') {
            body = i + 1;
        } else if (tmpl[i] == '{') {
            body = i;
        } else {
            continue;
        }
        size_t end = match_body(tmpl, body);
        if (end == 0) {
            // A backslash that does not introduce a placeholder is plain
            // text; the '{' after it is retried on the next iteration.
            continue;
        }
        start = i;
        len = end - i;
        return true;
    }
    return false;
}

// fzf: parsePlaceholder
bool parse_placeholder(std::string_view match, std::string& stripped, PlaceholderFlags& flags) {
    flags = PlaceholderFlags{};
    if (!match.empty() && match[0] == '\\') {
        stripped = std::string(match.substr(1));
        return true;
    }
    if (match.rfind("{fzf:", 0) == 0) {
        // {fzf:*} are not determined by the current item
        flags.force_update = true;
        stripped = std::string(match);
        return false;
    }
    std::string trimmed;
    for (size_t i = 1; i < match.size(); ++i) {
        char c = match[i];
        switch (c) {
            case '*': flags.asterisk = true; break;
            case '+': flags.plus = true; break;
            case 's': flags.preserve_space = true; break;
            case 'n': flags.number = true; break;
            case 'f': flags.file = true; break;
            case 'r': flags.raw = true; break;
            case 'q': flags.force_update = true; trimmed += c; break;
            default: trimmed += c; break;
        }
    }
    stripped = "{" + trimmed;
    return false;
}

// fzf: hasPreviewFlags
TemplateFlags has_preview_flags(std::string_view tmpl) {
    TemplateFlags out;
    size_t pos = 0, start, len;
    std::string stripped;
    PlaceholderFlags flags;
    while (find_placeholder(tmpl, pos, start, len)) {
        pos = start + len;
        if (parse_placeholder(tmpl.substr(start, len), stripped, flags)) continue;
        out.slot = true;
        out.plus = out.plus || flags.plus;
        out.asterisk = out.asterisk || flags.asterisk;
        out.force_update = out.force_update || flags.force_update;
    }
    return out;
}

// fzf: WriteTemporaryFile
std::string write_temporary_file(const std::vector<std::string>& data, const std::string& printsep) {
    const char* dir = std::getenv("TMPDIR");
    std::string path = (dir && *dir) ? dir : "/tmp";
    if (path.back() != '/') path += '/';
    path += "fzf-temp-XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0) return std::string();
    std::string content;
    for (size_t i = 0; i < data.size(); ++i) {
        if (i > 0) content += printsep;
        content += data[i];
    }
    content += printsep;
    size_t off = 0;
    while (off < content.size()) {
        ssize_t w = write(fd, content.data() + off, content.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    close(fd);
    return std::string(buf.data());
}

void remove_files(const std::vector<std::string>& files) {
    for (const auto& f : files) {
        if (!f.empty()) unlink(f.c_str());
    }
}

// fzf: replacePlaceholder
Expansion replace_placeholder(const PlaceholderParams& params) {
    Expansion out;
    static const std::vector<PlaceholderItem> kNone;
    const std::vector<PlaceholderItem>& current = params.current ? *params.current : kNone;
    const std::vector<PlaceholderItem>& selected = params.selected ? *params.selected : kNone;
    const std::vector<PlaceholderItem>& matched = params.matched ? *params.matched : kNone;
    Executor plain;
    const Executor& ex = params.executor ? *params.executor : plain;
    Delimiter awk;
    const Delimiter& delim = params.delimiter ? *params.delimiter : awk;
    const Tokenizer tokenizer(delim);

    std::string_view tmpl = params.tmpl;
    size_t pos = 0, start, len;
    std::string match, stripped;
    PlaceholderFlags flags;
    while (find_placeholder(tmpl, pos, start, len)) {
        out.command.append(tmpl.data() + pos, start - pos);
        pos = start + len;
        match.assign(tmpl.data() + start, len);
        bool escaped = parse_placeholder(match, stripped, flags);

        // Per-item replacement, for the item-type and token-type forms.
        std::function<std::string(const PlaceholderItem&)> replace;

        if (escaped) {
            out.command += stripped;
            continue;
        }
        if (stripped == "{q}" || stripped == "{fzf:query}") {
            out.command += ex.quote(params.query);
            continue;
        }
        if (stripped.rfind("{q:", 0) == 0) {
            std::vector<Range> nth;
            bool ok = true;
            try {
                nth = parse_nth(stripped.substr(3, stripped.size() - 4));
            } catch (const OptionError&) {
                ok = false;
            }
            if (!ok) {
                out.command += match;
                continue;
            }
            const Tokenizer awk_tokenizer(awk);
            std::vector<Token> tokens = awk_tokenizer.tokenize(params.query);
            std::string result = transform_join(tokens, nth);
            if (!flags.preserve_space) result = trim_space(result);
            out.command += ex.quote(result);
            continue;
        }
        if (stripped == "{}") {
            replace = [&](const PlaceholderItem& item) -> std::string {
                if (flags.number) {
                    if (item.index == kMinItemIndex) return "''";
                    return std::to_string(item.index);
                }
                if (flags.file || flags.raw) return item.text;
                return ex.quote(item.text);
            };
        } else if (stripped == "{fzf:action}") {
            out.command += params.last_action;
            continue;
        } else if (stripped == "{fzf:prompt}") {
            out.command += ex.quote(params.prompt);
            continue;
        } else {
            // Token type (and the fallback for anything else): a range list.
            std::vector<Range> ranges;
            bool ok = true;
            try {
                ranges = parse_nth(stripped.substr(1, stripped.size() - 2));
            } catch (const OptionError&) {
                ok = false;
            }
            if (!ok) {
                // Invalid expression, just return the original string in the template
                out.command += match;
                continue;
            }
            replace = [&, ranges](const PlaceholderItem& item) -> std::string {
                std::vector<Token> tokens = tokenizer.tokenize(item.text);
                std::string str = transform_join(tokens, ranges);
                // trim the last delimiter (string and regex delimiters only)
                std::string_view last = get_last_delimiter(str, delim);
                if (!last.empty()) str.resize(str.size() - last.size());
                if (!flags.preserve_space) str = trim_space(str);
                if (!flags.file && !flags.raw) str = ex.quote(str);
                return str;
            };
        }

        // apply 'replace' function over proper set of items and return result
        const std::vector<PlaceholderItem>* items = &current;
        if (flags.asterisk) {
            items = &matched;
        } else if (flags.plus || params.force_plus) {
            items = &selected;
        }
        std::vector<std::string> replacements;
        replacements.reserve(items->size());
        for (const auto& item : *items) replacements.push_back(replace(item));

        if (flags.file) {
            std::string file = write_temporary_file(replacements, params.printsep);
            out.temp_files.push_back(file);
            out.command += file;
        } else {
            for (size_t i = 0; i < replacements.size(); ++i) {
                if (i > 0) out.command += ' ';
                out.command += replacements[i];
            }
        }
    }
    out.command.append(tmpl.data() + pos, tmpl.size() - pos);
    return out;
}

} // namespace fzf
