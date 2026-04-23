#pragma once

#include "item.hpp"
#include "matcher.hpp"
#include "reader.hpp"
#include "options.hpp"
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

namespace ftxui {
    class Event;
}

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
    // Perform search in background
    void perform_search();

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
    bool handle_mouse_event(ftxui::Event event);

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

    // Expect key matching
    bool check_expect_key(const ftxui::Event& event, std::string& matched_key);

    // Preview support
    std::string substitute_placeholders(const std::string& cmd, size_t index);

    // Terminal size and layout calculation
    void get_terminal_size(int& rows, int& cols) const;
    void calculate_preview_position(int& top, int& left, int& lines, int& cols) const;
    void set_preview_env_vars() const;  // Set FZF_PREVIEW_* environment variables

    const Options& opts_;
    Reader& reader_;
    Matcher matcher_;

    // UI state
    std::string current_query_;
    std::vector<MatchResult> current_results_;
    size_t cursor_pos_;           // Current cursor position
    size_t scroll_offset_;        // Scroll offset for results
    std::set<size_t> selected_;   // Selected item indices
    bool running_;
    bool accepted_;               // True if user accepted, false if aborted
    std::string matched_expect_key_;  // Stores matched expect key

    // Threading
    mutable std::mutex results_mutex_;
    std::atomic<bool> search_pending_;
    std::atomic<bool> search_running_;
    std::thread search_thread_;

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
};

} // namespace fzf
