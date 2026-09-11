#pragma once

// Interactive terminal: UI state, the event loop and the action executor
// (fzf: src/terminal.go Terminal / Loop / doAction). The rendering is the
// direct-terminal frame renderer of render.hpp; the preview command runs
// in preview.hpp's worker. See docs/DESIGN.md section 10.
//
// Threading: every field is owned by the main thread unless noted. The
// reader and searcher threads only ever wake the loop through the self-pipe;
// the preview worker publishes its output under its own lock.

#include "chunklist.hpp"
#include "executor.hpp"
#include "keyevent.hpp"
#include "keymap.hpp"
#include "options.hpp"
#include "placeholder.hpp"
#include "preview.hpp"
#include "reader.hpp"
#include "search.hpp"
#include "tty.hpp"

#include <chrono>
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fzf {

// What the session ended with (fzf: Terminal.output / exit codes).
struct RunResult {
    int exit_code = 2;                // 0 ok, 1 no match, 2 error, 126 become failed, 130 abort
    std::vector<std::string> lines;   // everything to print, in order, one per printsep
};

// Rectangles of the current frame (0-based rows/cols), used by the painter
// and by mouse hit-testing.
struct Layout {
    int rows = 0, cols = 0;
    int margin = 0;                 // 1 with --border
    int content_col = 0, content_cols = 0;
    int info_row = -1;
    int header_row = -1, header_rows = 0;
    int list_top = 0, list_rows = 0, list_col = 0, list_width = 0;
    int prompt_row = 0;
    bool preview = false;
    int preview_top = 0, preview_left = 0, preview_lines = 0, preview_cols = 0;
    int vsep_col = -1;              // vertical separator column (left/right preview)
    int hsep_row = -1;              // horizontal separator row (up/down preview)
};

class Terminal {
public:
    // `output_fd` is the original stdout (become() execs onto it), `tty_in`
    // / `tty_out` the terminal fds the UI uses.
    Terminal(Options& opts, ItemBuilder& builder, Reader& reader, int tty_in, int tty_out, int output_fd);
    ~Terminal();

    RunResult run();

private:

    // --- Selection (fzf: selectedItem keyed by item index, ordered by time) ---
    struct Selected {
        uint64_t seq;
        ItemRef ref;
    };

    // --- Query buffer (fzf: t.input / t.cx / t.yanked) ---
    std::u32string input_;
    size_t cx_ = 0;
    std::u32string yanked_;
    std::string query_utf8() const;
    void set_input(const std::u32string& s);
    bool del_char();                       // fzf: delChar
    void rubout(bool word, bool sub);      // fzf: rubout with wordRubout / subWordRubout
    size_t find_last_boundary(bool sub) const;    // fzf: findLastMatch(...) + 1
    size_t find_next_boundary(bool sub) const;    // cx + findFirstMatch(...) + 1

    // --- Cursor / list (fzf: t.cy, t.offset, vmove, vset, constrain) ---
    std::shared_ptr<Merger> merger_;
    int cy_ = 0;
    int offset_ = 0;
    int max_items() const { return layout_.list_rows; }
    bool vmove(int o, bool allow_cycle);
    bool vset(int o);
    void constrain();
    ItemRef current_item() const;
    int32_t current_index() const;         // input ordinal or kMinItemIndex
    int list_count() const { return static_cast<int>(merger_->size()); }

    // --- Selection ---
    std::map<uint32_t, Selected> selected_;
    uint64_t select_seq_ = 0;
    uint64_t version_ = 0;                 // fzf: t.version (selection / preview target changes)
    int multi_ = 0;
    bool select_item(const ItemRef& item);
    bool select_item_changed(const ItemRef& item);
    void deselect_item(const ItemRef& item);
    bool deselect_item_changed(const ItemRef& item);
    bool toggle_item(const ItemRef& item);
    std::vector<Selected> sorted_selected() const;

    // --- Events / actions ---
    Keymap keymap_;
    Keymap keymap_org_;
    std::map<Event, std::string> expect_;
    bool has_load_actions_ = false;
    bool has_result_actions_ = false;
    bool has_focus_actions_ = false;
    bool has_resize_actions_ = false;
    ActionType last_action_ = ActionType::Start;
    std::string last_key_;
    std::chrono::steady_clock::time_point last_activity_;
    Event current_event_;                  // the event whose actions run (Char, Mouse)
    KeyEvent current_key_;                 // raw key event (mouse payload)

    // Per-batch flags (fzf: the locals of one Loop iteration).
    bool changed_ = false;                 // re-run the search
    bool query_changed_ = false;
    bool beof_ = false;
    bool looping_ = true;
    std::string new_command_;              // reload(...) to start after the batch
    bool has_new_command_ = false;
    bool reload_sync_ = false;
    std::vector<std::string> new_command_temps_;
    bool pending_change_ = false;          // fire `change` once per input batch
    int32_t last_focus_ = kMinItemIndex;
    std::set<Event> triggering_;           // trigger(...) recursion guard
    int click_header_line_ = 0;
    int click_header_column_ = 0;

    bool do_actions(const ActionList& actions);
    bool do_action(const Action& a);
    void process_event(const Event& ev, const KeyEvent& key);
    void fire_event(EventType type);       // run the actions bound to a non-key event
    bool handle_mouse();

    // --- Termination (fzf: reqClose / reqPrintQuery / reqQuit / reqBecome) ---
    enum class Exit { None, Close, PrintQuery, Quit, Become, Fatal };
    Exit exit_ = Exit::None;
    std::string pressed_;                  // matched --expect key
    std::vector<std::string> print_queue_; // print(...) actions
    void request_exit(Exit e);
    RunResult finish();

    // --- Commands (fzf: executeCommand, buildPlusList, environ) ---
    Executor executor_;
    struct PlusList {
        bool valid = false;
        std::vector<PlaceholderItem> current, selected, matched;
        bool has_current = false, has_selected = false, has_matched = false;
    };
    // `as_current` (optional) stands in for the cursor item (prefetch).
    PlusList build_plus_list(const std::string& tmpl, bool force_plus, const ItemRef* as_current = nullptr);
    Expansion expand(const std::string& tmpl, bool force_plus, const PlusList& list);
    std::vector<std::string> environ(bool for_preview);
    std::string execute_command(const std::string& tmpl, bool force_plus, bool background,
                                bool capture, bool first_line_only);
    void become(const std::string& tmpl);
    void pause_ui();
    void resume_ui();

    // --- Reader / searcher coordination ---
    Options& opts_;
    ItemBuilder& builder_;
    Reader& reader_;
    Searcher searcher_;
    std::shared_ptr<ChunkList> display_list_;   // the list being matched (old one during reload-sync)
    bool reading_ = true;
    bool trigger_load_ = false;
    bool paused_ = false;                  // fzf: t.paused (--disabled / disable-search)
    bool sort_ = true;
    bool inputless_ = false;
    std::string failed_command_;           // fzf: t.failed
    size_t last_item_count_ = SIZE_MAX;
    bool last_finished_ = false;
    int read_ticks_ = 0;
    bool request_pending_ = false;
    std::chrono::steady_clock::time_point last_request_time_;
    std::chrono::steady_clock::time_point request_deadline_;
    uint64_t list_generation_ = 0;
    bool merger_pending_ = false;          // a request was posted, result not seen yet
    void start_reload(const std::string& command, bool sync);
    void on_reader_progress();
    void submit_search();
    void submit_search_sync();
    void install_merger(std::shared_ptr<Merger> merger);
    void flush_pending_request();
    std::string search_query() const;

    // --- Prompt / header / preview options (live copies) ---
    std::string prompt_string_;
    std::vector<std::string> header0_;     // --header lines (change-header)
    std::vector<std::string> input_header_;// --header-lines records
    bool header_visible_ = true;
    PreviewOpts preview_opts_;             // fzf: t.previewOpts (initial + change-preview-window)
    PreviewOpts initial_preview_opts_;
    bool preview_hidden_ = false;          // fzf: activePreviewOpts.hidden
    std::string border_label_, preview_label_, ghost_, pointer_;

    // --- Preview ---
    PreviewWorker preview_;
    int preview_offset_ = 0;               // first visible line
    size_t preview_total_lines_ = 0;       // of the last painted content
    std::string last_painted_preview_;
    int last_painted_offset_ = -1;
    bool last_painted_valid_ = false;
    int32_t focused_index_ = kMinItemIndex;
    uint64_t preview_version_ = ~uint64_t{0};
    std::string preview_request_key_;      // cache key of the request in flight
    uint64_t preview_request_version_ = 0;
    // FZFPP_PREVIEW_PREFETCH: after the loop has been idle for a moment,
    // the commands of the items around the cursor run at low priority and
    // fill the cache (docs/DESIGN.md section 10).
    int prefetch_n_ = 0;
    bool prefetch_pending_ = false;
    std::chrono::steady_clock::time_point prefetch_after_;
    static constexpr int kPrefetchIdleMs = 250;
    void arm_prefetch(bool kill_running);
    void schedule_prefetch();
    std::unordered_map<std::string, std::string> preview_cache_;
    std::list<std::string> preview_lru_;
    static constexpr size_t kPreviewCacheMax = 50;
    bool has_preview_window() const { return !preview_opts_.command.empty() && !preview_hidden_; }
    bool can_preview() const { return !preview_opts_.command.empty(); }
    void refresh_preview(const std::string& command, bool bypass_cache);
    void cancel_preview();
    void scroll_preview_to(int offset);
    void scroll_preview_by(int amount);
    int evaluate_scroll_offset();
    void cache_preview(const std::string& key, const std::string& content);
    bool cached_preview(const std::string& key, std::string& out);
    void clear_preview_cache();

    // --- Rendering ---
    Layout layout_;
    bool needs_repaint_ = true;
    bool preview_dirty_ = true;
    bool full_redraw_ = false;
    int term_rows_ = 24, term_cols_ = 80;
    void update_terminal_size();
    void compute_layout();
    void repaint();
    void paint_preview(bool force);
    std::vector<std::string> header_rows() const;
    void render_info_text(std::string& out) const;

    // --- tty plumbing ---
    int tty_in_;
    int tty_out_;
    int output_fd_;
    std::unique_ptr<RawMode> raw_;
    bool screen_active_ = false;
    void enter_screen();
    void leave_screen();
    int wake_read_fd_ = -1;
    int wake_write_fd_ = -1;
    int winch_read_fd_ = -1;
    int winch_write_fd_ = -1;

    // Mouse (double-click detection)
    std::chrono::steady_clock::time_point last_click_time_;
    int last_click_x_ = -1;
    int last_click_y_ = -1;
    bool mouse_enabled_ = true;
};

} // namespace fzf
