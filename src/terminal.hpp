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
#include <cstdint>
#include <sys/types.h>

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

    // True if the interactive session ended by abort (esc/ctrl-c/ctrl-g) rather
    // than accept. main uses this for fzf's exit-code contract (130 on abort).
    bool was_aborted() const { return !accepted_; }

    // The query as it stood when the session ended — what --print-query must
    // print (fzf prints the live query, not the initial --query value).
    const std::string& final_query() const { return current_query_; }

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

    // Item-anchored variant used by the preview worker: the item is captured
    // once by the caller, so a concurrent results update can't swap which
    // item's text gets substituted mid-command. display_index only feeds
    // fzf's {n} placeholder fallback when item is null.
    std::string substitute_placeholders_for_item(const std::string& cmd,
                                                 const std::shared_ptr<Item>& item);

    // Run a shell command synchronously and return its stdout with a single
    // trailing newline trimmed (matching fzf's convention for transform-*
    // actions). Returns empty string if the command can't be started.
    static std::string run_command_capture_output(const std::string& cmd);

    // Terminal size and layout calculation
    void get_terminal_size(int& rows, int& cols) const;
    void calculate_preview_position(int& top, int& left, int& lines, int& cols) const;
    // Single source of truth for the results/separator/preview column split.
    // Both repaint() and calculate_preview_position() must derive from this
    // so the border and the preview pane never disagree on where the split
    // falls (a prior divergence let the preview pane overshoot the border
    // and painted stale columns nobody cleared -- see render.cpp history).
    void calculate_column_layout(int content_cols, int& results_width,
                                  int& preview_cols, int& sep_col) const;

    // FZF_PREVIEW_* variables for a preview child's environment. Returned as
    // NAME=value strings for shell_popen's execve instead of setenv'd: the
    // worker mutating environ while the main thread reads it is UB.
    std::vector<std::string> preview_env_vars() const;

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

    // UI state. current_query_ and current_header_ are mutated only on the
    // main thread, but the preview worker reads them ({q} substitution,
    // header-row layout math) -- every write and every cross-thread read
    // must hold preview_mutex_ (see set_current_query / set_current_header).
    std::string current_query_;
    std::u32string query_codepoints_;  // current_query_ decoded, for cursor math
    size_t query_cursor_;              // codepoint index into query_codepoints_
    std::string current_prompt_;  // Live prompt; starts at opts_.prompt, changed by change-prompt
    std::string current_header_;  // Live header; starts at opts_.header, changed by transform-header

    // Rebuild current_query_ from query_codepoints_ under preview_mutex_.
    void sync_query_from_codepoints();
    void set_current_header(const std::string& header);

    // Post a new preview target: bumps the generation (stopping any
    // in-flight render's publish/cache) and signals the streaming child.
    void supersede_preview();

    // If the cursor moved to a new item, serve its preview from cache or
    // post a render request to the worker. Returns true if the pane needs
    // repainting. Called once before the event loop (a small finished input
    // never wakes the loop) and after every dispatched batch.
    bool maybe_request_preview();
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

    // Guard against redundant preview repaints. The streaming preview worker
    // wakes a repaint after EVERY chunk it reads; without this, a preview that
    // contains a graphics blob (sixel / iTerm2 image / kitty) would re-emit the
    // whole blob to the terminal on every chunk and on every unrelated frame,
    // flooding a sixel terminal (mlterm) with repeated image data that reads as
    // streaming garbage. We only re-write the preview pane when the content or
    // scroll position actually changed since the last paint.
    std::string last_painted_preview_;
    size_t last_painted_scroll_ = SIZE_MAX;
    bool last_painted_valid_ = false;

    // Async preview rendering.
    //
    // Cancellation protocol: preview_generation_ increments every time the
    // main thread posts a new target (and once at shutdown). The worker
    // captures the generation together with the target; while streaming it
    // compares against the live value and stops when superseded. The old
    // scheme was a bool the main thread set and immediately cleared, which
    // the worker essentially never observed -- superseded previews ran to
    // completion and their output was cached under the *new* target's key
    // (wrong-item cache poisoning). The worker must also capture
    // preview_target_item_ at request-take time, never at completion time.
    //
    // preview_child_pid_ holds the process group of the currently-streaming
    // preview command so the main thread can kill it on supersede/teardown
    // instead of waiting for it to finish (or hang -- `--preview 'tail -f'`
    // used to wedge the worker and then the whole process at join()).
    std::thread preview_thread_;
    std::atomic<bool> preview_pending_;
    std::atomic<bool> preview_shutdown_;
    std::atomic<uint64_t> preview_generation_;
    std::atomic<pid_t> preview_child_pid_;
    std::atomic<size_t> preview_target_cursor_;
    std::string preview_target_item_;  // Item text for the target preview (protected by preview_mutex_)
    std::shared_ptr<Item> preview_target_item_ptr_;  // The Item itself (protected by preview_mutex_)

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
