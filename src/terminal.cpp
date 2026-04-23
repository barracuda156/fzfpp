#include "terminal.hpp"
#include "util.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <array>
#include <chrono>
#include <unistd.h>
#include <sys/ioctl.h>
#include <utf8.h>

namespace fzf {

using namespace ftxui;

Terminal::Terminal(const Options& opts, Reader& reader)
    : opts_(opts),
      reader_(reader),
      matcher_(opts.case_mode, opts.algo),
      cursor_pos_(0),
      scroll_offset_(0),
      running_(false),
      accepted_(false),
      matched_expect_key_(""),
      search_pending_(false),
      search_running_(false),
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
      preview_target_cursor_(SIZE_MAX)
{
}

Terminal::~Terminal() {
    // Signal preview thread to stop
    preview_cancel_.store(true);
    preview_pending_.store(false);

    if (preview_thread_.joinable()) {
        preview_thread_.join();
    }

    if (search_thread_.joinable()) {
        search_thread_.join();
    }
}

void Terminal::update_results(const std::string& query) {
    // Get current items
    auto items = reader_.get_items();

    std::vector<MatchResult> new_results;

    if (query.empty()) {
        // No query: show all items
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

void Terminal::perform_search() {
    search_running_.store(true, std::memory_order_release);

    while (search_pending_.load(std::memory_order_acquire)) {
        search_pending_.store(false, std::memory_order_release);

        std::string query_copy;
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            query_copy = current_query_;
        }

        update_results(query_copy);
    }

    search_running_.store(false, std::memory_order_release);
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

bool Terminal::check_expect_key(const ftxui::Event& event, std::string& matched_key) {
    if (opts_.expect_keys.empty()) {
        return false;
    }

    std::string input = event.input();

    // Check for special FTXUI events first
    for (const auto& expect : opts_.expect_keys) {
        // Return/Enter key
        if (expect == "enter" && event == Event::Return) {
            matched_key = "enter";
            return true;
        }

        // Escape key
        if (expect == "esc" && event == Event::Escape) {
            matched_key = "esc";
            return true;
        }

        // Tab key
        if (expect == "tab" && event == Event::Tab) {
            matched_key = "tab";
            return true;
        }

        // Arrow keys (FTXUI events)
        if (expect == "up" && event == Event::ArrowUp) {
            matched_key = "up";
            return true;
        }
        if (expect == "down" && event == Event::ArrowDown) {
            matched_key = "down";
            return true;
        }
        if (expect == "left" && event == Event::ArrowLeft) {
            matched_key = "left";
            return true;
        }
        if (expect == "right" && event == Event::ArrowRight) {
            matched_key = "right";
            return true;
        }

        // Page Up/Down
        if (expect == "page-up" && event == Event::PageUp) {
            matched_key = "page-up";
            return true;
        }
        if (expect == "page-down" && event == Event::PageDown) {
            matched_key = "page-down";
            return true;
        }

        // Home/End
        if (expect == "home" && event == Event::Home) {
            matched_key = "home";
            return true;
        }
        if (expect == "end" && event == Event::End) {
            matched_key = "end";
            return true;
        }
    }

    // Map of ANSI sequences to key names for shift+arrow and other special keys
    // Try multiple variants as different terminals may use different sequences
    std::map<std::string, std::string> key_map = {
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

        // Background the command so it doesn't block FTXUI
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
        update_results(current_query_);
        return true;
    }

    if (action.find("become(") == 0) {
        return true;  // Stub: process replacement not implemented
    }

    if (action.find("reload(") == 0) {
        return true;  // Stub: dynamic reload not implemented
    }

    if (action.find("change-preview(") == 0) {
        return true;  // Stub: change-preview not implemented
    }

    if (action.find("change-prompt(") == 0) {
        return true;  // Stub: change-prompt not implemented
    }

    return false;
}


void Terminal::get_terminal_size(int& rows, int& cols) const {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        rows = w.ws_row;
        cols = w.ws_col;
    } else {
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
    int header_rows = opts_.header.empty() ? 0 : 1;
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

bool Terminal::handle_mouse_event(Event event) {
    auto mouse = event.mouse();

    // Handle scroll wheel - scroll display without moving cursor
    if (mouse.button == Mouse::WheelUp) {
        std::lock_guard<std::mutex> lock(results_mutex_);
        if (scroll_offset_ > 0) {
            scroll_offset_--;
        }
        return true;
    }

    if (mouse.button == Mouse::WheelDown) {
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
    // Note: Double-click is handled in the event loop (needs access to exit())
    if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
        // Single click: calculate which result was clicked
        // The layout is complex, so we use a heuristic approach:
        // - Count UI elements above the results area
        // - Map click Y to result index

        int results_start_y;
        if (opts_.layout == LayoutType::Reverse) {
            // Reverse layout: header/info at top, results below, input at bottom
            results_start_y = 0;

            // Add header/info based on header_first setting
            if (opts_.header_first) {
                if (!opts_.header.empty()) {
                    results_start_y += 1;  // Header line
                }
                if (!opts_.info_hidden) {
                    results_start_y += 1;  // Info line
                }
            } else {
                if (!opts_.info_hidden) {
                    results_start_y += 1;  // Info line
                }
                if (!opts_.header.empty()) {
                    results_start_y += 1;  // Header line
                }
            }

            results_start_y += 1;  // Separator after header/info
        } else {
            // Default layout: same order as reverse, just affects result ordering
            results_start_y = 0;

            // Add header/info based on header_first setting
            if (opts_.header_first) {
                if (!opts_.header.empty()) {
                    results_start_y += 1;  // Header line
                }
                if (!opts_.info_hidden) {
                    results_start_y += 1;  // Info line
                }
            } else {
                if (!opts_.info_hidden) {
                    results_start_y += 1;  // Info line
                }
                if (!opts_.header.empty()) {
                    results_start_y += 1;  // Header line
                }
            }

            results_start_y += 1;  // Separator after header/info
        }

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
        bool ctrl_pressed = event.is_mouse() && (mouse.control);

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

std::vector<std::string> Terminal::run() {
    auto screen = ScreenInteractive::Fullscreen();
    auto exit = screen.ExitLoopClosure();  // Get exit closure FIRST

    // Initialize query
    current_query_ = opts_.query;
    update_results(current_query_);

    // Start preview worker thread if preview is enabled
    if (!opts_.preview_command.empty()) {
        preview_thread_ = std::thread(&Terminal::preview_worker, this);
    }

    // Calculate visible lines based on actual terminal size
    if (opts_.height > 0) {
        if (opts_.height_is_percent) {
            // Calculate percentage of terminal height
            int term_rows, term_cols;
            get_terminal_size(term_rows, term_cols);
            visible_lines_ = (term_rows * opts_.height) / 100;

            // Ensure at least 5 lines visible
            if (visible_lines_ < 5) {
                visible_lines_ = 5;
            }
        } else {
            // Absolute line count
            visible_lines_ = opts_.height;
        }
    } else {
        // Auto-calculate from terminal size
        int term_rows, term_cols;
        get_terminal_size(term_rows, term_cols);

        // Calculate available space for results
        // UI layout: info(0-1) + header(0-1) + separator(1) + content + separator(1) + input(1)
        int info_rows = opts_.info_hidden ? 0 : 1;
        int header_rows = opts_.header.empty() ? 0 : 1;
        int ui_overhead = info_rows + header_rows + 1 + 1 + 1;  // info + header + sep + sep + input
        visible_lines_ = term_rows - ui_overhead;

        // Ensure at least 5 lines visible
        if (visible_lines_ < 5) {
            visible_lines_ = 5;
        }
    }

    // Input component - we'll handle events manually
    std::string input_content = current_query_;
    auto input = Input(&input_content, opts_.prompt);

    // Track if we need to update search
    bool last_content_different = false;

    // Create a component that handles ALL events before passing to Input
    auto component = CatchEvent(input, [&, exit](Event event) {
        // Handle critical events FIRST, before Input sees them

        // Check for expect keys
        std::string matched_key;
        if (check_expect_key(event, matched_key)) {
            matched_expect_key_ = matched_key;
            accept_selection();
            running_ = false;
            exit();
            return true;
        }

        if (event == Event::Return) {
            // Check for custom enter binding
            auto bind_it = opts_.bindings.find("enter");
            if (bind_it != opts_.bindings.end()) {
                // Execute custom binding using the common handler
                execute_bind_action(bind_it->second);
            } else {
                // Default behavior: accept selection and exit
                accept_selection();
                exit();
            }
            return true;
        }

        if (event == Event::Escape) {
            running_ = false;
            exit();  // Actually exit the loop
            return true;
        }

        if (event == Event::Tab) {
            // Check for custom tab binding
            auto bind_it = opts_.bindings.find("tab");
            if (bind_it != opts_.bindings.end()) {
                execute_bind_action(bind_it->second);
            } else if (opts_.multi) {
                // Default behavior: toggle selection and move down
                toggle_selection();
            }
            return true;  // Consume the event
        }

        if (event == Event::ArrowUp) {
            // Check for custom up binding
            auto bind_it = opts_.bindings.find("up");
            if (bind_it != opts_.bindings.end()) {
                execute_bind_action(bind_it->second);
            } else {
                move_cursor_up();
            }
            return true;
        }

        if (event == Event::ArrowDown) {
            // Check for custom down binding
            auto bind_it = opts_.bindings.find("down");
            if (bind_it != opts_.bindings.end()) {
                execute_bind_action(bind_it->second);
            } else {
                move_cursor_down();
            }
            return true;
        }

        if (event == Event::ArrowLeft) {
            // Check for custom left binding
            auto bind_it = opts_.bindings.find("left");
            if (bind_it != opts_.bindings.end()) {
                execute_bind_action(bind_it->second);
            }
            // No default behavior for left arrow
            return true;
        }

        if (event == Event::ArrowRight) {
            // Check for custom right binding
            auto bind_it = opts_.bindings.find("right");
            if (bind_it != opts_.bindings.end()) {
                execute_bind_action(bind_it->second);
            }
            // No default behavior for right arrow
            return true;
        }

        if (event == Event::PageUp) {
            move_cursor_page_up();
            return true;
        }

        if (event == Event::PageDown) {
            move_cursor_page_down();
            return true;
        }

        if (event == Event::Home) {
            // Check for custom home binding
            auto bind_it = opts_.bindings.find("home");
            if (bind_it != opts_.bindings.end()) {
                execute_bind_action(bind_it->second);
            } else {
                // Default behavior: jump to first item
                execute_bind_action("top");
            }
            return true;
        }

        if (event == Event::End) {
            // Check for custom end binding
            auto bind_it = opts_.bindings.find("end");
            if (bind_it != opts_.bindings.end()) {
                execute_bind_action(bind_it->second);
            } else {
                // Default behavior: jump to last item
                execute_bind_action("bottom");
            }
            return true;
        }

        // Mouse events
        if (!opts_.no_mouse && event.is_mouse()) {
            auto mouse = event.mouse();

            // Check for double-click first (needs access to exit())
            if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_click_time_);

                int dx = std::abs(mouse.x - last_click_x_);
                int dy = std::abs(mouse.y - last_click_y_);
                bool is_double_click = (elapsed.count() < 600) && (dx <= 2) && (dy <= 1);

                last_click_time_ = now;
                last_click_x_ = mouse.x;
                last_click_y_ = mouse.y;

                if (is_double_click) {
                    // Double-click: accept and exit
                    accept_selection();
                    running_ = false;
                    exit();
                    return true;
                }
            }

            // Handle other mouse events
            return handle_mouse_event(event);
        }

        if (event.is_character()) {
            // Character input - will trigger search update
            last_content_different = true;
            return false;  // Let Input handle the character
        }

        if (event == Event::Backspace || event == Event::Delete) {
            last_content_different = true;
            return false;  // Let Input handle the deletion
        }

        return false;  // Let Input handle other events
    });

    // Track last item count to detect when reader adds new items
    // Initialize to current count since we just called update_results() above
    size_t last_item_count = reader_.item_count();

    // Renderer
    auto renderer = Renderer(component, [&] {
        // Check if reader has new items
        size_t current_item_count = reader_.item_count();
        bool items_changed = (current_item_count != last_item_count);
        if (items_changed) {
            last_item_count = current_item_count;
            update_results(current_query_);
        }

        // Update search if query changed
        if (last_content_different) {
            current_query_ = input_content;
            update_results(current_query_);
            last_content_different = false;
        }

        // Get visible results
        auto visible = get_visible_results();

        // Build result elements
        Elements result_elements;
        size_t result_count = 0;
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            result_count = current_results_.size();
        }

        for (size_t i = 0; i < visible.size(); ++i) {
            size_t actual_idx = scroll_offset_ + i;
            const auto& result = visible[i];

            bool is_cursor = (actual_idx == cursor_pos_);
            bool is_sel = is_selected(result.item->index());

            // Build line prefix (selection marker)
            std::string line_prefix;
            if (opts_.multi && is_sel) {
                line_prefix = "> ";
            } else {
                line_prefix = "  ";
            }

            // Get item text (without prefix)
            std::string item_text;
            if (!opts_.with_nth.empty()) {
                if (result.item->has_fields()) {
                    std::string display = result.item->get_display_fields(opts_.with_nth);
                    if (!display.empty()) {
                        item_text = display;
                    } else {
                        item_text = result.item->display_text();
                    }
                } else {
                    item_text = result.item->display_text();
                }
            } else {
                // Use display_text (ANSI codes already stripped during parsing)
                item_text = result.item->display_text();
            }

            // display_text() already has ANSI codes stripped, no need to strip again
            // (Unless field display added them back, so strip to be safe)
            if (item_text.find('\x1b') != std::string::npos) {
                item_text = strip_ansi_codes(item_text);
            }

            // Build element with match highlighting
            Element element;
            bool highlighting_success = false;

            // Try to apply match highlighting
            const auto& match_positions = result.positions;
            if (!match_positions.empty() && !item_text.empty()) {
                try {
                    // Convert item_text to UTF-32 for position indexing
                    std::u32string u32_text;
                    utf8::utf8to32(item_text.begin(), item_text.end(), std::back_inserter(u32_text));

                    // Create a set of highlighted positions (with bounds checking)
                    std::set<size_t> highlighted_positions;
                    for (const auto& match_pos : match_positions) {
                        // Validate and clamp positions
                        if (match_pos.start >= u32_text.size()) continue;
                        size_t end = std::min(static_cast<size_t>(match_pos.end), u32_text.size());

                        for (size_t i = match_pos.start; i < end; ++i) {
                            highlighted_positions.insert(i);
                        }
                    }

                    // Build segments with highlighting
                    if (!highlighted_positions.empty()) {
                        Elements line_elements;

                        // Add prefix first
                        line_elements.push_back(text(line_prefix));

                        // Build text segments by grouping consecutive highlighted/non-highlighted chars
                        size_t seg_start = 0;
                        bool seg_highlighted = highlighted_positions.count(0) > 0;

                        for (size_t i = 1; i <= u32_text.size(); ++i) {
                            bool is_highlighted = (i < u32_text.size()) && (highlighted_positions.count(i) > 0);

                            // Check if we need to start a new segment
                            if (i == u32_text.size() || is_highlighted != seg_highlighted) {
                                // Convert UTF-32 segment back to UTF-8
                                std::string seg_text;
                                utf8::utf32to8(u32_text.begin() + seg_start, u32_text.begin() + i,
                                             std::back_inserter(seg_text));

                                // Create text element with optional highlighting
                                Element seg_elem = text(seg_text);
                                if (seg_highlighted) {
                                    seg_elem = seg_elem | color(Color::Yellow) | bold;
                                }

                                line_elements.push_back(seg_elem);

                                // Start new segment
                                seg_start = i;
                                seg_highlighted = is_highlighted;
                            }
                        }

                        element = hbox(line_elements);
                        highlighting_success = true;
                    }
                } catch (...) {
                    // UTF-8 conversion or other error - fall back to plain text
                    highlighting_success = false;
                }
            }

            // Fallback: render without highlighting
            if (!highlighting_success) {
                std::string full_line = line_prefix + item_text;
                if (wrap_lines_) {
                    element = paragraph(full_line);
                } else {
                    element = text(full_line);
                }
            }

            // Apply cursor inversion
            if (is_cursor) {
                element = element | inverted;
            }

            result_elements.push_back(element);
        }

        // Trigger async preview update if cursor position changed
        if (!opts_.preview_command.empty() && !current_results_.empty()) {
            if (cursor_pos_ != last_preview_cursor_) {
                last_preview_cursor_ = cursor_pos_;

                // Get current item text for cache lookup
                std::string current_item_text;
                if (cursor_pos_ < current_results_.size()) {
                    current_item_text = current_results_[cursor_pos_].item->text();
                }

                // Check cache first for instant preview display
                std::string cached_content = get_cached_preview(current_item_text);
                if (!cached_content.empty()) {
                    // Cache hit! Display immediately (instant like golang fzf)
                    {
                        std::lock_guard<std::mutex> lock(preview_mutex_);
                        preview_content_ = cached_content;
                        preview_scroll_offset_ = 0;  // Reset scroll on content change
                    }
                    // No need to execute preview command
                } else {
                    // Cache miss - need to execute preview command

                    // Cancel any running preview
                    preview_cancel_.store(true);

                    // Show "Loading..." immediately (like browser loading page)
                    // Will be replaced by actual content as soon as command outputs
                    {
                        std::lock_guard<std::mutex> lock(preview_mutex_);
                        preview_content_ = "Loading preview...";
                        preview_scroll_offset_ = 0;  // Reset scroll on content change
                        preview_target_item_ = current_item_text;  // Store item text for caching
                    }

                    // Request new preview (will start immediately and stream output)
                    preview_target_cursor_.store(cursor_pos_);
                    preview_pending_.store(true);
                    preview_cancel_.store(false);
                }
            }
        }

        // Info line
        std::string info = std::to_string(result_count);
        if (opts_.multi && !selected_.empty()) {
            info += " (" + std::to_string(selected_.size()) + " selected)";
        }

        // Build main results box
        auto results_box = vbox(result_elements) | frame;

        // Build preview box if preview is enabled
        Elements layout_elements;

        // Build header element (if present)
        Element header_element;
        bool has_header = !opts_.header.empty();

        if (has_header) {
            // Check if header contains ANSI codes
            if (opts_.header.find('\x1b') != std::string::npos) {
                // Strip ANSI codes from header to avoid junk display
                header_element = text(strip_ansi_codes(opts_.header)) | bold;
            } else {
                header_element = text(opts_.header) | bold;
            }
        }

        // Add header and info line in the correct order
        if (opts_.header_first) {
            // Header first mode: header before info
            if (has_header) {
                layout_elements.push_back(header_element);
            }
            if (!opts_.info_hidden) {
                layout_elements.push_back(text(info) | hcenter);
            }
        } else {
            // Default mode: info before header
            if (!opts_.info_hidden) {
                layout_elements.push_back(text(info) | hcenter);
            }
            if (has_header) {
                layout_elements.push_back(header_element);
            }
        }

        layout_elements.push_back(separator());

        // Main content area with optional preview
        if (!opts_.preview_command.empty() && preview_visible_) {
            // Split screen: results on left, preview on right
            std::string preview_text;
            {
                std::lock_guard<std::mutex> lock(preview_mutex_);
                preview_text = preview_content_;
            }

            // Parse preview text into lines - first collect all lines
            std::istringstream stream(preview_text);
            std::string line;
            std::vector<std::string> all_preview_lines;

            while (std::getline(stream, line)) {
                all_preview_lines.push_back(line);
            }

            // Update total lines count
            preview_total_lines_ = all_preview_lines.size();

            // Clamp scroll offset to valid range
            if (preview_total_lines_ > 0 && preview_scroll_offset_ >= preview_total_lines_) {
                preview_scroll_offset_ = preview_total_lines_ - 1;
            }

            // Render only visible window of lines
            Elements preview_elements;
            size_t visible_start = preview_scroll_offset_;
            size_t visible_end = all_preview_lines.size();  // Show all remaining lines (FTXUI will clip)

            for (size_t line_idx = visible_start; line_idx < visible_end; line_idx++) {
                const std::string& line = all_preview_lines[line_idx];
                try {
                    // Parse ANSI colored segments
                    auto segments = parse_ansi_colored_text(line);

                    if (segments.empty()) {
                        preview_elements.push_back(text(""));
                        continue;
                    }

                    // Build colored line
                    Elements line_elements;
                    for (const auto& seg : segments) {
                        if (seg.text.empty()) continue;

                        Element elem = text(seg.text);

                    // Apply foreground color if specified
                    if (seg.fg_is_rgb) {
                        // RGB true color mode
                        elem = elem | color(Color::RGB(seg.fg_rgb[0], seg.fg_rgb[1], seg.fg_rgb[2]));
                    } else if (seg.fg_color >= 0 && seg.fg_color < 16) {
                        // Basic 16 colors (Palette16)
                        Color ftxui_color;
                        switch (seg.fg_color) {
                            case 0: ftxui_color = Color::Black; break;
                            case 1: ftxui_color = Color::Red; break;
                            case 2: ftxui_color = Color::Green; break;
                            case 3: ftxui_color = Color::Yellow; break;
                            case 4: ftxui_color = Color::Blue; break;
                            case 5: ftxui_color = Color::Magenta; break;
                            case 6: ftxui_color = Color::Cyan; break;
                            case 7: ftxui_color = Color::GrayLight; break;
                            case 8: ftxui_color = Color::GrayDark; break;
                            case 9: ftxui_color = Color::RedLight; break;
                            case 10: ftxui_color = Color::GreenLight; break;
                            case 11: ftxui_color = Color::YellowLight; break;
                            case 12: ftxui_color = Color::BlueLight; break;
                            case 13: ftxui_color = Color::MagentaLight; break;
                            case 14: ftxui_color = Color::CyanLight; break;
                            case 15: ftxui_color = Color::White; break;
                            default: ftxui_color = Color::Default; break;
                        }
                        elem = elem | color(ftxui_color);
                    } else if (seg.fg_color >= 16 && seg.fg_color <= 255) {
                        // 256-color palette
                        elem = elem | color(Color::Palette256(seg.fg_color));
                    }

                    // Apply background color if specified
                    if (seg.bg_is_rgb) {
                        // RGB true color mode
                        elem = elem | bgcolor(Color::RGB(seg.bg_rgb[0], seg.bg_rgb[1], seg.bg_rgb[2]));
                    } else if (seg.bg_color >= 0 && seg.bg_color < 16) {
                        // Basic 16 colors (Palette16)
                        Color ftxui_bgcolor;
                        switch (seg.bg_color) {
                            case 0: ftxui_bgcolor = Color::Black; break;
                            case 1: ftxui_bgcolor = Color::Red; break;
                            case 2: ftxui_bgcolor = Color::Green; break;
                            case 3: ftxui_bgcolor = Color::Yellow; break;
                            case 4: ftxui_bgcolor = Color::Blue; break;
                            case 5: ftxui_bgcolor = Color::Magenta; break;
                            case 6: ftxui_bgcolor = Color::Cyan; break;
                            case 7: ftxui_bgcolor = Color::GrayLight; break;
                            case 8: ftxui_bgcolor = Color::GrayDark; break;
                            case 9: ftxui_bgcolor = Color::RedLight; break;
                            case 10: ftxui_bgcolor = Color::GreenLight; break;
                            case 11: ftxui_bgcolor = Color::YellowLight; break;
                            case 12: ftxui_bgcolor = Color::BlueLight; break;
                            case 13: ftxui_bgcolor = Color::MagentaLight; break;
                            case 14: ftxui_bgcolor = Color::CyanLight; break;
                            case 15: ftxui_bgcolor = Color::White; break;
                            default: ftxui_bgcolor = Color::Default; break;
                        }
                        elem = elem | bgcolor(ftxui_bgcolor);
                    } else if (seg.bg_color >= 16 && seg.bg_color <= 255) {
                        // 256-color palette
                        elem = elem | bgcolor(Color::Palette256(seg.bg_color));
                    }

                    // Apply bold if specified
                    if (seg.bold) {
                        elem = elem | bold;
                    }

                    line_elements.push_back(elem);
                }

                // Combine segments into a single line
                preview_elements.push_back(hbox(line_elements));
                } catch (...) {
                    // If color parsing fails, fall back to plain text
                    preview_elements.push_back(text(strip_ansi_codes(line)));
                }
            }

            auto preview_box = vbox(preview_elements) | frame;

            // Horizontal split with configurable position
            if (opts_.preview_position == "left") {
                layout_elements.push_back(
                    hbox({
                        preview_box | flex,
                        separator(),
                        results_box | size(WIDTH, GREATER_THAN, 40)
                    }) | flex
                );
            } else {
                // Default: preview on right
                layout_elements.push_back(
                    hbox({
                        results_box | size(WIDTH, GREATER_THAN, 40),
                        separator(),
                        preview_box | flex
                    }) | flex
                );
            }
        } else {
            // No preview, just results
            layout_elements.push_back(results_box | flex);
        }

        layout_elements.push_back(separator());
        layout_elements.push_back(hbox({text(opts_.prompt), component->Render()}));

        auto layout = vbox(layout_elements);

        // Apply border if requested
        if (opts_.border) {
            return layout | border;
        }

        return layout;
    });

    // Background thread to trigger screen updates when items arrive
    // Only runs while reader is active and checks every 200ms (low overhead)
    std::atomic<bool> update_thread_running{true};
    size_t last_seen_count = last_item_count;
    std::thread update_thread([&]() {
        while (update_thread_running.load() && !reader_.is_finished()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            size_t current_count = reader_.item_count();
            if (current_count != last_seen_count) {
                last_seen_count = current_count;
                screen.PostEvent(Event::Custom);
            }
        }
    });

    running_ = true;
    screen.Loop(renderer);

    // Stop update thread
    update_thread_running.store(false);
    if (update_thread.joinable()) {
        update_thread.join();
    }

    // Collect selected items
    std::vector<std::string> result;

    if (accepted_) {
        if (opts_.multi && !selected_.empty()) {
            // Return all selected items (Tab-selected)
            auto items = reader_.get_items();
            for (size_t idx : selected_) {
                if (idx < items.size()) {
                    std::string text = items[idx]->text();
                    // Always strip ANSI codes from output
                    // The --ansi flag controls parsing for display, not output
                    text = strip_ansi_codes(text);
                    result.push_back(text);
                }
            }
        } else {
            // Return current cursor item (single-select or multi without Tab selections)
            std::lock_guard<std::mutex> lock(results_mutex_);
            if (!current_results_.empty() && cursor_pos_ < current_results_.size()) {
                std::string text = current_results_[cursor_pos_].item->text();
                // Always strip ANSI codes from output
                // The --ansi flag controls parsing for display, not output
                text = strip_ansi_codes(text);
                result.push_back(text);
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

    // Return matched items
    std::vector<std::string> output;
    for (const auto& result : results) {
        output.push_back(result.item->text());
    }

    return output;
}

} // namespace fzf
