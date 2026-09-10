#include "terminal.hpp"
#include "util.hpp"
#include "tty.hpp"
#include "keyparser.hpp"
#include "render.hpp"
#include "shellcmd.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <array>
#include <chrono>
#include <sys/select.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <utf8.h>

namespace fzf {

namespace {

// SIGTERM/SIGHUP land here: request a clean abort so the run loop restores
// the terminal (alt screen, mouse reporting, cursor) before exiting. Only
// async-signal-safe operations: set a flag, poke the self-pipe.
volatile sig_atomic_t g_termination_requested = 0;
int g_termination_wake_fd = -1;

void termination_handler(int) {
    g_termination_requested = 1;
    if (g_termination_wake_fd >= 0) {
        int saved_errno = errno;
        char byte = 1;
        ssize_t w = write(g_termination_wake_fd, &byte, 1);
        (void)w;
        errno = saved_errno;
    }
}

// Restores terminal modes even when the loop exits by exception -- an
// uncaught throw used to strand the user's shell inside the alt screen
// with the cursor hidden and mouse reporting on.
struct ScreenGuard {
    bool mouse;
    explicit ScreenGuard(bool with_mouse) : mouse(with_mouse) {
        enter_alt_screen(STDOUT_FILENO);
        hide_cursor(STDOUT_FILENO);
        if (mouse) {
            enable_mouse(STDOUT_FILENO);
        }
    }
    ~ScreenGuard() {
        if (mouse) {
            disable_mouse(STDOUT_FILENO);
        }
        show_cursor(STDOUT_FILENO);
        leave_alt_screen(STDOUT_FILENO);
    }
};

}  // namespace

Terminal::Terminal(const Options& opts, Reader& reader)
    : opts_(opts),
      reader_(reader),
      matcher_(opts.case_mode, opts.algo, !opts.fuzzy),
      query_cursor_(0),
      cursor_pos_(0),
      scroll_offset_(0),
      running_(false),
      accepted_(false),
      matched_expect_key_(""),
      visible_lines_(10),  // Default, will be updated
      wrap_lines_(opts.wrap),  // Initialize from options
      preview_visible_(!opts.preview_hidden),  // --preview-window=hidden starts it off
      last_click_time_(std::chrono::steady_clock::now()),
      last_click_x_(-1),
      last_click_y_(-1),
      last_preview_cursor_(SIZE_MAX),  // Force initial preview update
      preview_scroll_offset_(0),  // Start at top of preview
      preview_total_lines_(0),    // No preview lines initially
      preview_pending_(false),
      preview_shutdown_(false),
      preview_generation_(0),
      preview_child_pid_(-1),
      preview_target_cursor_(SIZE_MAX),
      wake_read_fd_(-1),
      wake_write_fd_(-1),
      winch_read_fd_(-1),
      winch_write_fd_(-1)
{
    current_prompt_ = opts_.prompt;
    current_header_ = opts_.legacy_header;
}

Terminal::~Terminal() {
    // Signal preview thread to stop, and kill any streaming preview command
    // outright -- the worker may be blocked in fread() on a child that never
    // exits (tail -f style previews), where no flag check can reach it.
    preview_shutdown_.store(true);
    preview_generation_.fetch_add(1);
    preview_pending_.store(false);
    shell_kill(preview_child_pid_.load(), SIGKILL);

    if (preview_thread_.joinable()) {
        preview_thread_.join();
    }
}

void Terminal::supersede_preview() {
    preview_generation_.fetch_add(1);
    // A streaming render for the old target may sit in fread() indefinitely;
    // terminate its process group so the worker gets EOF and moves on. The
    // worker re-checks its generation before publishing, so even output that
    // raced in before the signal can't be shown or cached.
    shell_kill(preview_child_pid_.load(), SIGTERM);
}

void Terminal::sync_query_from_codepoints() {
    std::string utf8_text;
    utf8::utf32to8(query_codepoints_.begin(), query_codepoints_.end(),
                   std::back_inserter(utf8_text));
    std::lock_guard<std::mutex> lock(preview_mutex_);
    current_query_ = std::move(utf8_text);
}

void Terminal::set_current_header(const std::string& header) {
    std::lock_guard<std::mutex> lock(preview_mutex_);
    current_header_ = header;
}

void Terminal::update_results(const std::string& query) {
    // Get current items
    auto items = reader_.get_items();

    std::vector<MatchResult> new_results;

    if (query.empty() || opts_.disabled) {
        // No query (or --disabled: the query never filters, an external
        // reload command does): show all items in their current order.
        for (const auto& item : items) {
            new_results.emplace_back(item, 0);
        }
    } else {
        // Perform matching
        new_results = matcher_.match_items(items, query);
    }

    // Update results (thread-safe)
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        current_results_ = std::move(new_results);

        // Reset cursor if out of bounds
        if (cursor_pos_ >= current_results_.size()) {
            cursor_pos_ = current_results_.empty() ? 0 : current_results_.size() - 1;
        }

        // Adjust scroll offset
        if (cursor_pos_ < scroll_offset_) {
            scroll_offset_ = cursor_pos_;
        } else if (cursor_pos_ >= scroll_offset_ + visible_lines_) {
            scroll_offset_ = cursor_pos_ - visible_lines_ + 1;
        }
    }

    // Populate prefetch queue with new results (for background preview caching)
    if (!opts_.preview_command.empty()) {
        populate_prefetch_queue();
    }
}

std::vector<MatchResult> Terminal::get_visible_results() const {
    std::lock_guard<std::mutex> lock(results_mutex_);

    if (current_results_.empty()) {
        return {};
    }

    size_t start = scroll_offset_;
    size_t end = std::min(start + visible_lines_, current_results_.size());

    return std::vector<MatchResult>(
        current_results_.begin() + start,
        current_results_.begin() + end
    );
}

void Terminal::move_cursor_up() {
    if (cursor_pos_ > 0) {
        cursor_pos_--;
        if (cursor_pos_ < scroll_offset_) {
            scroll_offset_ = cursor_pos_;
        }
    } else if (opts_.cycle && !current_results_.empty()) {
        cursor_pos_ = current_results_.size() - 1;
        scroll_offset_ = cursor_pos_ >= visible_lines_
                        ? cursor_pos_ - visible_lines_ + 1
                        : 0;
    }
}

void Terminal::move_cursor_down() {
    if (cursor_pos_ + 1 < current_results_.size()) {
        cursor_pos_++;
        if (cursor_pos_ >= scroll_offset_ + visible_lines_) {
            scroll_offset_ = cursor_pos_ - visible_lines_ + 1;
        }
    } else if (opts_.cycle && !current_results_.empty()) {
        cursor_pos_ = 0;
        scroll_offset_ = 0;
    }
}

void Terminal::move_cursor_page_up() {
    if (cursor_pos_ >= visible_lines_) {
        cursor_pos_ -= visible_lines_;
    } else {
        cursor_pos_ = 0;
    }
    scroll_offset_ = cursor_pos_;
}

void Terminal::move_cursor_page_down() {
    if (current_results_.empty()) {
        return;
    }
    cursor_pos_ = std::min(cursor_pos_ + visible_lines_,
                          current_results_.size() - 1);
    if (cursor_pos_ >= scroll_offset_ + visible_lines_) {
        scroll_offset_ = cursor_pos_ - visible_lines_ + 1;
    }
}

void Terminal::toggle_selection() {
    if (!opts_.multi || current_results_.empty()) {
        return;
    }

    size_t item_idx = current_results_[cursor_pos_].item->index();

    if (selected_.count(item_idx)) {
        selected_.erase(item_idx);
    } else {
        selected_.insert(item_idx);
    }

    // Move cursor down after toggle
    move_cursor_down();
}

void Terminal::select_all() {
    if (!opts_.multi) {
        return;
    }

    std::lock_guard<std::mutex> lock(results_mutex_);
    for (const auto& result : current_results_) {
        selected_.insert(result.item->index());
    }
}

void Terminal::deselect_all() {
    selected_.clear();
}

void Terminal::toggle_all() {
    if (!opts_.multi) {
        return;
    }

    std::lock_guard<std::mutex> lock(results_mutex_);
    for (const auto& result : current_results_) {
        size_t item_idx = result.item->index();
        if (selected_.count(item_idx)) {
            selected_.erase(item_idx);
        } else {
            selected_.insert(item_idx);
        }
    }
}

bool Terminal::is_selected(size_t item_index) const {
    return selected_.count(item_index) > 0;
}

void Terminal::accept_selection() {
    accepted_ = true;
    running_ = false;
}

bool Terminal::check_expect_key(const KeyEvent& event, std::string& matched_key) {
    if (opts_.expect_keys.empty()) {
        return false;
    }

    const std::string& input = event.input;

    // Check for special named keys first
    for (const auto& expect : opts_.expect_keys) {
        if (expect == "enter" && event.type == KeyType::Special && event.special == SpecialKey::Return) {
            matched_key = "enter";
            return true;
        }
        if (expect == "esc" && event.type == KeyType::Special && event.special == SpecialKey::Escape) {
            matched_key = "esc";
            return true;
        }
        if (expect == "tab" && event.type == KeyType::Special && event.special == SpecialKey::Tab) {
            matched_key = "tab";
            return true;
        }
        if (expect == "up" && event.type == KeyType::Special && event.special == SpecialKey::ArrowUp) {
            matched_key = "up";
            return true;
        }
        if (expect == "down" && event.type == KeyType::Special && event.special == SpecialKey::ArrowDown) {
            matched_key = "down";
            return true;
        }
        if (expect == "left" && event.type == KeyType::Special && event.special == SpecialKey::ArrowLeft) {
            matched_key = "left";
            return true;
        }
        if (expect == "right" && event.type == KeyType::Special && event.special == SpecialKey::ArrowRight) {
            matched_key = "right";
            return true;
        }
        if (expect == "page-up" && event.type == KeyType::Special && event.special == SpecialKey::PageUp) {
            matched_key = "page-up";
            return true;
        }
        if (expect == "page-down" && event.type == KeyType::Special && event.special == SpecialKey::PageDown) {
            matched_key = "page-down";
            return true;
        }
        if (expect == "home" && event.type == KeyType::Special && event.special == SpecialKey::Home) {
            matched_key = "home";
            return true;
        }
        if (expect == "end" && event.type == KeyType::Special && event.special == SpecialKey::End) {
            matched_key = "end";
            return true;
        }
    }

    // Map of ANSI sequences to key names for shift+arrow and other special keys
    // Try multiple variants as different terminals may use different sequences
    static const std::pair<const char*, const char*> key_map[] = {
        // Standard xterm sequences for shift+arrows
        {"\x1b[1;2D", "shift-left"},
        {"\x1b[1;2C", "shift-right"},
        {"\x1b[1;2A", "shift-up"},
        {"\x1b[1;2B", "shift-down"},
        // Alternative sequences (some terminals)
        {"\x1b[2D", "shift-left"},
        {"\x1b[2C", "shift-right"},
        {"\x1b[2A", "shift-up"},
        {"\x1b[2B", "shift-down"},
        // Ctrl+arrows
        {"\x1b[1;5D", "ctrl-left"},
        {"\x1b[1;5C", "ctrl-right"},
        {"\x1b[1;5A", "ctrl-up"},
        {"\x1b[1;5B", "ctrl-down"},
        // Alt+arrows
        {"\x1b[1;3D", "alt-left"},
        {"\x1b[1;3C", "alt-right"},
        {"\x1b[1;3A", "alt-up"},
        {"\x1b[1;3B", "alt-down"},
    };

    for (const auto& expect : opts_.expect_keys) {
        // Check against ANSI sequences
        for (const auto& [seq, name] : key_map) {
            if (expect == name && input == seq) {
                matched_key = name;
                return true;
            }
        }

        // Check for single character keys (like 'a', 'b', etc.)
        if (expect.length() == 1 && input == expect) {
            matched_key = expect;
            return true;
        }

        // Check for ctrl+letter (ctrl-a, ctrl-b, etc.)
        if (expect.find("ctrl-") == 0 && expect.length() == 6) {
            char letter = expect[5];
            if (letter >= 'a' && letter <= 'z') {
                char ctrl_char = letter - 'a' + 1;  // ctrl-a = 0x01, ctrl-b = 0x02, etc.
                if (input.length() == 1 && input[0] == ctrl_char) {
                    matched_key = expect;
                    return true;
                }
            }
        }

        // Check for alt+letter
        if (expect.find("alt-") == 0 && expect.length() == 5) {
            char letter = expect[4];
            std::string alt_seq = "\x1b";
            alt_seq += letter;
            if (input == alt_seq) {
                matched_key = expect;
                return true;
                }
        }
    }

    return false;
}

std::string Terminal::event_to_bind_key(const KeyEvent& event) {
    // Named special keys that only reach binds through this lookup (the
    // navigation keys with default behaviors are dispatched before this).
    if (event.type == KeyType::Special) {
        switch (event.special) {
            case SpecialKey::Insert: return "insert";
            case SpecialKey::F1:  return "f1";
            case SpecialKey::F2:  return "f2";
            case SpecialKey::F3:  return "f3";
            case SpecialKey::F4:  return "f4";
            case SpecialKey::F5:  return "f5";
            case SpecialKey::F6:  return "f6";
            case SpecialKey::F7:  return "f7";
            case SpecialKey::F8:  return "f8";
            case SpecialKey::F9:  return "f9";
            case SpecialKey::F10: return "f10";
            case SpecialKey::F11: return "f11";
            case SpecialKey::F12: return "f12";
            default: break;
        }
    }

    const std::string& input = event.input;

    // Alt-modified keys arrive as "ESC <char>" (and alt-backspace as
    // "ESC 0x7f"). fzf names them alt-<char> / alt-bs.
    if (input.length() == 2 && input[0] == '\x1b') {
        unsigned char c1 = static_cast<unsigned char>(input[1]);
        if (c1 == 0x7f || c1 == 0x08) {
            return "alt-bs";
        }
        if (c1 >= 0x20 && c1 < 0x7f) {
            return std::string("alt-") + static_cast<char>(c1);
        }
        return "";
    }

    // Control-character keys (ctrl-a..z, ctrl-space, ctrl-/, etc.) arrive as
    // Character events carrying a single control byte (see keyparser.cpp) —
    // check the raw input byte directly rather than any printability check.
    if (input.length() != 1) {
        return "";
    }
    unsigned char c = static_cast<unsigned char>(input[0]);

    // Ctrl+space and ctrl-` both send NUL on most terminals; fzf calls this ctrl-space.
    if (c == 0x00) {
        return "ctrl-space";
    }
    // Ctrl-a .. ctrl-z, skipping the ones the parser turns into named Special
    // events before this would even be reached (ctrl-i=Tab, ctrl-m/j=Return,
    // ctrl-h=Backspace).
    if (c >= 0x01 && c <= 0x1a) {
        char letter = static_cast<char>('a' + (c - 0x01));
        return std::string("ctrl-") + letter;
    }
    // Ctrl-/ (and ctrl-_) conventionally send 0x1f.
    if (c == 0x1f) {
        return "ctrl-/";
    }
    // Ctrl-\  sends 0x1c.
    if (c == 0x1c) {
        return "ctrl-\\";
    }
    // Ctrl-] sends 0x1d.
    if (c == 0x1d) {
        return "ctrl-]";
    }
    return "";
}

bool Terminal::extract_paren_arg(const std::string& action, size_t open_paren_pos,
                                 std::string& out_arg, size_t& out_end) {
    if (open_paren_pos >= action.length() || action[open_paren_pos] != '(') {
        return false;
    }
    int depth = 0;
    for (size_t i = open_paren_pos; i < action.length(); ++i) {
        if (action[i] == '(') {
            depth++;
        } else if (action[i] == ')') {
            if (--depth == 0) {
                out_arg = action.substr(open_paren_pos + 1, i - open_paren_pos - 1);
                out_end = i;
                return true;
            }
        }
    }
    return false;
}

bool Terminal::execute_bind_action(const std::string& action) {
    // Wrap a fully-substituted command for --with-shell. The substitution
    // injects single-quoted values, so the command itself must be re-escaped
    // before being wrapped in another layer of single quotes -- a naive
    // "'" + cmd + "'" terminated at the first inner quote and word-split the
    // rest ({} containing "it's" broke every --with-shell invocation).
    auto wrap_with_shell = [&](std::string cmd) -> std::string {
        if (opts_.with_shell.empty()) {
            return cmd;
        }
        std::string escaped;
        escaped.reserve(cmd.size() + 2);
        for (char c : cmd) {
            if (c == '\'') {
                escaped += "'\\''";
            } else {
                escaped += c;
            }
        }
        return opts_.with_shell + " '" + escaped + "'";
    };

    auto current_cursor = [&]() -> size_t {
        std::lock_guard<std::mutex> lock(results_mutex_);
        return cursor_pos_;
    };

    auto run_transform_header = [&](const std::string& cmd_tpl) {
        std::string final_cmd = wrap_with_shell(
            substitute_placeholders(cmd_tpl, current_cursor()));
        set_current_header(run_command_capture_output(final_cmd));
    };

    auto run_reload = [&](const std::string& cmd_tpl) {
        std::string final_cmd = wrap_with_shell(
            substitute_placeholders(cmd_tpl, current_cursor()));
        reader_.load_from_command(final_cmd);

        // The new list has fresh zero-based indices; cursor, scroll and any
        // Tab-selections referring to the old set are all meaningless now
        // (stale selected_ entries used to map onto arbitrary rows of the
        // reloaded list and get emitted on accept).
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            cursor_pos_ = 0;
            scroll_offset_ = 0;
        }
        selected_.clear();
        update_results(current_query_);
    };

    // fzf's trailing-colon syntax: "action-name:ARG" takes ARG verbatim to
    // the END of the bind string -- commas, '+', parens are all literal.
    // These must be recognized before the composite '+' split below.
    {
        struct ColonAction { const char* prefix; int kind; };
        static const ColonAction kColonActions[] = {
            {"transform-header:", 0},
            {"reload:", 1},
            {"reload-sync:", 1},
            {"change-prompt:", 2},
        };
        for (const auto& ca : kColonActions) {
            if (action.rfind(ca.prefix, 0) == 0) {
                std::string arg = action.substr(std::strlen(ca.prefix));
                switch (ca.kind) {
                    case 0: run_transform_header(arg); break;
                    case 1: run_reload(arg); break;
                    case 2: current_prompt_ = arg; break;
                }
                return true;
            }
        }
    }

    if (action.find("execute(") == 0) {
        // Find matching closing parenthesis by counting nesting level
        size_t start = 7;  // Position after "execute"
        int depth = 0;
        size_t end = std::string::npos;

        for (size_t i = start; i < action.length(); ++i) {
            if (action[i] == '(') {
                depth++;
            } else if (action[i] == ')') {
                depth--;
                if (depth == 0) {
                    // Found the matching closing paren
                    end = i;
                    break;
                }
            }
        }

        if (end == std::string::npos) {
            return false;
        }

        // Extract command from execute(command)
        size_t cmd_start = 8;  // Length of "execute("
        std::string cmd = action.substr(cmd_start, end - cmd_start);

        // Get cursor position safely with brief mutex lock
        size_t cursor_idx;
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            cursor_idx = cursor_pos_;
        }

        // Substitute placeholders (this function handles its own mutex locking)
        std::string shell_cmd = wrap_with_shell(substitute_placeholders(cmd, cursor_idx));

        // Background the command so it doesn't block the UI loop.
        // The command will run asynchronously and write to /dev/tty
        std::string bg_cmd = "(" + shell_cmd + ") &";

        int result = system(bg_cmd.c_str());
        (void)result;  // Ignore return value

        // Give the background process a moment to start
        usleep(100000);  // 100ms

        // Check if there's a composite action after execute()
        if (end + 1 < action.length() && action[end + 1] == '+') {
            std::string rest = action.substr(end + 2);  // Skip ')+'
            execute_bind_action(rest);
        }

        return true;
    }

    if (action.find("change-prompt(") == 0) {
        // Match the closing paren by nesting depth so a prompt string may itself
        // contain parentheses; parsed here (before the naive '+' split below) so
        // a '+' inside the new prompt is not mistaken for a composite separator.
        size_t start = action.find('(');
        int depth = 0;
        size_t end = std::string::npos;
        for (size_t i = start; i < action.length(); ++i) {
            if (action[i] == '(') {
                depth++;
            } else if (action[i] == ')') {
                if (--depth == 0) { end = i; break; }
            }
        }
        if (end == std::string::npos) {
            return false;
        }

        current_prompt_ = action.substr(start + 1, end - start - 1);

        // Honor a composite action chained after change-prompt(...)+...
        if (end + 1 < action.length() && action[end + 1] == '+') {
            execute_bind_action(action.substr(end + 2));
        }
        return true;
    }

    // Composite actions: split on '+' at paren depth 0 only, so an argument
    // like reload(date +%s) or execute(echo a+b) stays intact and falls
    // through to its own handler below.
    {
        int depth = 0;
        size_t plus_pos = std::string::npos;
        for (size_t i = 0; i < action.size(); ++i) {
            char c = action[i];
            if (c == '(') {
                depth++;
            } else if (c == ')') {
                if (depth > 0) depth--;
            } else if (c == '+' && depth == 0) {
                plus_pos = i;
                break;
            }
        }
        if (plus_pos != std::string::npos) {
            execute_bind_action(action.substr(0, plus_pos));
            execute_bind_action(action.substr(plus_pos + 1));
            return true;
        }
    }

    if (action == "select-all") {
        select_all();
        return true;
    }

    if (action == "deselect-all") {
        deselect_all();
        return true;
    }

    if (action == "toggle-all") {
        toggle_all();
        return true;
    }

    if (action == "toggle") {
        toggle_selection();
        return true;
    }

    if (action == "down") {
        move_cursor_down();
        return true;
    }

    if (action == "up") {
        move_cursor_up();
        return true;
    }

    if (action == "page-down") {
        move_cursor_page_down();
        return true;
    }

    if (action == "page-up") {
        move_cursor_page_up();
        return true;
    }

    if (action == "top" || action == "first") {
        std::lock_guard<std::mutex> lock(results_mutex_);
        cursor_pos_ = 0;
        scroll_offset_ = 0;
        return true;
    }

    if (action == "bottom" || action == "last") {
        std::lock_guard<std::mutex> lock(results_mutex_);
        if (!current_results_.empty()) {
            cursor_pos_ = current_results_.size() - 1;
            if (cursor_pos_ >= visible_lines_) {
                scroll_offset_ = cursor_pos_ - visible_lines_ + 1;
            }
        }
        return true;
    }

    if (action == "toggle-wrap") {
        wrap_lines_ = !wrap_lines_;
        return true;
    }

    if (action == "toggle-preview") {
        preview_visible_ = !preview_visible_;
        // The pane is cleared/overwritten while hidden, so the cached "already
        // painted this" state is stale on re-show — force a full repaint.
        last_painted_valid_ = false;
        return true;
    }

    if (action == "accept") {
        accept_selection();
        running_ = false;
        return true;
    }

    if (action == "abort") {
        selected_.clear();
        accepted_ = false;
        running_ = false;
        return true;
    }

    if (action == "preview-up") {
        if (preview_scroll_offset_ > 0) {
            preview_scroll_offset_--;
        }
        return true;
    }

    // preview_total_lines_ reflects the last painted frame; clamping here
    // (not only after the paint) keeps the offset from running past the end,
    // which used to paint an empty pane and then latch it via the
    // painted-state cache (pane stayed blank until the content changed).
    if (action == "preview-down") {
        if (preview_total_lines_ == 0 ||
            preview_scroll_offset_ + 1 < preview_total_lines_) {
            preview_scroll_offset_++;
        }
        return true;
    }

    if (action == "preview-page-up") {
        if (preview_scroll_offset_ >= visible_lines_) {
            preview_scroll_offset_ -= visible_lines_;
        } else {
            preview_scroll_offset_ = 0;
        }
        return true;
    }

    if (action == "preview-page-down") {
        size_t next = preview_scroll_offset_ + visible_lines_;
        if (preview_total_lines_ > 0 && next >= preview_total_lines_) {
            next = preview_total_lines_ - 1;
        }
        preview_scroll_offset_ = next;
        return true;
    }

    if (action == "ignore") {
        return true;
    }

    if (action == "clear-query") {
        query_codepoints_.clear();
        query_cursor_ = 0;
        sync_query_from_codepoints();
        update_results(current_query_);
        return true;
    }

    // --- Query-line editing (fzf's readline-style default keymap) ---

    if (action == "beginning-of-line") {
        query_cursor_ = 0;
        return true;
    }

    if (action == "end-of-line") {
        query_cursor_ = query_codepoints_.size();
        return true;
    }

    if (action == "backward-char") {
        query_move_left();
        return true;
    }

    if (action == "forward-char") {
        query_move_right();
        return true;
    }

    if (action == "delete-char") {
        query_delete();
        return true;
    }

    if (action == "backward-delete-char") {
        query_backspace();
        return true;
    }

    if (action == "delete-char/eof") {
        if (query_codepoints_.empty()) {
            selected_.clear();
            accepted_ = false;
            running_ = false;
            return false;
        }
        query_delete();
        return true;
    }

    if (action == "unix-line-discard") {
        if (query_cursor_ > 0) {
            query_codepoints_.erase(0, query_cursor_);
            query_cursor_ = 0;
            sync_query_from_codepoints();
        }
        return true;
    }

    if (action == "kill-line") {
        if (query_cursor_ < query_codepoints_.size()) {
            query_codepoints_.erase(query_cursor_);
            sync_query_from_codepoints();
        }
        return true;
    }

    if (action == "unix-word-rubout" || action == "backward-kill-word") {
        // Delete back over trailing blanks, then over the word before the
        // cursor. (fzf distinguishes the two by word charset; blank-delimited
        // covers the default keymap uses.)
        size_t end = query_cursor_;
        size_t pos = end;
        while (pos > 0 && query_codepoints_[pos - 1] == U' ') pos--;
        while (pos > 0 && query_codepoints_[pos - 1] != U' ') pos--;
        if (pos < end) {
            query_codepoints_.erase(pos, end - pos);
            query_cursor_ = pos;
            sync_query_from_codepoints();
        }
        return true;
    }

    if (action == "kill-word") {
        size_t start = query_cursor_;
        size_t pos = start;
        size_t n = query_codepoints_.size();
        while (pos < n && query_codepoints_[pos] == U' ') pos++;
        while (pos < n && query_codepoints_[pos] != U' ') pos++;
        if (pos > start) {
            query_codepoints_.erase(start, pos - start);
            sync_query_from_codepoints();
        }
        return true;
    }

    if (action == "backward-word") {
        while (query_cursor_ > 0 && query_codepoints_[query_cursor_ - 1] == U' ') query_cursor_--;
        while (query_cursor_ > 0 && query_codepoints_[query_cursor_ - 1] != U' ') query_cursor_--;
        return true;
    }

    if (action == "forward-word") {
        size_t n = query_codepoints_.size();
        while (query_cursor_ < n && query_codepoints_[query_cursor_] == U' ') query_cursor_++;
        while (query_cursor_ < n && query_codepoints_[query_cursor_] != U' ') query_cursor_++;
        return true;
    }

    if (action.find("become(") == 0) {
        return true;  // Stub: process replacement not implemented
    }

    if (action.find("reload(") == 0 || action.find("reload-sync(") == 0) {
        // reload(cmd): run cmd and replace the item set with its output.
        // reload-sync is treated identically here (we always reload synchronously).
        size_t open = action.find('(');
        std::string cmd;
        size_t end;
        if (!extract_paren_arg(action, open, cmd, end)) {
            return false;
        }

        run_reload(cmd);

        // Honor a composite action chained after reload(...)+...
        if (end + 1 < action.length() && action[end + 1] == '+') {
            execute_bind_action(action.substr(end + 2));
        }
        return true;
    }

    if (action.find("change-preview(") == 0) {
        return true;  // Stub: change-preview not implemented
    }

    // transform-header(CMD) paren form; the colon form transform-header:CMD
    // (fzf's "extends to end of bind string" syntax, needed when CMD has
    // unbalanced parens, e.g. a shell `case`) is handled at the top of this
    // function, before the composite '+' split.
    if (action.find("transform-header(") == 0) {
        size_t open = action.find('(');
        std::string cmd;
        size_t end;
        if (!extract_paren_arg(action, open, cmd, end)) {
            return false;
        }

        run_transform_header(cmd);

        if (end + 1 < action.length() && action[end + 1] == '+') {
            execute_bind_action(action.substr(end + 2));
        }
        return true;
    }

    return false;
}

void Terminal::get_terminal_size(int& rows, int& cols) const {
    if (!fzf::get_terminal_size(STDOUT_FILENO, rows, cols)) {
        // Fallback to reasonable defaults
        rows = 24;
        cols = 80;
    }
}

void Terminal::calculate_column_layout(int content_cols, int& results_width,
                                       int& preview_cols, int& sep_col) const {
    int preview_width = opts_.preview_size_is_percent
        ? (content_cols * opts_.preview_size_percent) / 100
        : opts_.preview_size_percent;  // absolute column count
    if (preview_width > content_cols - 2) {
        preview_width = content_cols - 2;  // always leave room for results
    }
    if (preview_width < 1) {
        preview_width = 1;
    }
    preview_cols = preview_width;
    results_width = content_cols - preview_width - 1; // -1 for separator
    if (results_width < 1) results_width = 1;

    sep_col = (opts_.preview_position == "left")
                  ? preview_cols
                  : results_width;
}

void Terminal::calculate_preview_position(int& top, int& left, int& lines, int& cols) const {
    int term_rows, term_cols;
    get_terminal_size(term_rows, term_cols);

    // Calculate layout based on whether preview is enabled
    if (opts_.preview_command.empty()) {
        // No preview, shouldn't be called but provide safe defaults
        top = 0;
        left = 0;
        lines = 0;
        cols = 0;
        return;
    }

    int margin = opts_.border ? 1 : 0;
    int content_cols = term_cols - 2 * margin;
    if (content_cols < 1) content_cols = 1;

    int results_width, preview_cols, sep_col;
    calculate_column_layout(content_cols, results_width, preview_cols, sep_col);

    // Calculate vertical layout:
    // Row 0: Info line (1 row, if not hidden)
    // Row 1: Header (if present, 1 row)
    // Row 2: Separator (1 row)
    // Row 3+: Content area
    // Bottom: Separator (1 row) + Input (1 row)

    // Called from the preview worker too (via preview_env_vars):
    // current_header_ and visible_lines_ are main-thread state, so snapshot
    // them under the shared lock.
    bool header_present;
    int band_lines;
    {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        header_present = !current_header_.empty();
        band_lines = static_cast<int>(visible_lines_);
    }

    int info_rows = opts_.info_hidden ? 0 : 1;
    int header_rows = header_present ? 1 : 0;
    int top_ui_rows = info_rows + header_rows + 1; // info + header + separator
    if (opts_.border) {
        top_ui_rows += 1;
    }

    // Preview starts after top UI elements and spans exactly the results
    // band repaint() draws (visible_lines_). Deriving the height from
    // term_rows here while repaint honored --height meant the pane could
    // overshoot the bottom separator and prompt whenever they differed.
    top = top_ui_rows;
    if (opts_.preview_position == "left") {
        left = margin; // Preview starts at the content area's left edge
    } else {
        left = margin + sep_col + 1; // After results + separator
    }
    lines = band_lines;
    cols = preview_cols;
}

std::vector<std::string> Terminal::preview_env_vars() const {
    // Passed to the preview child via execve (see shell_popen) rather than
    // setenv: the worker calling setenv while the main thread walks environ
    // (getenv in shell_popen, system() for execute binds) is a glibc UB race.
    int preview_top, preview_left, preview_lines, preview_cols;
    calculate_preview_position(preview_top, preview_left, preview_lines, preview_cols);

    return {
        "FZF_PREVIEW_TOP=" + std::to_string(preview_top),
        "FZF_PREVIEW_LEFT=" + std::to_string(preview_left),
        "FZF_PREVIEW_LINES=" + std::to_string(preview_lines),
        "FZF_PREVIEW_COLUMNS=" + std::to_string(preview_cols),
    };
}

std::string Terminal::substitute_placeholders(const std::string& cmd, size_t index) {
    std::shared_ptr<Item> item;
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        if (index < current_results_.size()) {
            item = current_results_[index].item;
        }
    }
    return substitute_placeholders_for_item(cmd, item);
}

std::string Terminal::substitute_placeholders_for_item(const std::string& cmd,
                                                       const std::shared_ptr<Item>& item) {
    std::string result = cmd;
    std::string line_text = item ? item->text() : "";

    // Replace {n} with the item's zero-based input index (fzf semantics --
    // the ordinal in the original input stream, not the position in the
    // filtered list, which changes with every keystroke).
    size_t n_value = item ? item->index() : 0;
    size_t pos = 0;
    while ((pos = result.find("{n}", pos)) != std::string::npos) {
        result.replace(pos, 3, std::to_string(n_value));
        pos += std::to_string(n_value).length();
    }

    // Replace {q} with current query (copied under the lock the preview
    // worker shares with the main thread's per-keystroke reassignment).
    pos = 0;
    std::string escaped_query;
    {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        escaped_query = current_query_;
    }
    size_t qpos = 0;
    while ((qpos = escaped_query.find("'", qpos)) != std::string::npos) {
        escaped_query.replace(qpos, 1, "'\\''");
        qpos += 4;
    }
    while ((pos = result.find("{q}", pos)) != std::string::npos) {
        result.replace(pos, 3, "'" + escaped_query + "'");
        pos += escaped_query.length() + 2;
    }

    // Replace {1}, {2}, {3}, etc. with specific fields (1-based)
    for (int field_num = 1; field_num <= 9; ++field_num) {
        std::string placeholder = "{" + std::to_string(field_num) + "}";
        pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            std::string field_value;
            if (item && item->has_fields()) {
                field_value = item->get_field(field_num);
            }
            // Escape field value
            std::string escaped_field = field_value;
            size_t fpos = 0;
            while ((fpos = escaped_field.find("'", fpos)) != std::string::npos) {
                escaped_field.replace(fpos, 1, "'\\''");
                fpos += 4;
            }
            result.replace(pos, placeholder.length(), "'" + escaped_field + "'");
            pos += escaped_field.length() + 2;
        }
    }

    // Replace {} with the full line text
    // Need to escape shell special characters
    std::string escaped_line = line_text;
    pos = 0;
    while ((pos = escaped_line.find("'", pos)) != std::string::npos) {
        escaped_line.replace(pos, 1, "'\\''");
        pos += 4;
    }

    pos = 0;
    while ((pos = result.find("{}", pos)) != std::string::npos) {
        result.replace(pos, 2, "'" + escaped_line + "'");
        pos += escaped_line.length() + 2;
    }

    return result;
}

std::string Terminal::run_command_capture_output(const std::string& cmd) {
    ShellPipe pipe = shell_popen(cmd);
    if (!pipe.stream) {
        return "";
    }

    std::string output;
    std::array<char, 4096> buffer;
    size_t bytes_read;
    while ((bytes_read = fread(buffer.data(), 1, buffer.size(), pipe.stream)) > 0) {
        output.append(buffer.data(), bytes_read);
    }
    shell_pclose(pipe);

    if (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }
    return output;
}

std::string Terminal::get_cached_preview(const std::string& item_text) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = preview_cache_.find(item_text);
    if (it == preview_cache_.end()) {
        return "";  // Not in cache
    }

    // Move this item to front of LRU list (mark as most recently used)
    preview_lru_.remove(item_text);
    preview_lru_.push_front(item_text);

    return it->second;
}

void Terminal::cache_preview(const std::string& item_text, const std::string& content) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Check if already in cache
    auto it = preview_cache_.find(item_text);
    if (it != preview_cache_.end()) {
        // Update existing entry
        it->second = content;
        // Move to front of LRU list
        preview_lru_.remove(item_text);
        preview_lru_.push_front(item_text);
        return;
    }

    // Add new entry
    preview_cache_[item_text] = content;
    preview_lru_.push_front(item_text);

    // Evict oldest if cache is too large
    if (preview_cache_.size() > PREVIEW_CACHE_MAX_SIZE) {
        std::string oldest = preview_lru_.back();
        preview_lru_.pop_back();
        preview_cache_.erase(oldest);
    }
}

void Terminal::populate_prefetch_queue() {
    // Prefetch only around the visible window. Queueing EVERY result spawned
    // one $SHELL per item in the list (thousands on a big pipe), rebuilt on
    // every keystroke, and with a 50-entry cache most of that work was
    // evicted before it could ever be served. fzf itself renders only the
    // focused item; a window of the on-screen items plus one page below is
    // already more speculative than that.
    std::lock_guard<std::mutex> prefetch_lock(prefetch_mutex_);
    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
    std::lock_guard<std::mutex> results_lock(results_mutex_);

    prefetch_queue_.clear();

    size_t start = scroll_offset_;
    size_t end = std::min(current_results_.size(), start + 2 * visible_lines_);
    for (size_t i = start; i < end; ++i) {
        std::string item_text = current_results_[i].item->text();

        // Skip if already cached
        if (preview_cache_.find(item_text) != preview_cache_.end()) {
            continue;
        }

        prefetch_queue_.push_back(item_text);
    }
}

void Terminal::preview_worker() {
    // Background thread for async preview rendering with streaming and
    // prefetching. Cancellation protocol: the main thread bumps
    // preview_generation_ whenever it posts a new target (and at shutdown);
    // we capture the generation with each request and stop publishing or
    // caching the moment it goes stale. The streaming child's pid is
    // published in preview_child_pid_ so the main thread can kill its
    // process group -- fread() here can block indefinitely on a command
    // that never exits (tail -f style), where no flag check would run.
    //
    // preview_scroll_offset_ is main-thread state now: the main thread
    // resets it when it posts a target. The worker resetting it per chunk
    // used to snap the user's scroll position back to the top while a
    // preview was still streaming.
    while (true) {
        if (preview_shutdown_.load()) {
            return;
        }
        // PRIORITY 1: Check for high-priority preview request (cursor moved)
        if (preview_pending_.load()) {
            preview_pending_.store(false);

            uint64_t my_generation = preview_generation_.load();
            std::string target_text;
            std::shared_ptr<Item> target_item;
            {
                // Captured together with the request. Reading the live
                // target again at completion time used to cache a
                // superseded render's output under the NEW target's key --
                // wrong-item cache poisoning that persisted until eviction.
                std::lock_guard<std::mutex> lock(preview_mutex_);
                target_text = preview_target_item_;
                target_item = preview_target_item_ptr_;
            }

            try {
                if (opts_.preview_command.empty()) {
                    continue;
                }

                std::string cmd = substitute_placeholders_for_item(
                    opts_.preview_command, target_item);
                if (cmd.empty()) {
                    std::lock_guard<std::mutex> lock(preview_mutex_);
                    preview_content_ = "Error: Preview command is empty";
                    continue;
                }

                ShellPipe pipe = shell_popen(cmd, preview_env_vars());
                if (!pipe.stream) {
                    std::lock_guard<std::mutex> lock(preview_mutex_);
                    preview_content_ = "Error: Could not execute preview command";
                    continue;
                }
                preview_child_pid_.store(pipe.pid);

                std::array<char, 4096> buffer;
                std::string accumulated_output;
                size_t bytes_read;

                bool stale = false;
                while ((bytes_read = fread(buffer.data(), 1, buffer.size(), pipe.stream)) > 0) {
                    if (preview_generation_.load() != my_generation) {
                        stale = true;
                        break;
                    }
                    accumulated_output.append(buffer.data(), bytes_read);

                    // Publish incrementally - text should appear as it streams
                    {
                        std::lock_guard<std::mutex> lock(preview_mutex_);
                        preview_content_ = accumulated_output;
                    }
                    wake_pipe(wake_write_fd_);
                }
                if (preview_generation_.load() != my_generation) {
                    stale = true;
                }

                preview_child_pid_.store(-1);
                shell_pclose(pipe);

                if (!stale) {
                    {
                        std::lock_guard<std::mutex> lock(preview_mutex_);
                        preview_content_ = accumulated_output;
                    }
                    wake_pipe(wake_write_fd_);

                    if (!target_text.empty() && !accumulated_output.empty()) {
                        cache_preview(target_text, accumulated_output);
                    }
                }

            } catch (...) {
                preview_child_pid_.store(-1);
                std::lock_guard<std::mutex> lock(preview_mutex_);
                preview_content_ = "Error: Preview command exception";
            }
        }
        // If no priority request was processed, handle PRIORITY 2: prefetch queue
        else {
            std::string item_to_prefetch;
            {
                std::lock_guard<std::mutex> lock(prefetch_mutex_);
                if (!prefetch_queue_.empty()) {
                    item_to_prefetch = prefetch_queue_.front();
                    prefetch_queue_.pop_front();
                }
            }

            if (!item_to_prefetch.empty()) {
                // Check if still not in cache (might have been added by user navigation)
                if (get_cached_preview(item_to_prefetch).empty()) {
                    // Capture the Item while scanning: substituting by index
                    // after releasing the lock used to race a results update
                    // and cache a different item's output under this key.
                    std::shared_ptr<Item> prefetch_item;
                    {
                        std::lock_guard<std::mutex> lock(results_mutex_);
                        for (const auto& r : current_results_) {
                            if (r.item->text() == item_to_prefetch) {
                                prefetch_item = r.item;
                                break;
                            }
                        }
                    }

                    if (prefetch_item && !opts_.preview_command.empty()) {
                        try {
                            std::string cmd = substitute_placeholders_for_item(
                                opts_.preview_command, prefetch_item);

                            if (!cmd.empty()) {
                                ShellPipe pipe = shell_popen(cmd, preview_env_vars());
                                if (pipe.stream) {
                                    preview_child_pid_.store(pipe.pid);
                                    std::array<char, 4096> buffer;
                                    std::string accumulated_output;
                                    size_t bytes_read;

                                    // Stream output, but check for priority requests frequently
                                    bool interrupted = false;
                                    while (true) {
                                        if (preview_pending_.load() || preview_shutdown_.load()) {
                                            interrupted = true;
                                            break;
                                        }
                                        bytes_read = fread(buffer.data(), 1, buffer.size(), pipe.stream);
                                        if (bytes_read == 0) break;  // clean EOF
                                        accumulated_output.append(buffer.data(), bytes_read);
                                    }

                                    preview_child_pid_.store(-1);
                                    shell_pclose(pipe);

                                    // Only cache a COMPLETE capture. If we broke out because a
                                    // priority request arrived, accumulated_output is truncated —
                                    // possibly mid-image-sequence (sixel/iTerm2/kitty). Caching that
                                    // partial blob means the next scroll to this item serves a
                                    // truncated escape sequence to the terminal, which renders as
                                    // garbage. The old code re-checked preview_pending_ here, but
                                    // that flag can flip back to false once the foreground handler
                                    // consumes it, letting a truncated blob through. Track the exit
                                    // reason explicitly instead.
                                    if (!interrupted && !accumulated_output.empty()) {
                                        cache_preview(item_to_prefetch, accumulated_output);
                                    }
                                }
                            }
                        } catch (...) {
                            preview_child_pid_.store(-1);
                            // Silently ignore prefetch errors
                        }
                    }
                }
            } else {
                // No items to prefetch, wait a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
}

bool Terminal::handle_mouse_event(const KeyEvent& event) {
    const MouseInfo& mouse = event.mouse;

    // Handle scroll wheel - scroll display without moving cursor
    if (mouse.button == MouseInfo::Button::WheelUp) {
        std::lock_guard<std::mutex> lock(results_mutex_);
        if (scroll_offset_ > 0) {
            scroll_offset_--;
        }
        return true;
    }

    if (mouse.button == MouseInfo::Button::WheelDown) {
        std::lock_guard<std::mutex> lock(results_mutex_);
        size_t max_offset = current_results_.size() > visible_lines_
                          ? current_results_.size() - visible_lines_
                          : 0;
        if (scroll_offset_ < max_offset) {
            scroll_offset_++;
        }
        return true;
    }

    // Handle mouse clicks (left button only for now)
    // Note: Double-click is handled by the caller (needs access to the exit path).
    if (mouse.button == MouseInfo::Button::Left && mouse.motion == MouseInfo::Motion::Pressed) {
        // Single click: calculate which result was clicked.
        // The layout is the same top-down order regardless of --reverse/--layout
        // today (a known, pre-existing gap — only this hit-test cared about
        // LayoutType at all, and both branches computed the same thing).
        int results_start_y = 0;

        if (opts_.header_first) {
            if (!current_header_.empty()) {
                results_start_y += 1;  // Header line
            }
            if (!opts_.info_hidden) {
                results_start_y += 1;  // Info line
            }
        } else {
            if (!opts_.info_hidden) {
                results_start_y += 1;  // Info line
            }
            if (!current_header_.empty()) {
                results_start_y += 1;  // Header line
            }
        }

        results_start_y += 1;  // Separator after header/info

        // Account for border offset (border adds 1 row at top)
        if (opts_.border) {
            results_start_y += 1;
        }

        // Calculate clicked result index
        int click_offset = mouse.y - results_start_y;
        if (click_offset < 0 ||
            static_cast<size_t>(click_offset) >= visible_lines_) {
            // Above the list, or on the bottom separator/prompt rows --
            // without the upper bound a click there computed an index past
            // the visible band and yanked the cursor to an off-screen item.
            return true;
        }

        size_t clicked_index = scroll_offset_ + click_offset;

        // Check if ctrl is pressed for multi-select toggle
        bool ctrl_pressed = mouse.ctrl;

        {
            std::lock_guard<std::mutex> lock(results_mutex_);

            if (clicked_index < current_results_.size()) {
                if (ctrl_pressed && opts_.multi) {
                    // Ctrl+click: toggle selection without moving cursor
                    size_t item_idx = current_results_[clicked_index].item->index();
                    if (selected_.find(item_idx) != selected_.end()) {
                        selected_.erase(item_idx);
                    } else {
                        selected_.insert(item_idx);
                    }
                } else {
                    // Normal click: move cursor to clicked item
                    cursor_pos_ = clicked_index;

                    // Ensure cursor is visible (shouldn't be needed for clicks, but just in case)
                    if (cursor_pos_ < scroll_offset_) {
                        scroll_offset_ = cursor_pos_;
                    } else if (cursor_pos_ >= scroll_offset_ + visible_lines_) {
                        scroll_offset_ = cursor_pos_ - visible_lines_ + 1;
                    }
                }
            }
        }

        return true;
    }

    return true;  // Consume all mouse events
}

// --- Query-buffer editing (replaces FTXUI's Input component) ---

void Terminal::query_insert_codepoints(const std::u32string& codepoints) {
    query_codepoints_.insert(query_cursor_, codepoints);
    query_cursor_ += codepoints.size();
    sync_query_from_codepoints();
}

void Terminal::query_backspace() {
    if (query_cursor_ == 0) {
        return;
    }
    query_codepoints_.erase(query_cursor_ - 1, 1);
    query_cursor_--;
    sync_query_from_codepoints();
}

void Terminal::query_delete() {
    if (query_cursor_ >= query_codepoints_.size()) {
        return;
    }
    query_codepoints_.erase(query_cursor_, 1);
    sync_query_from_codepoints();
}

void Terminal::query_move_left() {
    if (query_cursor_ > 0) {
        query_cursor_--;
    }
}

void Terminal::query_move_right() {
    if (query_cursor_ < query_codepoints_.size()) {
        query_cursor_++;
    }
}

// --- Rendering ---

void Terminal::recompute_visible_lines() {
    int term_rows, term_cols;
    get_terminal_size(term_rows, term_cols);

    int info_rows = opts_.info_hidden ? 0 : 1;
    int header_rows = current_header_.empty() ? 0 : 1;
    int ui_overhead = info_rows + header_rows + 1 + 1 + 1;  // info + header + sep + sep + input
    if (opts_.border) {
        ui_overhead += 2;
    }

    // The chrome (separators, prompt) must always fit below the results
    // band: whatever --height asks for, cap at what the terminal leaves
    // after the overhead. --height=100% used to compute prompt_row >=
    // term_rows and silently drop the query line entirely.
    int max_lines = term_rows - ui_overhead;
    if (max_lines < 1) max_lines = 1;

    int computed;
    if (opts_.legacy_height > 0) {
        if (opts_.height_is_percent) {
            computed = (term_rows * opts_.legacy_height) / 100 - ui_overhead;
        } else {
            computed = opts_.legacy_height - ui_overhead;
        }
    } else {
        computed = max_lines;
    }
    if (computed > max_lines) computed = max_lines;
    if (computed < 1) computed = 1;

    std::lock_guard<std::mutex> lock(preview_mutex_);
    visible_lines_ = static_cast<size_t>(computed);
}

void Terminal::repaint(bool preview_dirty) {
    int term_rows, term_cols;
    get_terminal_size(term_rows, term_cols);

    int margin = opts_.border ? 1 : 0;
    int content_cols = term_cols - 2 * margin;
    if (content_cols < 1) content_cols = 1;

    FrameRenderer frame(term_rows, term_cols);

    int row = margin;

    size_t result_count;
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        result_count = current_results_.size();
    }

    std::string info = std::to_string(result_count);
    if (opts_.multi && !selected_.empty()) {
        info += " (" + std::to_string(selected_.size()) + " selected)";
    }

    bool has_header = !current_header_.empty();
    std::string header_text = has_header ? strip_ansi_codes(current_header_) : "";

    auto draw_info_line = [&]() {
        if (!opts_.info_hidden) {
            frame.draw_text(row++, margin, info, Style{}, content_cols);
        }
    };
    auto draw_header_line = [&]() {
        if (has_header) {
            frame.draw_text(row++, margin, header_text, Style{Color::Default, true, false}, content_cols);
        }
    };

    if (opts_.header_first) {
        draw_header_line();
        draw_info_line();
    } else {
        draw_info_line();
        draw_header_line();
    }

    frame.draw_separator(row++, margin, opts_.border ? content_cols : 0);

    // Results area (and preview pane, if enabled) share this vertical band.
    int content_top = row;
    bool show_preview = !opts_.preview_command.empty() && preview_visible_;

    int preview_top = 0, preview_left = 0, preview_lines = 0, preview_cols = 0;
    int results_col = margin;
    int results_width = content_cols;

    if (show_preview) {
        // calculate_column_layout is the single source of truth for the
        // results/separator/preview split; calculate_preview_position derives
        // its preview_top/left/lines/cols from the exact same call, so the
        // separator drawn here and the pane write_preview_content/clear_region
        // paint into can never disagree (a prior divergence let the preview
        // pane overshoot the border and left stale, never-cleared columns
        // between the separator and the pane).
        int col_results_width, col_preview_cols, col_sep_col;
        calculate_column_layout(content_cols, col_results_width, col_preview_cols, col_sep_col);
        calculate_preview_position(preview_top, preview_left, preview_lines, preview_cols);

        results_width = col_results_width;
        if (opts_.preview_position == "left") {
            results_col = margin + col_sep_col + 1;
        }

        // Vertical separator between preview and results.
        int sep_col = margin + col_sep_col;
        for (int r = content_top; r < content_top + static_cast<int>(visible_lines_) && r < term_rows; ++r) {
            frame.draw_text(r, sep_col, "\xE2\x94\x82", Style{}, 1);
        }
    }

    auto visible = get_visible_results();

    for (size_t i = 0; i < visible.size(); ++i) {
        size_t actual_idx = scroll_offset_ + i;
        const auto& result = visible[i];

        bool is_cursor = (actual_idx == cursor_pos_);
        bool is_sel = is_selected(result.item->index());

        std::string line_prefix = (opts_.multi && is_sel) ? "> " : "  ";

        std::string item_text;
        if (!opts_.with_nth.empty() && result.item->has_fields()) {
            std::string display = result.item->get_fields_by_ranges(opts_.with_nth, opts_.legacy_delimiter);
            item_text = !display.empty() ? display : result.item->display_text();
        } else {
            item_text = result.item->display_text();
        }
        if (item_text.find('\x1b') != std::string::npos) {
            item_text = strip_ansi_codes(item_text);
        }

        Row spans;
        const auto& match_positions = result.positions;

        if (!match_positions.empty() && !item_text.empty()) {
            std::u32string u32_text;
            try {
                utf8::utf8to32(item_text.begin(), item_text.end(), std::back_inserter(u32_text));
            } catch (...) {
                u32_text.clear();
            }

            if (!u32_text.empty()) {
                std::set<size_t> highlighted_positions;
                for (const auto& match_pos : match_positions) {
                    if (match_pos.start >= u32_text.size()) continue;
                    size_t end = std::min(static_cast<size_t>(match_pos.end), u32_text.size());
                    for (size_t p = match_pos.start; p < end; ++p) {
                        highlighted_positions.insert(p);
                    }
                }

                spans.push_back(Span{line_prefix, Style{}});

                size_t seg_start = 0;
                bool seg_highlighted = highlighted_positions.count(0) > 0;
                for (size_t p = 1; p <= u32_text.size(); ++p) {
                    bool is_highlighted = (p < u32_text.size()) && (highlighted_positions.count(p) > 0);
                    if (p == u32_text.size() || is_highlighted != seg_highlighted) {
                        std::string seg_text;
                        utf8::utf32to8(u32_text.begin() + static_cast<long>(seg_start),
                                        u32_text.begin() + static_cast<long>(p),
                                        std::back_inserter(seg_text));
                        Style style;
                        if (seg_highlighted) {
                            style.fg = Color::Yellow;
                            style.bold = true;
                        }
                        spans.push_back(Span{seg_text, style});
                        seg_start = p;
                        seg_highlighted = is_highlighted;
                    }
                }
            }
        }

        if (spans.empty()) {
            spans.push_back(Span{line_prefix + item_text, Style{}});
        }

        if (is_cursor) {
            for (auto& span : spans) {
                span.style.inverted = true;
            }
        }

        int row_num = content_top + static_cast<int>(i);
        if (row_num < term_rows) {
            frame.draw_row(row_num, results_col, spans, results_width);
        }
    }

    // Blank out any leftover result rows from a previous, longer frame.
    for (size_t i = visible.size(); i < visible_lines_; ++i) {
        int row_num = content_top + static_cast<int>(i);
        if (row_num >= term_rows) break;
        frame.draw_row(row_num, results_col, {}, results_width);
    }

    int bottom_row = content_top + static_cast<int>(visible_lines_);
    if (bottom_row < term_rows) {
        frame.draw_separator(bottom_row, margin, opts_.border ? content_cols : 0);
    }

    int prompt_row = bottom_row + 1;
    std::string prompt_line = current_prompt_ + current_query_;
    if (prompt_row < term_rows) {
        frame.draw_text(prompt_row, margin, prompt_line, Style{}, content_cols);
    }

    // Border last so content writes can't overwrite its verticals
    // (render.hpp documents this ordering requirement).
    if (opts_.border) {
        frame.draw_border();
    }

    write_all(STDOUT_FILENO, frame.bytes().data(), frame.bytes().size());

    // Move the real cursor to the query-editing position, measured in
    // display columns (a CJK/emoji query is wider than its codepoint count).
    std::string query_before_cursor;
    utf8::utf32to8(query_codepoints_.begin(),
                   query_codepoints_.begin() + static_cast<long>(query_cursor_),
                   std::back_inserter(query_before_cursor));
    int cursor_col = margin + static_cast<int>(visible_width(current_prompt_) +
                                               visible_width(query_before_cursor));
    std::string cursor_seq = "\x1b[" + std::to_string(prompt_row + 1) + ";" +
                              std::to_string(cursor_col + 1) + "H";
    write_all(STDOUT_FILENO, cursor_seq);

    if (show_preview && preview_dirty) {
        std::string preview_text;
        {
            std::lock_guard<std::mutex> lock(preview_mutex_);
            preview_text = preview_content_;
        }

        // Skip the write entirely when neither the content nor the scroll
        // position changed since we last painted this pane. The streaming
        // preview worker wakes a repaint after every chunk it reads, and many
        // unrelated events (keystrokes, item-count updates) also set
        // preview_dirty; without this guard a preview containing a graphics
        // blob (sixel / iTerm2 image / kitty) would re-emit the ENTIRE blob to
        // the terminal on every one of those frames, flooding a sixel terminal
        // (mlterm) with repeated image data that reads as streaming garbage,
        // and making iTerm2 re-decode the image dozens of times a second. The
        // main chrome frame never draws into the preview columns (draw_row pads
        // only to its own budget), so leaving the pane untouched is safe.
        // A resize clears last_painted_valid_ (see the SIGWINCH branch) so the
        // pane is always fully repainted when geometry changes.
        // Clamp the scroll offset against the last known line count BEFORE
        // painting. Painting with an out-of-range offset drew an empty pane
        // and then latched it: the post-paint clamp changed the offset to a
        // value recorded as "already painted", so every later repaint was
        // skipped and the pane stayed blank until the content changed.
        if (preview_total_lines_ > 0 && preview_scroll_offset_ >= preview_total_lines_) {
            preview_scroll_offset_ = preview_total_lines_ - 1;
        }

        if (last_painted_valid_ && preview_text == last_painted_preview_ &&
            preview_scroll_offset_ == last_painted_scroll_) {
            // Nothing to do — recompute the line count for scroll bookkeeping
            // without touching the terminal.
        } else {
            // Clear the preview region first so a smaller/shorter new preview
            // doesn't leave stale content from a larger previous one.
            {
                FrameRenderer clear_frame(term_rows, term_cols);
                clear_frame.clear_region(preview_top, preview_left, preview_lines, preview_cols);
                write_all(STDOUT_FILENO, clear_frame.bytes().data(), clear_frame.bytes().size());
            }

            // Stream the whole preview blob into the pane (see
            // write_preview_content): text lines are positioned per-row,
            // sanitized and clipped, but a graphics blob — iTerm OSC 1337
            // image, kitty APC, sixel — is passed through contiguous and
            // unaltered, and an in-flight UNTERMINATED blob (a chunk read cut
            // mid-image) is held back so an incomplete escape never reaches the
            // terminal. The leading ESC[H ESC[J from scripts like ytsurf's is
            // still stripped so it can't wipe the results list.
            size_t painted_scroll = preview_scroll_offset_;
            size_t total = 0;
            write_preview_content(STDOUT_FILENO, preview_top, preview_left,
                                  preview_text, painted_scroll,
                                  preview_lines, preview_cols, total);
            preview_total_lines_ = total;
            if (preview_total_lines_ > 0 && preview_scroll_offset_ >= preview_total_lines_) {
                preview_scroll_offset_ = preview_total_lines_ - 1;
            }
            last_painted_preview_ = preview_text;
            // Record the offset the pane was actually painted with; if the
            // clamp above just moved preview_scroll_offset_, the mismatch
            // forces one more repaint at the corrected position instead of
            // latching a blank pane.
            last_painted_scroll_ = painted_scroll;
            last_painted_valid_ = true;
        }
    }
}

bool Terminal::dispatch_event(const KeyEvent& event) {
    // Check for expect keys first (matches FTXUI-era ordering: checked before
    // any other handling, including default enter/escape behavior).
    std::string matched_key;
    if (check_expect_key(event, matched_key)) {
        matched_expect_key_ = matched_key;
        accept_selection();
        running_ = false;
        return false;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::Return) {
        auto bind_it = opts_.bindings.find("enter");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
            return running_;
        }
        accept_selection();
        return false;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::Escape) {
        auto bind_it = opts_.bindings.find("esc");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
            return running_;
        }
        accepted_ = false;
        running_ = false;
        return false;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::Tab) {
        auto bind_it = opts_.bindings.find("tab");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else if (opts_.multi) {
            toggle_selection();
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::BackTab) {
        auto bind_it = opts_.bindings.find("btab");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else if (opts_.multi) {
            // fzf default: toggle the current item, then move up.
            toggle_selection();   // auto-advances down...
            move_cursor_up();     // ...undo that...
            move_cursor_up();     // ...and go one above the toggled item.
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::ArrowUp) {
        auto bind_it = opts_.bindings.find("up");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else {
            move_cursor_up();
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::ArrowDown) {
        auto bind_it = opts_.bindings.find("down");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else {
            move_cursor_down();
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::ArrowLeft) {
        auto bind_it = opts_.bindings.find("left");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else {
            query_move_left();
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::ArrowRight) {
        auto bind_it = opts_.bindings.find("right");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else {
            query_move_right();
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::PageUp) {
        auto bind_it = opts_.bindings.find("page-up");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else {
            move_cursor_page_up();
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::PageDown) {
        auto bind_it = opts_.bindings.find("page-down");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else {
            move_cursor_page_down();
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::Home) {
        auto bind_it = opts_.bindings.find("home");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else {
            execute_bind_action("top");
        }
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::End) {
        auto bind_it = opts_.bindings.find("end");
        if (bind_it != opts_.bindings.end()) {
            execute_bind_action(bind_it->second);
        } else {
            execute_bind_action("bottom");
        }
        return running_;
    }

    if (!opts_.no_mouse && event.type == KeyType::Mouse) {
        // Drag/motion reports (?1002 mode) are not clicks: without this,
        // holding the button across one cell decoded as a second "press"
        // within the double-click window and instantly accepted whatever
        // was under the cursor.
        if (event.mouse.motion == MouseInfo::Motion::Moved) {
            return running_;
        }
        if (event.mouse.button == MouseInfo::Button::Left &&
            event.mouse.motion == MouseInfo::Motion::Pressed) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_click_time_);

            int dx = std::abs(event.mouse.x - last_click_x_);
            int dy = std::abs(event.mouse.y - last_click_y_);
            bool is_double_click = (elapsed.count() < 600) && (dx <= 2) && (dy <= 1);

            last_click_time_ = now;
            last_click_x_ = event.mouse.x;
            last_click_y_ = event.mouse.y;

            if (is_double_click) {
                accept_selection();
                running_ = false;
                return false;
            }
        }

        handle_mouse_event(event);
        return running_;
    }

    // Custom bindings on ctrl/alt-modified keys (ctrl-r, ctrl-/, ctrl-space,
    // etc.) that aren't one of the specially-handled navigation keys above.
    {
        std::string bind_key = event_to_bind_key(event);
        if (!bind_key.empty()) {
            auto bind_it = opts_.bindings.find(bind_key);
            if (bind_it != opts_.bindings.end()) {
                execute_bind_action(bind_it->second);
                return running_;
            }
        }
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::Backspace) {
        query_backspace();
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::Delete) {
        query_delete();
        return running_;
    }

    if (event.is_character() && !event.codepoints.empty()) {
        query_insert_codepoints(event.codepoints);
        return running_;
    }

    return running_;
}

bool Terminal::maybe_request_preview() {
    if (opts_.preview_command.empty()) {
        return false;
    }

    std::string current_item_text;
    std::shared_ptr<Item> current_item;
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        if (current_results_.empty() || cursor_pos_ == last_preview_cursor_) {
            return false;
        }
        last_preview_cursor_ = cursor_pos_;
        if (cursor_pos_ < current_results_.size()) {
            current_item = current_results_[cursor_pos_].item;
            current_item_text = current_item->text();
        }
    }

    std::string cached_content = get_cached_preview(current_item_text);
    if (!cached_content.empty()) {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        preview_content_ = cached_content;
        preview_scroll_offset_ = 0;
    } else {
        // Bump the generation FIRST so an in-flight render for the previous
        // target goes stale before the new target is visible, then publish
        // the target, then raise the pending flag last -- the worker reads
        // them in the opposite order, so it can never pair the new flag with
        // the old target.
        supersede_preview();
        {
            std::lock_guard<std::mutex> lock(preview_mutex_);
            preview_content_ = "Loading preview...";
            preview_scroll_offset_ = 0;
            preview_target_item_ = current_item_text;
            preview_target_item_ptr_ = current_item;
        }
        preview_target_cursor_.store(cursor_pos_);
        preview_pending_.store(true);
    }
    return true;
}

std::vector<std::string> Terminal::run() {
    RawMode raw(STDIN_FILENO);
    ScreenGuard screen(!opts_.no_mouse);

    set_tabstop(opts_.tabstop);

    if (!make_self_pipe(winch_read_fd_, winch_write_fd_)) {
        winch_read_fd_ = winch_write_fd_ = -1;
    }
    if (!make_self_pipe(wake_read_fd_, wake_write_fd_)) {
        wake_read_fd_ = wake_write_fd_ = -1;
    }
    if (winch_write_fd_ >= 0) {
        install_sigwinch_handler(winch_write_fd_);
    }

    // SIGTERM/SIGHUP: abort cleanly (restore the terminal, exit 130-style)
    // instead of dying mid-alt-screen with the tty in raw mode.
    g_termination_requested = 0;
    g_termination_wake_fd = winch_write_fd_;
    struct sigaction term_sa {};
    term_sa.sa_handler = termination_handler;
    sigemptyset(&term_sa.sa_mask);
    struct sigaction old_term_sa {}, old_hup_sa {};
    sigaction(SIGTERM, &term_sa, &old_term_sa);
    sigaction(SIGHUP, &term_sa, &old_hup_sa);

    reader_.set_wake_callback([this]() { wake_pipe(wake_write_fd_); });

    // Initialize query
    try {
        utf8::utf8to32(opts_.query.begin(), opts_.query.end(), std::back_inserter(query_codepoints_));
    } catch (...) {
        query_codepoints_.clear();
    }
    query_cursor_ = query_codepoints_.size();
    sync_query_from_codepoints();
    update_results(current_query_);

    // Fire the start: event binding once (e.g. start:reload(...)), letting an
    // external command populate the initial list before the loop begins.
    {
        auto start_it = opts_.bindings.find("start");
        if (start_it != opts_.bindings.end()) {
            execute_bind_action(start_it->second);
        }
    }

    // Start preview worker thread if preview is enabled
    if (!opts_.preview_command.empty()) {
        preview_thread_ = std::thread(&Terminal::preview_worker, this);
    }

    recompute_visible_lines();

    size_t last_item_count = reader_.item_count();
    size_t last_focus_pos = SIZE_MAX;
    bool last_content_different = false;

    KeyParser parser;

    running_ = true;
    // Request the initial preview before the first select(): with a small,
    // already-finished input nothing ever wakes the loop, so a trigger that
    // only ran inside it left the pane on "no preview" until a keystroke.
    maybe_request_preview();
    repaint(/*preview_dirty=*/true);

    int max_fd = STDIN_FILENO;
    if (winch_read_fd_ >= 0) max_fd = std::max(max_fd, winch_read_fd_);
    if (wake_read_fd_ >= 0) max_fd = std::max(max_fd, wake_read_fd_);

    // Escape-sequence timeout as an absolute deadline. Keying it off
    // "select() returned 0" starved it whenever any other fd was busy: the
    // streaming preview worker wakes the pipe on every chunk, so a bare ESC
    // pressed during a stream wasn't resolved until the stream ended.
    // The deadline is re-armed on every feed() that leaves bytes pending,
    // preserving the parser's "one full window of silence" contract.
    auto esc_deadline = std::chrono::steady_clock::now();
    bool esc_deadline_armed = false;

    while (running_) {
        if (g_termination_requested) {
            selected_.clear();
            accepted_ = false;
            break;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        if (winch_read_fd_ >= 0) FD_SET(winch_read_fd_, &read_fds);
        if (wake_read_fd_ >= 0) FD_SET(wake_read_fd_, &read_fds);

        if (parser.has_pending() && !esc_deadline_armed) {
            esc_deadline_armed = true;
            esc_deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(KeyParser::kEscapeTimeoutMs);
        } else if (!parser.has_pending()) {
            esc_deadline_armed = false;
        }

        struct timeval tv;
        bool has_timeout = esc_deadline_armed;
        if (has_timeout) {
            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                esc_deadline - std::chrono::steady_clock::now()).count();
            if (remaining < 0) remaining = 0;
            tv.tv_sec = static_cast<time_t>(remaining / 1000000);
            tv.tv_usec = static_cast<suseconds_t>(remaining % 1000000);
        }

        int n = select(max_fd + 1, &read_fds, nullptr, nullptr, has_timeout ? &tv : nullptr);

        bool needs_repaint = false;
        bool preview_dirty = false;

        if (n < 0) {
            continue;  // EINTR or similar; loop and re-check state
        }

        if (esc_deadline_armed && parser.has_pending() &&
            std::chrono::steady_clock::now() >= esc_deadline) {
            esc_deadline_armed = false;
            auto events = parser.timeout_tick(KeyParser::kEscapeTimeoutMs);
            for (const auto& ev : events) {
                if (!dispatch_event(ev)) break;
                needs_repaint = true;
            }
        }

        if (winch_read_fd_ >= 0 && FD_ISSET(winch_read_fd_, &read_fds)) {
            drain_pipe(winch_read_fd_);
            if (g_termination_requested) {
                continue;  // handled at loop top
            }
            recompute_visible_lines();
            needs_repaint = true;
            preview_dirty = true;  // stale image geometry; force re-render
            last_painted_valid_ = false;  // geometry changed; force full repaint
            if (!opts_.preview_command.empty()) {
                // Cached previews were rendered at the old FZF_PREVIEW_COLUMNS/
                // LINES; serving them into the new pane paints a wrong-size
                // image. The cache is keyed by item text only, so flush it.
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    preview_cache_.clear();
                    preview_lru_.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(prefetch_mutex_);
                    prefetch_queue_.clear();
                }
                last_preview_cursor_ = SIZE_MAX;  // force preview re-invocation
            }
        }

        if (wake_read_fd_ >= 0 && FD_ISSET(wake_read_fd_, &read_fds)) {
            drain_pipe(wake_read_fd_);
            size_t current_item_count = reader_.item_count();
            if (current_item_count != last_item_count) {
                last_item_count = current_item_count;
                update_results(current_query_);
            }
            needs_repaint = true;
            preview_dirty = true;
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char buf[256];
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r == 0) {
                // EOF on the tty (hangup with no SIGHUP delivered): treat as
                // abort. Ignoring it left select() reporting the fd readable
                // forever -- a 100% CPU spin until externally killed.
                selected_.clear();
                accepted_ = false;
                running_ = false;
                break;
            }
            if (r > 0) {
                // New bytes re-arm the escape-timeout window: the parser's
                // contract is a full window of *silence* since the last feed.
                esc_deadline_armed = false;
                auto events = parser.feed(std::string(buf, static_cast<size_t>(r)));
                for (const auto& ev : events) {
                    bool changes_query = ev.is_character() || (ev.type == KeyType::Special &&
                        (ev.special == SpecialKey::Backspace || ev.special == SpecialKey::Delete));
                    // Re-run matching before dispatching any later event in
                    // this same read() batch (e.g. a fast "query\n" paste),
                    // so accept/expect-key events see the filtered results
                    // rather than a stale pre-keystroke list.
                    if (last_content_different && !changes_query) {
                        update_results(current_query_);
                        last_content_different = false;

                        auto change_it = opts_.bindings.find("change");
                        if (change_it != opts_.bindings.end()) {
                            execute_bind_action(change_it->second);
                        }
                    }
                    if (!dispatch_event(ev)) break;
                    if (changes_query) {
                        last_content_different = true;
                    }
                    needs_repaint = true;
                }
            }
        }

        if (!running_) {
            break;
        }

        if (last_content_different) {
            update_results(current_query_);
            last_content_different = false;

            auto change_it = opts_.bindings.find("change");
            if (change_it != opts_.bindings.end()) {
                execute_bind_action(change_it->second);
            }
            needs_repaint = true;
        }

        {
            size_t current_focus_pos;
            {
                std::lock_guard<std::mutex> lock(results_mutex_);
                current_focus_pos = cursor_pos_;
            }
            if (current_focus_pos != last_focus_pos) {
                last_focus_pos = current_focus_pos;
                auto focus_it = opts_.bindings.find("focus");
                if (focus_it != opts_.bindings.end()) {
                    execute_bind_action(focus_it->second);
                }
                needs_repaint = true;
            }
        }

        // Trigger async preview update if cursor position changed.
        if (maybe_request_preview()) {
            preview_dirty = true;
            needs_repaint = true;
        }

        if (needs_repaint) {
            repaint(preview_dirty);
        }
    }

    // Teardown. Screen/raw-mode restore happens via the RAII guards on
    // return (exception-safe); everything else is explicit.
    sigaction(SIGTERM, &old_term_sa, nullptr);
    sigaction(SIGHUP, &old_hup_sa, nullptr);
    g_termination_wake_fd = -1;
    if (winch_write_fd_ >= 0) {
        restore_sigwinch_handler();
    }
    reader_.set_wake_callback(nullptr);

    preview_shutdown_.store(true);
    preview_generation_.fetch_add(1);
    preview_pending_.store(false);
    shell_kill(preview_child_pid_.load(), SIGKILL);
    if (preview_thread_.joinable()) {
        preview_thread_.join();
    }

    if (winch_read_fd_ >= 0) close(winch_read_fd_);
    if (winch_write_fd_ >= 0) close(winch_write_fd_);
    if (wake_read_fd_ >= 0) close(wake_read_fd_);
    if (wake_write_fd_ >= 0) close(wake_write_fd_);

    // Collect selected items
    std::vector<std::string> result;

    // Apply --accept-nth (print only selected fields) and strip ANSI codes.
    // The --ansi flag controls parsing for display, not output.
    auto output_text = [this](const std::shared_ptr<Item>& item) -> std::string {
        std::string text;
        if (!opts_.accept_nth.empty() && item->has_fields()) {
            text = item->get_fields_by_ranges(opts_.accept_nth, opts_.legacy_delimiter);
        } else {
            text = item->text();
        }
        return strip_ansi_codes(text);
    };

    if (accepted_) {
        if (opts_.multi && !selected_.empty()) {
            // Return all selected items (Tab-selected)
            auto items = reader_.get_items();
            for (size_t idx : selected_) {
                if (idx < items.size()) {
                    result.push_back(output_text(items[idx]));
                }
            }
        } else {
            // Return current cursor item (single-select or multi without Tab selections)
            std::lock_guard<std::mutex> lock(results_mutex_);
            if (!current_results_.empty() && cursor_pos_ < current_results_.size()) {
                result.push_back(output_text(current_results_[cursor_pos_].item));
            }
        }
    }

    return result;
}

std::vector<std::string> Terminal::run_filter(const std::string& query) {
    // Wait for all input to be read
    reader_.wait_for_finish();

    // Perform matching
    auto items = reader_.get_items();
    auto results = matcher_.match_items(items, query);

    // Return matched items, honoring --accept-nth
    std::vector<std::string> output;
    for (const auto& result : results) {
        if (!opts_.accept_nth.empty() && result.item->has_fields()) {
            output.push_back(
                result.item->get_fields_by_ranges(opts_.accept_nth, opts_.legacy_delimiter));
        } else {
            output.push_back(result.item->text());
        }
    }

    return output;
}

} // namespace fzf
