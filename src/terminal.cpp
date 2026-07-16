#include "terminal.hpp"
#include "util.hpp"
#include "tty.hpp"
#include "keyparser.hpp"
#include "render.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <array>
#include <chrono>
#include <sys/select.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <utf8.h>

namespace fzf {

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
      preview_visible_(true),  // Preview visible by default (if preview_command set)
      last_click_time_(std::chrono::steady_clock::now()),
      last_click_x_(-1),
      last_click_y_(-1),
      last_preview_cursor_(SIZE_MAX),  // Force initial preview update
      preview_scroll_offset_(0),  // Start at top of preview
      preview_total_lines_(0),    // No preview lines initially
      preview_pending_(false),
      preview_cancel_(false),
      preview_target_cursor_(SIZE_MAX),
      wake_read_fd_(-1),
      wake_write_fd_(-1),
      winch_read_fd_(-1),
      winch_write_fd_(-1)
{
    current_prompt_ = opts_.prompt;
    current_header_ = opts_.header;
}

Terminal::~Terminal() {
    // Signal preview thread to stop
    preview_cancel_.store(true);
    preview_pending_.store(false);

    if (preview_thread_.joinable()) {
        preview_thread_.join();
    }
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
    // Control-character keys (ctrl-a..z, ctrl-space, ctrl-/, etc.) arrive as
    // Character events carrying a single control byte (see keyparser.cpp) —
    // check the raw input byte directly rather than any printability check.
    const std::string& input = event.input;
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
        std::string final_cmd = substitute_placeholders(cmd, cursor_idx);

        // Use specified shell if provided
        std::string shell_cmd;
        if (!opts_.with_shell.empty()) {
            shell_cmd = opts_.with_shell + " '" + final_cmd + "'";
        } else {
            shell_cmd = final_cmd;
        }

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

    size_t plus_pos = action.find('+');
    if (plus_pos != std::string::npos) {
        std::string first = action.substr(0, plus_pos);
        std::string rest = action.substr(plus_pos + 1);

        execute_bind_action(first);
        execute_bind_action(rest);

        return true;
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

    if (action == "preview-down") {
        preview_scroll_offset_++;
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
        preview_scroll_offset_ += visible_lines_;
        return true;
    }

    if (action == "ignore") {
        return true;
    }

    if (action == "clear-query") {
        current_query_.clear();
        query_codepoints_.clear();
        query_cursor_ = 0;
        update_results(current_query_);
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

        size_t cursor_idx;
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            cursor_idx = cursor_pos_;
        }
        std::string final_cmd = substitute_placeholders(cmd, cursor_idx);
        if (!opts_.with_shell.empty()) {
            final_cmd = opts_.with_shell + " '" + final_cmd + "'";
        }

        reader_.load_from_command(final_cmd);

        // Reset cursor/scroll to the top for the fresh item set, then re-filter.
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            cursor_pos_ = 0;
            scroll_offset_ = 0;
        }
        update_results(current_query_);

        // Honor a composite action chained after reload(...)+...
        if (end + 1 < action.length() && action[end + 1] == '+') {
            execute_bind_action(action.substr(end + 2));
        }
        return true;
    }

    if (action.find("change-preview(") == 0) {
        return true;  // Stub: change-preview not implemented
    }

    // transform-header supports both transform-header(CMD) and the colon form
    // transform-header:CMD (fzf's "extends to end of bind string" syntax, used
    // when CMD needs multiple lines or unbalanced parens, e.g. a shell `case`).
    // Try the paren form first since it's unambiguous when present.
    if (action.find("transform-header(") == 0) {
        size_t open = action.find('(');
        std::string cmd;
        size_t end;
        if (!extract_paren_arg(action, open, cmd, end)) {
            return false;
        }

        size_t cursor_idx;
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            cursor_idx = cursor_pos_;
        }
        std::string final_cmd = substitute_placeholders(cmd, cursor_idx);
        if (!opts_.with_shell.empty()) {
            final_cmd = opts_.with_shell + " '" + final_cmd + "'";
        }
        current_header_ = run_command_capture_output(final_cmd);

        if (end + 1 < action.length() && action[end + 1] == '+') {
            execute_bind_action(action.substr(end + 2));
        }
        return true;
    }

    if (action.find("transform-header:") == 0) {
        std::string cmd = action.substr(std::string("transform-header:").length());

        size_t cursor_idx;
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            cursor_idx = cursor_pos_;
        }
        std::string final_cmd = substitute_placeholders(cmd, cursor_idx);
        if (!opts_.with_shell.empty()) {
            final_cmd = opts_.with_shell + " '" + final_cmd + "'";
        }
        current_header_ = run_command_capture_output(final_cmd);
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

    // Split-screen layout with configurable preview position and size
    int preview_width = (term_cols * opts_.preview_size_percent) / 100;
    int results_width = term_cols - preview_width - 1; // -1 for separator

    // Calculate vertical layout:
    // Row 0: Info line (1 row, if not hidden)
    // Row 1: Header (if present, 1 row)
    // Row 2: Separator (1 row)
    // Row 3+: Content area
    // Bottom: Separator (1 row) + Input (1 row)

    int info_rows = opts_.info_hidden ? 0 : 1;
    int header_rows = current_header_.empty() ? 0 : 1;
    int top_ui_rows = info_rows + header_rows + 1; // info + header + separator
    int bottom_ui_rows = 2; // separator + input
    int content_rows = term_rows - top_ui_rows - bottom_ui_rows;

    // Preview starts after top UI elements
    top = top_ui_rows;
    if (opts_.preview_position == "left") {
        left = 0; // Preview starts at left edge
    } else {
        left = results_width + 1; // After results + separator
    }
    lines = content_rows;
    cols = preview_width;

    if (opts_.border) {
        top += 1;
        left += 1;
    }
}

void Terminal::set_preview_env_vars() const {
    // Calculate preview window position and set environment variables
    int preview_top, preview_left, preview_lines, preview_cols;
    calculate_preview_position(preview_top, preview_left, preview_lines, preview_cols);

    std::string top_str = std::to_string(preview_top);
    std::string left_str = std::to_string(preview_left);
    std::string lines_str = std::to_string(preview_lines);
    std::string cols_str = std::to_string(preview_cols);

    setenv("FZF_PREVIEW_TOP", top_str.c_str(), 1);
    setenv("FZF_PREVIEW_LEFT", left_str.c_str(), 1);
    setenv("FZF_PREVIEW_LINES", lines_str.c_str(), 1);
    setenv("FZF_PREVIEW_COLUMNS", cols_str.c_str(), 1);
}

std::string Terminal::substitute_placeholders(const std::string& cmd, size_t index) {
    std::string result = cmd;

    // Get the current line and item if we have results
    std::string line_text;
    std::shared_ptr<Item> item;
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        if (index < current_results_.size()) {
            item = current_results_[index].item;
            line_text = item->text();
        }
    }

    // Replace {n} with 0-based index
    size_t pos = 0;
    while ((pos = result.find("{n}", pos)) != std::string::npos) {
        result.replace(pos, 3, std::to_string(index));
        pos += std::to_string(index).length();
    }

    // Replace {q} with current query
    pos = 0;
    std::string escaped_query = current_query_;
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
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }

    std::string output;
    std::array<char, 4096> buffer;
    size_t bytes_read;
    while ((bytes_read = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        output.append(buffer.data(), bytes_read);
    }
    pclose(pipe);

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
    // Populate prefetch queue with all current results (excluding already cached items)
    std::lock_guard<std::mutex> prefetch_lock(prefetch_mutex_);
    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
    std::lock_guard<std::mutex> results_lock(results_mutex_);

    prefetch_queue_.clear();

    // Add all results to prefetch queue, skipping items already in cache
    for (const auto& result : current_results_) {
        std::string item_text = result.item->text();

        // Skip if already cached
        if (preview_cache_.find(item_text) != preview_cache_.end()) {
            continue;
        }

        prefetch_queue_.push_back(item_text);
    }
}

void Terminal::preview_worker() {
    // Background thread for async preview rendering with streaming and prefetching
    while (true) {
        // PRIORITY 1: Check for high-priority preview request (cursor moved)
        if (preview_pending_.load()) {
            // User moved cursor - handle immediately (highest priority)
            size_t target_cursor = preview_target_cursor_.load();
            preview_pending_.store(false);

            // Execute preview command with streaming output
            try {
                if (opts_.preview_command.empty()) {
                    continue;
                }

                // Substitute placeholders
                std::string cmd = substitute_placeholders(opts_.preview_command, target_cursor);
                if (cmd.empty()) {
                    std::lock_guard<std::mutex> lock(preview_mutex_);
                    preview_content_ = "Error: Preview command is empty";
                    preview_scroll_offset_ = 0;  // Reset scroll on content change
                    continue;
                }

                // Set environment variables for preview command
                set_preview_env_vars();

                // Execute command and stream output
                FILE* pipe = popen(cmd.c_str(), "r");
                if (!pipe) {
                    std::lock_guard<std::mutex> lock(preview_mutex_);
                    preview_content_ = "Error: Could not execute preview command";
                    preview_scroll_offset_ = 0;  // Reset scroll on content change
                    continue;
                }

                // Stream output, updating preview immediately for instant text display
                std::array<char, 4096> buffer;
                std::string accumulated_output;
                size_t bytes_read;

                while (!preview_cancel_.load() && (bytes_read = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
                    accumulated_output.append(buffer.data(), bytes_read);

                    // Update preview immediately - text needs to appear instantly
                    {
                        std::lock_guard<std::mutex> lock(preview_mutex_);
                        preview_content_ = accumulated_output;
                        preview_scroll_offset_ = 0;  // Reset scroll on content change
                    }
                    wake_pipe(wake_write_fd_);

                    // Very small yield to prevent mutex starvation (1ms)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }

                pclose(pipe);

                // Final update if not cancelled
                if (!preview_cancel_.load()) {
                    std::string item_text_for_cache;
                    {
                        std::lock_guard<std::mutex> lock(preview_mutex_);
                        preview_content_ = accumulated_output;
                        preview_scroll_offset_ = 0;  // Reset scroll on content change
                        item_text_for_cache = preview_target_item_;  // Get item text for caching
                    }
                    wake_pipe(wake_write_fd_);

                    // Cache the preview result for instant display on next visit
                    if (!item_text_for_cache.empty() && !accumulated_output.empty()) {
                        cache_preview(item_text_for_cache, accumulated_output);
                    }
                }

            } catch (...) {
                std::lock_guard<std::mutex> lock(preview_mutex_);
                preview_content_ = "Error: Preview command exception";
                preview_scroll_offset_ = 0;  // Reset scroll on content change
            }
        }
        // If no priority request was processed, handle PRIORITY 2: prefetch queue
        else {
            // Check if we should exit
            if (preview_cancel_.load() && !preview_pending_.load()) {
                return;  // Exit thread
            }

            // PRIORITY 2: Process prefetch queue when idle
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
                    // Find the index of this item in current results
                    size_t item_index = SIZE_MAX;
                    {
                        std::lock_guard<std::mutex> lock(results_mutex_);
                        for (size_t i = 0; i < current_results_.size(); ++i) {
                            if (current_results_[i].item->text() == item_to_prefetch) {
                                item_index = i;
                                break;
                            }
                        }
                    }

                    if (item_index != SIZE_MAX) {
                        // Execute preview command for prefetch
                        try {
                            if (!opts_.preview_command.empty()) {
                                std::string cmd = substitute_placeholders(opts_.preview_command, item_index);

                                if (!cmd.empty()) {
                                    // Set environment variables
                                    set_preview_env_vars();

                                    FILE* pipe = popen(cmd.c_str(), "r");
                                    if (pipe) {
                                        std::array<char, 4096> buffer;
                                        std::string accumulated_output;
                                        size_t bytes_read;

                                        // Stream output, but check for priority requests frequently
                                        while (!preview_pending_.load() &&
                                               (bytes_read = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
                                            accumulated_output.append(buffer.data(), bytes_read);
                                            // Small yield to allow priority requests to interrupt
                                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                        }

                                        pclose(pipe);

                                        // Cache result if we weren't interrupted by priority request
                                        if (!preview_pending_.load() && !accumulated_output.empty()) {
                                            cache_preview(item_to_prefetch, accumulated_output);
                                        }
                                    }
                                }
                            }
                        } catch (...) {
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
        if (click_offset < 0) {
            return true;  // Click above results area
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

    std::string utf8_text;
    utf8::utf32to8(query_codepoints_.begin(), query_codepoints_.end(), std::back_inserter(utf8_text));
    current_query_ = utf8_text;
}

void Terminal::query_backspace() {
    if (query_cursor_ == 0) {
        return;
    }
    query_codepoints_.erase(query_cursor_ - 1, 1);
    query_cursor_--;

    std::string utf8_text;
    utf8::utf32to8(query_codepoints_.begin(), query_codepoints_.end(), std::back_inserter(utf8_text));
    current_query_ = utf8_text;
}

void Terminal::query_delete() {
    if (query_cursor_ >= query_codepoints_.size()) {
        return;
    }
    query_codepoints_.erase(query_cursor_, 1);

    std::string utf8_text;
    utf8::utf32to8(query_codepoints_.begin(), query_codepoints_.end(), std::back_inserter(utf8_text));
    current_query_ = utf8_text;
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
    if (opts_.height > 0) {
        if (opts_.height_is_percent) {
            int term_rows, term_cols;
            get_terminal_size(term_rows, term_cols);
            visible_lines_ = static_cast<size_t>((term_rows * opts_.height) / 100);
            if (visible_lines_ < 5) {
                visible_lines_ = 5;
            }
        } else {
            visible_lines_ = static_cast<size_t>(opts_.height);
        }
    } else {
        int term_rows, term_cols;
        get_terminal_size(term_rows, term_cols);

        int info_rows = opts_.info_hidden ? 0 : 1;
        int header_rows = current_header_.empty() ? 0 : 1;
        int ui_overhead = info_rows + header_rows + 1 + 1 + 1;  // info + header + sep + sep + input
        int computed = term_rows - ui_overhead;
        if (opts_.border) {
            computed -= 2;
        }
        visible_lines_ = computed > 0 ? static_cast<size_t>(computed) : 0;
        if (visible_lines_ < 5) {
            visible_lines_ = 5;
        }
    }
}

void Terminal::repaint(bool preview_dirty) {
    int term_rows, term_cols;
    get_terminal_size(term_rows, term_cols);

    int margin = opts_.border ? 1 : 0;
    int content_cols = term_cols - 2 * margin;
    if (content_cols < 1) content_cols = 1;

    FrameRenderer frame(term_rows, term_cols);
    if (opts_.border) {
        frame.draw_border();
    }

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

    frame.draw_separator(row++);

    // Results area (and preview pane, if enabled) share this vertical band.
    int content_top = row;
    bool show_preview = !opts_.preview_command.empty() && preview_visible_;

    int preview_top = 0, preview_left = 0, preview_lines = 0, preview_cols = 0;
    int results_col = margin;
    int results_width = content_cols;

    if (show_preview) {
        calculate_preview_position(preview_top, preview_left, preview_lines, preview_cols);
        int preview_width_with_sep = preview_cols + 1;  // + separator column
        if (opts_.preview_position == "left") {
            results_col = margin + preview_width_with_sep;
            results_width = content_cols - preview_width_with_sep;
        } else {
            results_width = content_cols - preview_width_with_sep;
        }
        if (results_width < 1) results_width = 1;

        // Vertical separator between preview and results.
        int sep_col = (opts_.preview_position == "left")
                          ? margin + preview_cols
                          : margin + results_width;
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
            std::string display = result.item->get_fields_by_ranges(opts_.with_nth, opts_.delimiter);
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
        frame.draw_separator(bottom_row);
    }

    int prompt_row = bottom_row + 1;
    std::string prompt_line = current_prompt_ + current_query_;
    if (prompt_row < term_rows) {
        frame.draw_text(prompt_row, margin, prompt_line, Style{}, content_cols);
    }

    ssize_t written = write(STDOUT_FILENO, frame.bytes().data(), frame.bytes().size());
    (void)written;

    // Move the real cursor to the query-editing position.
    size_t prompt_display_width = visible_width(current_prompt_);
    int cursor_col = margin + static_cast<int>(prompt_display_width + query_cursor_);
    std::string cursor_seq = "\x1b[" + std::to_string(prompt_row + 1) + ";" +
                              std::to_string(cursor_col + 1) + "H";
    ssize_t written2 = write(STDOUT_FILENO, cursor_seq.data(), cursor_seq.size());
    (void)written2;

    if (show_preview && preview_dirty) {
        std::string preview_text;
        {
            std::lock_guard<std::mutex> lock(preview_mutex_);
            preview_text = preview_content_;
        }

        // Clear the preview region first so a smaller/shorter new preview
        // doesn't leave stale content from a larger previous one.
        {
            FrameRenderer clear_frame(term_rows, term_cols);
            clear_frame.clear_region(preview_top, preview_left, preview_lines, preview_cols);
            ssize_t w = write(STDOUT_FILENO, clear_frame.bytes().data(), clear_frame.bytes().size());
            (void)w;
        }

        // Apply scroll offset by dropping leading lines, then clip to the
        // pane's line budget. Raw passthrough: no color re-parsing, no width
        // clipping beyond whole-line truncation, so sixel/kitty-graphics/SGR
        // sequences in preview output reach the terminal unmodified.
        std::vector<std::string> lines = split_lines(preview_text);
        preview_total_lines_ = lines.size();
        if (preview_total_lines_ > 0 && preview_scroll_offset_ >= preview_total_lines_) {
            preview_scroll_offset_ = preview_total_lines_ - 1;
        }

        std::string visible_preview;
        size_t shown = 0;
        for (size_t li = preview_scroll_offset_; li < lines.size() && shown < static_cast<size_t>(preview_lines); ++li, ++shown) {
            if (shown > 0) visible_preview += "\r\n";
            visible_preview += lines[li];
        }

        write_raw_passthrough(STDOUT_FILENO, preview_top, preview_left, visible_preview);
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
        move_cursor_page_up();
        return running_;
    }

    if (event.type == KeyType::Special && event.special == SpecialKey::PageDown) {
        move_cursor_page_down();
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

std::vector<std::string> Terminal::run() {
    RawMode raw(STDIN_FILENO);

    enter_alt_screen(STDOUT_FILENO);
    hide_cursor(STDOUT_FILENO);
    if (!opts_.no_mouse) {
        enable_mouse(STDOUT_FILENO);
    }

    if (!make_self_pipe(winch_read_fd_, winch_write_fd_)) {
        winch_read_fd_ = winch_write_fd_ = -1;
    }
    if (!make_self_pipe(wake_read_fd_, wake_write_fd_)) {
        wake_read_fd_ = wake_write_fd_ = -1;
    }
    if (winch_write_fd_ >= 0) {
        install_sigwinch_handler(winch_write_fd_);
    }

    reader_.set_wake_callback([this]() { wake_pipe(wake_write_fd_); });

    // Initialize query
    current_query_ = opts_.query;
    try {
        utf8::utf8to32(current_query_.begin(), current_query_.end(), std::back_inserter(query_codepoints_));
    } catch (...) {
        query_codepoints_.clear();
    }
    query_cursor_ = query_codepoints_.size();
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
    repaint(/*preview_dirty=*/true);

    int max_fd = std::max({STDIN_FILENO, winch_read_fd_, wake_read_fd_});

    while (running_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(winch_read_fd_, &read_fds);
        FD_SET(wake_read_fd_, &read_fds);

        bool has_timeout = parser.has_pending();
        struct timeval tv;
        if (has_timeout) {
            tv.tv_sec = KeyParser::kEscapeTimeoutMs / 1000;
            tv.tv_usec = (KeyParser::kEscapeTimeoutMs % 1000) * 1000;
        }

        int n = select(max_fd + 1, &read_fds, nullptr, nullptr, has_timeout ? &tv : nullptr);

        bool needs_repaint = false;
        bool preview_dirty = false;

        if (n < 0) {
            continue;  // EINTR or similar; loop and re-check state
        }

        if (n == 0 && parser.has_pending()) {
            auto events = parser.timeout_tick(KeyParser::kEscapeTimeoutMs);
            for (const auto& ev : events) {
                if (!dispatch_event(ev)) break;
                needs_repaint = true;
            }
        }

        if (FD_ISSET(winch_read_fd_, &read_fds)) {
            drain_pipe(winch_read_fd_);
            recompute_visible_lines();
            needs_repaint = true;
            preview_dirty = true;  // stale image geometry; force re-render
            if (!opts_.preview_command.empty() && !current_results_.empty()) {
                last_preview_cursor_ = SIZE_MAX;  // force preview re-invocation
            }
        }

        if (FD_ISSET(wake_read_fd_, &read_fds)) {
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
            if (r > 0) {
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
        if (!opts_.preview_command.empty() && !current_results_.empty()) {
            if (cursor_pos_ != last_preview_cursor_) {
                last_preview_cursor_ = cursor_pos_;

                std::string current_item_text;
                if (cursor_pos_ < current_results_.size()) {
                    current_item_text = current_results_[cursor_pos_].item->text();
                }

                std::string cached_content = get_cached_preview(current_item_text);
                if (!cached_content.empty()) {
                    std::lock_guard<std::mutex> lock(preview_mutex_);
                    preview_content_ = cached_content;
                    preview_scroll_offset_ = 0;
                } else {
                    preview_cancel_.store(true);
                    {
                        std::lock_guard<std::mutex> lock(preview_mutex_);
                        preview_content_ = "Loading preview...";
                        preview_scroll_offset_ = 0;
                        preview_target_item_ = current_item_text;
                    }
                    preview_target_cursor_.store(cursor_pos_);
                    preview_pending_.store(true);
                    preview_cancel_.store(false);
                }
                preview_dirty = true;
                needs_repaint = true;
            }
        }

        if (needs_repaint) {
            repaint(preview_dirty);
        }
    }

    // Teardown, matching the RawMode/alt-screen/mouse setup at the top.
    if (!opts_.no_mouse) {
        disable_mouse(STDOUT_FILENO);
    }
    show_cursor(STDOUT_FILENO);
    leave_alt_screen(STDOUT_FILENO);
    if (winch_write_fd_ >= 0) {
        restore_sigwinch_handler();
    }
    reader_.set_wake_callback(nullptr);

    preview_cancel_.store(true);
    preview_pending_.store(false);
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
            text = item->get_fields_by_ranges(opts_.accept_nth, opts_.delimiter);
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
                result.item->get_fields_by_ranges(opts_.accept_nth, opts_.delimiter));
        } else {
            output.push_back(result.item->text());
        }
    }

    return output;
}

} // namespace fzf
