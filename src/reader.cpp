#include "reader.hpp"

#include "ansi.hpp"
#include "placeholder.hpp"
#include "shellcmd.hpp"
#include "tty.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fzf {

namespace {

constexpr size_t kReadBufferSize = 64 * 1024;   // fzf: readerBufferSize

// Codepoint count and ASCII-ness of a UTF-8 string in one pass.
uint32_t count_runes(std::string_view s, bool& ascii) {
    uint32_t n = 0;
    ascii = true;
    for (unsigned char c : s) {
        if (c >= 0x80) {
            ascii = false;
            if ((c & 0xC0) == 0x80) continue;
        }
        ++n;
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// ItemBuilder
// ---------------------------------------------------------------------------

ItemBuilder::ItemBuilder(const Options& opts)
    : ansi_(opts.ansi), delimiter_(opts.delimiter), tokenizer_(opts.delimiter), nth_(opts.nth) {
    if (!opts.with_nth_expr.empty()) with_nth_ = std::make_unique<NthTransformer>(opts.with_nth_expr);
    if (!opts.accept_nth_expr.empty()) accept_nth_ = std::make_unique<NthTransformer>(opts.accept_nth_expr);
    with_aux_ = ansi_ || !nth_.empty() || with_nth_ != nullptr;
}

// fzf: core.go -- the ChunkList's ItemBuilder closure. With --ansi the
// escapes are stripped and the SGR runs recorded; with --with-nth the
// display text is the transformed line (fzf: transformItem, then
// TrimTrailingWhitespaces) and the original line is kept for output; with
// --nth the codepoint spans of the selected fields are recorded so the
// matcher only looks there (fzf: Pattern.transformInput, done lazily
// there, eagerly here).
const ItemSpec& ItemBuilder::build(std::string_view raw, uint32_t index) {
    spec_ = ItemSpec{};
    std::string_view text = raw;
    bool ascii = true;
    uint32_t runes = 0;

    if (ansi_ && has_escape(raw)) {
        runes = extract_color(raw, stripped_, colors_, ascii);
        text = stripped_;
        if (!colors_.empty()) {
            spec_.colors = colors_.data();
            spec_.color_count = static_cast<uint32_t>(colors_.size());
        }
    } else {
        runes = count_runes(text, ascii);
    }

    if (with_nth_) {
        tokenizer_.tokenize(text, tokens_);
        transformed_ = with_nth_->apply_raw(tokens_, static_cast<int32_t>(index), delimiter_);
        transformed_.resize(trim_trailing_whitespace(transformed_));
        spec_.has_orig = true;
        spec_.orig = raw;
        text = transformed_;
        runes = count_runes(text, ascii);
        // Colors were recorded for the untransformed text; they do not map
        // onto the transformed one (fzf re-runs its ANSI processor on the
        // transformed line; Tier 2 can do the same when --ansi rendering
        // lands). Drop them.
        spec_.colors = nullptr;
        spec_.color_count = 0;
    }

    if (!nth_.empty()) {
        tokenizer_.tokenize(text, tokens_);
        transform_spans(tokens_, nth_, nth_ranges_);
        spec_.nth = nth_ranges_.data();
        spec_.nth_count = static_cast<uint32_t>(nth_ranges_.size());
    }

    spec_.text = text;
    spec_.rune_len = runes;
    spec_.ascii = ascii;
    return spec_;
}

// fzf: Item.AsString(stripAnsi)
std::string ItemBuilder::original_text(const ItemRef& ref) const {
    std::string_view orig = ref.orig_text();
    if (ansi_ && (ref.item().flags & kItemHasOrig)) {
        return strip_ansi(orig);
    }
    return std::string(orig);
}

std::string ItemBuilder::output_text(const ItemRef& ref) const {
    std::string s = original_text(ref);
    if (!accept_nth_) return s;
    std::vector<Token> tokens = tokenizer_.tokenize(s);
    return accept_nth_->apply(tokens, static_cast<int32_t>(ref.index()), delimiter_);
}

std::string ItemBuilder::field_text(const ItemRef& ref, int field) const {
    if (field < 1) return std::string();
    std::string s = original_text(ref);
    std::vector<Token> tokens = tokenizer_.tokenize(s);
    if (static_cast<size_t>(field) > tokens.size()) return std::string();
    return strip_last_delimiter(std::string(tokens[static_cast<size_t>(field) - 1].text), delimiter_);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

Reader::Reader(const Options& opts, ItemBuilder& builder)
    : opts_(opts), builder_(builder), executor_(Executor::create(opts.with_shell)) {
    if (!make_self_pipe(cancel_r_, cancel_w_)) {
        cancel_r_ = cancel_w_ = -1;
    }
}

Reader::~Reader() {
    cancel();
    if (cancel_r_ >= 0) close(cancel_r_);
    if (cancel_w_ >= 0) close(cancel_w_);
}

void Reader::set_wake_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(wake_mu_);
    wake_cb_ = std::move(cb);
}

void Reader::wake() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(wake_mu_);
        cb = wake_cb_;
    }
    if (!cb) return;
    // One wake per consumer round trip: a fast producer generates a
    // handful of wakes, not one per line (DESIGN section 7).
    if (!wake_pending_.exchange(true, std::memory_order_acq_rel)) {
        cb();
    }
}

std::shared_ptr<ChunkList> Reader::list() const {
    std::lock_guard<std::mutex> lock(mu_);
    return list_;
}

bool Reader::finished() const {
    auto l = list();
    return !l || l->finished();
}

std::vector<std::string> Reader::header_lines() const {
    std::lock_guard<std::mutex> lock(mu_);
    return header_;
}

std::string Reader::failed_command() const {
    std::lock_guard<std::mutex> lock(mu_);
    return failed_ ? command_ : std::string();
}

std::shared_ptr<ChunkList> Reader::start_fd(int fd) {
    return start(fd, -1);
}

std::shared_ptr<ChunkList> Reader::start_command(const std::string& command,
                                                 const std::vector<std::string>& env,
                                                 const std::vector<std::string>& temp_files) {
    cancel();
    SpawnedCommand sp = shell_spawn(executor_, command, env, /*null_stdin=*/true, /*setpgid=*/true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        command_ = command;
        temp_files_ = temp_files;
        failed_ = false;
    }
    if (sp.fd < 0) {
        // Nothing to read: an empty, finished generation.
        auto l = std::make_shared<ChunkList>(builder_.with_aux(),
                                             static_cast<uint32_t>(opts_.header_lines));
        l->finish();
        std::lock_guard<std::mutex> lock(mu_);
        list_ = l;
        header_.clear();
        failed_ = true;
        remove_files(temp_files_);
        temp_files_.clear();
        return l;
    }
    return start(sp.fd, sp.pid);
}

std::shared_ptr<ChunkList> Reader::start(int fd, pid_t pid) {
    cancel();
    auto l = std::make_shared<ChunkList>(builder_.with_aux(),
                                         static_cast<uint32_t>(opts_.header_lines));
    {
        std::lock_guard<std::mutex> lock(mu_);
        list_ = l;
        header_.clear();
        if (pid <= 0) {
            command_.clear();
            failed_ = false;
        }
    }
    child_pid_.store(pid);
    thread_ = std::thread(&Reader::run, this, fd, pid, l);
    return l;
}

void Reader::cancel() {
    if (!thread_.joinable()) return;
    walker_cancel_.store(true, std::memory_order_release);
    if (cancel_w_ >= 0) wake_pipe(cancel_w_);
    pid_t pid = child_pid_.load();
    if (pid > 0) kill(-pid, SIGKILL);
    thread_.join();
    walker_cancel_.store(false, std::memory_order_release);
    if (cancel_r_ >= 0) drain_pipe(cancel_r_);
}

std::shared_ptr<ChunkList> Reader::start_default_source() {
    const char* cmd = std::getenv("FZF_DEFAULT_COMMAND");
    if (cmd && *cmd) return start_command(cmd);
    return start_walker(opts_.walker_root, opts_.walker, opts_.walker_skip);
}

std::shared_ptr<ChunkList> Reader::start_walker(const std::vector<std::string>& roots,
                                                const WalkerOpts& walker,
                                                const std::vector<std::string>& skip) {
    cancel();
    auto l = std::make_shared<ChunkList>(builder_.with_aux(),
                                         static_cast<uint32_t>(opts_.header_lines));
    {
        std::lock_guard<std::mutex> lock(mu_);
        list_ = l;
        header_.clear();
        command_.clear();
        failed_ = false;
    }
    child_pid_.store(-1);
    thread_ = std::thread(&Reader::walk, this, roots, walker, skip, l);
    return l;
}

// fzf: reader.go readFiles (fastwalk with SortFilesFirst, follow, hidden,
// and the ignore rules of --walker-skip). Symlink loops are cut by
// remembering every directory's (device, inode).
void Reader::walk(std::vector<std::string> roots, WalkerOpts walker, std::vector<std::string> skip,
                  std::shared_ptr<ChunkList> list) {
    namespace fs = std::filesystem;
    std::vector<std::string> ignores_base, ignores_full, ignores_suffix;
    for (const auto& ignore : skip) {
        if (ignore.find('/') != std::string::npos) {
            if (!ignore.empty() && ignore[0] == '/') {
                ignores_suffix.push_back(ignore);
            } else {
                // 'foo/bar' should match 'foo/bar' and 'baz/foo/bar' but not 'bazfoo/bar'
                ignores_full.push_back(ignore);
                ignores_suffix.push_back("/" + ignore);
            }
        } else {
            ignores_base.push_back(ignore);
        }
    }
    auto trim_path = [](std::string p) {
        while (p.size() > 1 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
        if (p.empty()) p = ".";
        return p;
    };
    auto has_suffix = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    std::set<std::pair<dev_t, ino_t>> visited;
    uint32_t seen = 0;
    const uint32_t header_lines = static_cast<uint32_t>(opts_.header_lines);
    auto push = [&](const std::string& rec) {
        if (seen < header_lines) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                header_.emplace_back(rec);
            }
            ++seen;
            wake();
            return;
        }
        ++seen;
        const ItemSpec& spec = builder_.build(rec, list->first_index() + list->count());
        list->append(spec);
        wake();
    };
    auto cancelled = [&]() { return walker_cancel_.load(std::memory_order_acquire); };

    struct Entry {
        std::string path;   // trimmed, as printed
        bool is_dir;
    };
    // Iterative DFS in fastwalk's order: files of a directory first, then
    // its subdirectories, each group sorted by name.
    std::function<void(const std::string&)> walk_dir = [&](const std::string& dir) {
        if (cancelled()) return;
        std::error_code ec;
        struct stat st {};
        if (::stat(dir.c_str(), &st) == 0) {
            auto key = std::make_pair(st.st_dev, st.st_ino);
            if (!visited.insert(key).second) return;   // loop or duplicate
        }
        std::vector<Entry> files, dirs;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (cancelled()) return;
            const fs::directory_entry& de = *it;
            std::string path = trim_path(de.path().string());
            std::string base = de.path().filename().string();
            std::error_code sec;
            bool is_symlink = de.is_symlink(sec);
            bool is_dir = false;
            if (is_symlink) {
                if (!walker.follow) {
                    // Symlinks are listed as files, never entered.
                    is_dir = false;
                } else {
                    is_dir = fs::is_directory(de.path(), sec);
                }
            } else {
                is_dir = de.is_directory(sec);
            }
            if (is_dir) {
                if (!walker.hidden && !base.empty() && base[0] == '.' && base != "..") continue;
                if (std::find(ignores_base.begin(), ignores_base.end(), base) != ignores_base.end()) continue;
                if (std::find(ignores_full.begin(), ignores_full.end(), path) != ignores_full.end()) continue;
                bool skipped = false;
                for (const auto& suf : ignores_suffix) {
                    if (has_suffix(path, suf)) { skipped = true; break; }
                }
                if (skipped) continue;
                dirs.push_back(Entry{path, true});
            } else {
                if (!walker.hidden && !base.empty() && base[0] == '.') continue;
                files.push_back(Entry{path, false});
            }
        }
        auto by_name = [](const Entry& a, const Entry& b) { return a.path < b.path; };
        std::sort(files.begin(), files.end(), by_name);
        std::sort(dirs.begin(), dirs.end(), by_name);
        if (walker.file) {
            for (const auto& f : files) {
                if (cancelled()) return;
                push(f.path);
            }
        }
        for (const auto& d : dirs) {
            if (cancelled()) return;
            if (walker.dir) push(d.path + "/");
            walk_dir(d.path);
        }
    };
    for (const auto& root : roots) {
        if (cancelled()) break;
        walk_dir(root);
    }
    list->finish();
    wake();
}

void Reader::wait() {
    if (thread_.joinable()) thread_.join();
}

// fzf: Reader.feed
void Reader::run(int fd, pid_t pid, std::shared_ptr<ChunkList> list) {
    std::vector<char> buf(kReadBufferSize);
    std::string leftover;
    const char delim = opts_.read_zero ? '\0' : '\n';
    const uint32_t header_lines = static_cast<uint32_t>(opts_.header_lines);
    uint32_t seen = 0;
    bool cancelled = false;

    auto push = [&](std::string_view rec, bool delimited) {
        // One trailing \r before the newline is trimmed (DESIGN section 7);
        // --read0 records are kept verbatim.
        if (delimited && delim == '\n' && !rec.empty() && rec.back() == '\r') {
            rec.remove_suffix(1);
        }
        if (seen < header_lines) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                header_.emplace_back(rec);
            }
            ++seen;
            wake();
            return;
        }
        ++seen;
        const ItemSpec& spec = builder_.build(rec, list->first_index() + list->count());
        list->append(spec);
        wake();
    };

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int max_fd = fd;
        if (cancel_r_ >= 0) {
            FD_SET(cancel_r_, &rfds);
            if (cancel_r_ > max_fd) max_fd = cancel_r_;
        }
        int n = select(max_fd + 1, &rfds, nullptr, nullptr, nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (cancel_r_ >= 0 && FD_ISSET(cancel_r_, &rfds)) {
            cancelled = true;
            break;
        }
        if (!FD_ISSET(fd, &rfds)) continue;

        ssize_t r = read(fd, buf.data(), buf.size());
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (r == 0) break;   // EOF

        size_t pos = 0;
        const size_t len = static_cast<size_t>(r);
        while (pos < len) {
            const char* hit = static_cast<const char*>(std::memchr(buf.data() + pos, delim, len - pos));
            if (!hit) {
                leftover.append(buf.data() + pos, len - pos);
                break;
            }
            size_t end = static_cast<size_t>(hit - buf.data());
            if (leftover.empty()) {
                push(std::string_view(buf.data() + pos, end - pos), true);
            } else {
                leftover.append(buf.data() + pos, end - pos);
                push(leftover, true);
                leftover.clear();
            }
            pos = end + 1;
        }
    }

    if (!cancelled && !leftover.empty()) {
        push(leftover, false);
    }

    list->finish();
    close(fd);
    if (pid > 0) {
        if (cancelled) kill(-pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        child_pid_.store(-1);
        std::vector<std::string> temps;
        {
            std::lock_guard<std::mutex> lock(mu_);
            // fzf: reader.fin(success) -- the command name is reported when
            // it failed and was not killed by us.
            failed_ = !cancelled && !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            temps.swap(temp_files_);
        }
        remove_files(temps);
    }
    wake();
}

} // namespace fzf
