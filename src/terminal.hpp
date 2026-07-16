#pragma once

#include "item.hpp"
#include "matcher.hpp"
#include "reader.hpp"
#include "options.hpp"
#include "keyevent.hpp"
#include <string>
#include <vector>
#include <memory>
#include <set>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <list>
#include <chrono>

namespace fzf {

// Terminal UI controller
class Terminal {
public:
    explicit Terminal(const Options& opts, Reader& reader);
    ~Terminal();

    // Run the interactive loop
    // Returns selected items (empty if aborted)
    std::vector<std::string> run();

    // Run filter mode (non-interactive)
    std::vector<std::string> run_filter(const std::string& query);

    // Get matched expect key (empty if none matched)
    std::string get_matched_expect_key() const { return matched_expect_key_; }

private:
    // Update search results
    void update_results(const std::string& query);

    // Get visible results (for current scroll position)
    std::vector<MatchResult> get_visible_results() const;

    // Navigation
    void move_cursor_up();
    void move_cursor_down();
    void move_cursor_page_up();
    void move_cursor_page_down();

    // Mouse handling
    bool handle_mouse_event(const KeyEvent& event);

    // Selection
    void toggle_selection();
    void select_all();
    void deselect_all();
    void toggle_all();
    void accept_selection();

    // Check if item is selected
    bool is_selected(size_t item_index) const;

    // Bind action execution
    bool execute_bind_action(const std::string& action);

    // Given an action like "reload(cmd)" and a prefix "reload(", return the
    // argument inside the balanced parentheses and set out_end to the index of
    // the matching ')'. Returns false if the parens are unbalanced. Handles
    // nested parens so a command may itself contain '(' and ')'.
    static bool extract_paren_arg(const std::string& action, size_t open_paren_pos,
                                  std::string& out_arg, size_t& out_end);

    // Expect key matching
    bool check_expect_key(const KeyEvent& event, std::string& matched_key);

    // Convert an arbitrary key event to fzf's bind key-name syntax (e.g.
    // "ctrl-r", "ctrl-/", "ctrl-space", "alt-a"), for looking up --bind targets
    // that aren't one of the specially-handled navigation keys. Returns empty
    // string if the event doesn't map to a name fzf recognizes as a bind key.
    static std::string event_to_bind_key(const KeyEvent& event);

    // Preview support
    std::string substitute_placeholders(const std::string& cmd, size_t index);

    // Run a shell command synchronously and return its stdout with a single
    // trailing newline trimmed (matching fzf's convention for transform-*
    // actions). Returns empty string if the command can't be started.
    static std::string run_command_capture_output(const std::string& cmd);

    // Terminal size and layout calculation
    void get_terminal_size(int& rows, int& cols) const;
    void calculate_preview_position(int& top, int& left, int& lines, int& cols) const;
    void set_preview_env_vars() const;  // Set FZF_PREVIEW_* environment variables

    // --- Rendering (direct-terminal backend, replaces FTXUI) ---

    // Recompute visible_lines_ from the current terminal size / opts_.height.
    void recompute_visible_lines();

    // Redraw the chrome (info/header/results/prompt/border) unconditionally,
    // and the preview pane only if preview_dirty is true. Chrome and preview
    // are written as two separate buffered writes so a partial-write
    // interruption can't leave the cursor in the wrong saved slot for the
    // other one — see render.hpp's write_raw_passthrough.
    void repaint(bool preview_dirty);

    // Query-buffer editing (replaces FTXUI's Input component). Operates on
    // current_query_ and query_cursor_ (a codepoint index, not a byte index).
    void query_insert_codepoints(const std::u32string& codepoints);
    void query_backspace();
    void query_delete();
    void query_move_left();
    void query_move_right();

    // Dispatch one decoded KeyEvent: expect-key check, bind lookup, default
    // navigation, or query-buffer editing. Returns true if the main loop
    // should keep running, false if it should exit (accept/abort/expect-key
    // match/double-click).
    bool dispatch_event(const KeyEvent& event);

    const Options& opts_;
    Reader& reader_;
    Matcher matcher_;

    // UI state
    std::string current_query_;
    std::u32string query_codepoints_;  // current_query_ decoded, for cursor math
    size_t query_cursor_;              // codepoint index into query_codepoints_
    std::string current_prompt_;  // Live prompt; starts at opts_.prompt, changed by change-prompt
    std::string current_header_;  // Live header; starts at opts_.header, changed by transform-header
    std::vector<MatchResult> current_results_;
    size_t cursor_pos_;           // Current cursor position
    size_t scroll_offset_;        // Scroll offset for results
    std::set<size_t> selected_;   // Selected item indices
    bool running_;
    bool accepted_;               // True if user accepted, false if aborted
    std::string matched_expect_key_;  // Stores matched expect key

    // Threading
    mutable std::mutex results_mutex_;

    // Display parameters
    size_t visible_lines_;        // Number of visible result lines
    bool wrap_lines_;             // Line wrapping state (for toggle-wrap)
    bool preview_visible_;        // Preview visibility state (for toggle-preview)

    // Mouse state (for double-click detection)
    std::chrono::steady_clock::time_point last_click_time_;
    int last_click_x_;
    int last_click_y_;

    // Preview state
    std::string preview_content_;
    mutable std::mutex preview_mutex_;
    size_t last_preview_cursor_;  // Track last cursor position for preview updates
    size_t preview_scroll_offset_;  // Current scroll position in preview (line number at top)
    size_t preview_total_lines_;    // Total lines in current preview content

    // Async preview rendering
    std::thread preview_thread_;
    std::atomic<bool> preview_pending_;
    std::atomic<bool> preview_cancel_;
    std::atomic<size_t> preview_target_cursor_;
    std::string preview_target_item_;  // Item text for the target preview (protected by preview_mutex_)

    // Preview cache (LRU)
    std::unordered_map<std::string, std::string> preview_cache_;  // item_text -> preview_content
    std::list<std::string> preview_lru_;  // LRU list: front = most recent
    static constexpr size_t PREVIEW_CACHE_MAX_SIZE = 50;  // Cache up to 50 previews
    mutable std::mutex cache_mutex_;

    // Background prefetch queue
    std::list<std::string> prefetch_queue_;  // Items to prefetch in background
    mutable std::mutex prefetch_mutex_;

    void preview_worker();  // Background preview rendering thread
    std::string get_cached_preview(const std::string& item_text);  // Check cache
    void cache_preview(const std::string& item_text, const std::string& content);  // Store in cache
    void populate_prefetch_queue();  // Populate prefetch queue from current results

    // --- tty/poll-loop plumbing (replaces FTXUI ScreenInteractive) ---
    int wake_read_fd_;
    int wake_write_fd_;
    int winch_read_fd_;
    int winch_write_fd_;
};

} // namespace fzf
