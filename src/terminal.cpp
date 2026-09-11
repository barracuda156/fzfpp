#include "terminal.hpp"

#include "ansi.hpp"
#include "keyparser.hpp"
#include "render.hpp"
#include "shellcmd.hpp"
#include "theme.hpp"
#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/select.h>
#include <unistd.h>
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

std::string to_utf8(const std::u32string& s) {
    std::string out;
    utf8::unchecked::utf32to8(s.begin(), s.end(), std::back_inserter(out));
    return out;
}

std::u32string to_utf32(const std::string& s) {
    std::u32string out;
    try {
        utf8::utf8to32(s.begin(), s.end(), std::back_inserter(out));
    } catch (...) {
        out.clear();
        utf8::unchecked::utf8to32(s.begin(), s.end(), std::back_inserter(out));
    }
    return out;
}

// fzf: keyMatch
bool key_match(const Event& key, const Event& event) {
    return key.type == event.type && key.ch == event.ch;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Terminal::Terminal(Options& opts, ItemBuilder& builder, Reader& reader, int tty_in, int tty_out,
                   int output_fd)
    : merger_(std::make_shared<Merger>()),
      executor_(Executor::create(opts.with_shell)),
      opts_(opts),
      builder_(builder),
      reader_(reader),
      searcher_(opts, /*interactive=*/true),
      preview_(executor_, [this]() { if (wake_write_fd_ >= 0) wake_pipe(wake_write_fd_); }),
      tty_in_(tty_in),
      tty_out_(tty_out),
      output_fd_(output_fd) {
    keymap_ = build_keymap(opts);
    keymap_org_ = keymap_;
    expect_ = build_expect(opts);
    has_load_actions_ = keymap_.count(event_of(EventType::Load)) > 0;
    has_result_actions_ = keymap_.count(event_of(EventType::Result)) > 0 ||
                          keymap_.count(event_of(EventType::ResultFinal)) > 0;
    has_focus_actions_ = keymap_.count(event_of(EventType::Focus)) > 0;
    has_resize_actions_ = keymap_.count(event_of(EventType::Resize)) > 0;

    multi_ = opts.multi;
    sort_ = opts.sort > 0;
    paused_ = opts.phony;
    inputless_ = opts.inputless;
    mouse_enabled_ = opts.mouse;
    prompt_string_ = opts.prompt;
    header0_ = opts.header;
    preview_opts_ = opts.preview;
    initial_preview_opts_ = opts.preview;
    preview_hidden_ = opts.preview.hidden;
    border_label_ = opts.border_label.label;
    preview_label_ = opts.preview_label.label;
    ghost_ = opts.ghost;
    pointer_ = opts.pointer.value_or(">");
    prefetch_n_ = opts.preview_prefetch;
    scheme_ = materialize_theme(opts);
    // fzf: NewTerminal -- glyph defaults follow --unicode
    pointer_len_ = static_cast<int>(visible_width(pointer_));
    marker_len_ = static_cast<int>(visible_width(opts.marker.value_or(">")));
    pointer_empty_ = std::string(static_cast<size_t>(pointer_len_), ' ');
    marker_empty_ = std::string(static_cast<size_t>(marker_len_), ' ');
    if (opts.separator) separator_ = *opts.separator;
    else separator_ = opts.unicode ? "\xE2\x94\x80" : "-";
    if (opts.scrollbar) {
        scrollbar_ = *opts.scrollbar;
        // Only the first character is used for the list (the second is the
        // preview scrollbar).
        if (!scrollbar_.empty()) {
            size_t n = utf8_char_length(scrollbar_[0]);
            scrollbar_ = scrollbar_.substr(0, n);
        }
    } else {
        scrollbar_ = opts.unicode ? "\xE2\x94\x82" : "|";
    }
    input_ = to_utf32(opts.query);
    cx_ = input_.size();
    display_list_ = reader.list();
    last_activity_ = std::chrono::steady_clock::now();
    last_click_time_ = last_activity_;
}

Terminal::~Terminal() {
    preview_.shutdown();
}

std::string Terminal::query_utf8() const { return to_utf8(input_); }

void Terminal::set_input(const std::u32string& s) {
    input_ = s;
    cx_ = input_.size();
}

// ---------------------------------------------------------------------------
// Screen plumbing
// ---------------------------------------------------------------------------

void Terminal::enter_screen() {
    if (screen_active_) return;
    raw_ = std::make_unique<RawMode>(tty_in_);
    enter_alt_screen(tty_out_);
    hide_cursor(tty_out_);
    if (mouse_enabled_) enable_mouse(tty_out_);
    screen_active_ = true;
}

void Terminal::leave_screen() {
    if (!screen_active_) return;
    if (mouse_enabled_) disable_mouse(tty_out_);
    show_cursor(tty_out_);
    leave_alt_screen(tty_out_);
    raw_.reset();
    screen_active_ = false;
}

// fzf: tui.Pause / tui.Resume around a foreground execute()
void Terminal::pause_ui() { leave_screen(); }

void Terminal::resume_ui() {
    enter_screen();
    full_redraw_ = true;
    last_painted_valid_ = false;
    needs_repaint_ = true;
    preview_dirty_ = true;
}

void Terminal::update_terminal_size() {
    if (!get_terminal_size(tty_out_, term_rows_, term_cols_)) {
        term_rows_ = 24;
        term_cols_ = 80;
    }
}

// ---------------------------------------------------------------------------
// Reader / searcher coordination
// ---------------------------------------------------------------------------

std::string Terminal::search_query() const {
    if (paused_) return std::string();
    return query_utf8();
}

void Terminal::submit_search() {
    searcher_.request(display_list_->snapshot(), search_query(), display_list_->finished(), sort_);
    merger_pending_ = true;
    last_request_time_ = std::chrono::steady_clock::now();
    request_pending_ = false;
}

void Terminal::submit_search_sync() {
    install_merger(searcher_.scan_sync(display_list_->snapshot(), search_query(),
                                       display_list_->finished(), sort_));
    merger_pending_ = false;
    last_request_time_ = std::chrono::steady_clock::now();
    request_pending_ = false;
}

void Terminal::flush_pending_request() {
    if (request_pending_ && std::chrono::steady_clock::now() >= request_deadline_) {
        submit_search();
    }
}

// fzf: core.go EvtReadNew / EvtReadFin handling plus Terminal.UpdateCount.
void Terminal::on_reader_progress() {
    auto list = reader_.list();
    if (list != display_list_) {
        // reload-sync keeps the old list on display until the new one is
        // complete (fzf: useSnapshot).
        if (!list->finished()) return;
        display_list_ = list;
        selected_.clear();
        ++version_;
        cy_ = 0;
        offset_ = 0;
        last_item_count_ = SIZE_MAX;
        searcher_.clear_cache();
    }
    size_t count = display_list_->count();
    bool finished = display_list_->finished();
    if (count != last_item_count_ || finished != last_finished_) {
        last_item_count_ = count;
        last_finished_ = finished;
        if (opts_.header_lines > 0) input_header_ = reader_.header_lines();
        if (finished) {
            if (reading_) {
                reading_ = false;
                trigger_load_ = has_load_actions_;
                failed_command_ = reader_.failed_command();
            }
            submit_search();
        } else {
            // While the producer is streaming, re-matching is throttled with
            // a growing delay (fzf: coordinatorDelayStep up to
            // coordinatorDelayMax) so a fast pipe cannot starve the searcher.
            auto now = std::chrono::steady_clock::now();
            auto delay = std::chrono::milliseconds(std::min(100, 10 * read_ticks_));
            ++read_ticks_;
            if (now - last_request_time_ >= delay) {
                submit_search();
            } else if (!request_pending_) {
                request_pending_ = true;
                request_deadline_ = last_request_time_ + delay;
            }
        }
        needs_repaint_ = true;
    }
    flush_pending_request();
}

// fzf: Terminal.UpdateList (the parts that apply without --track)
void Terminal::install_merger(std::shared_ptr<Merger> merger) {
    if (!merger) return;
    merger_ = std::move(merger);
    merger_pending_ = false;
    constrain();
    needs_repaint_ = true;
    preview_dirty_ = true;
    arm_prefetch(false);

    if (trigger_load_) {
        trigger_load_ = false;
        fire_event(EventType::Load);
    }
    if (!reading_) {
        switch (merger_->size()) {
            case 0: fire_event(EventType::Zero); break;
            case 1: fire_event(EventType::One); break;
            default: break;
        }
    }
    if (has_result_actions_) {
        fire_event(EventType::Result);
        if (!reading_) fire_event(EventType::ResultFinal);
    }
}

void Terminal::fire_event(EventType type) {
    auto it = keymap_.find(event_of(type));
    if (it == keymap_.end() || exit_ != Exit::None) return;
    Event saved_event = current_event_;
    current_event_ = event_of(type);
    do_actions(it->second);
    current_event_ = saved_event;
}

// fzf: actReload at the end of the loop iteration (restart the reader)
void Terminal::start_reload(const std::string& command, bool sync) {
    ++list_generation_;
    std::shared_ptr<ChunkList> old_list = display_list_;
    reader_.start_command(command, environ(false), new_command_temps_);
    new_command_temps_.clear();
    reading_ = true;
    read_ticks_ = 0;
    last_item_count_ = SIZE_MAX;
    last_finished_ = false;
    if (!sync) {
        display_list_ = reader_.list();
        searcher_.clear_cache();
        selected_.clear();
        ++version_;
        cy_ = 0;
        offset_ = 0;
    }
    // Either way the current display is re-matched (the new, empty list or
    // the old one that stays on screen until the new one is complete).
    submit_search();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

namespace {

// fzf: RangesToString
std::string ranges_to_string(const std::vector<Range>& ranges) {
    std::string out;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const Range& r = ranges[i];
        std::string s;
        if (r.begin == kRangeEllipsis && r.end == kRangeEllipsis) {
            s = "..";
        } else if (r.begin == r.end) {
            s = std::to_string(r.begin);
        } else {
            if (r.begin != kRangeEllipsis) s += std::to_string(r.begin);
            if (r.begin != -1) {
                s += "..";
                if (r.end != kRangeEllipsis) s += std::to_string(r.end);
            }
        }
        if (i > 0) out += ',';
        out += s;
    }
    return out;
}

} // namespace

// fzf: Terminal.environImpl
std::vector<std::string> Terminal::environ(bool for_preview) {
    std::vector<std::string> env;
    auto add = [&](const char* name, const std::string& value) { env.push_back(std::string(name) + "=" + value); };
    add("FZF_QUERY", query_utf8());
    add("FZF_ACTION", action_kebab_name(last_action_));
    add("FZF_KEY", last_key_);
    auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_activity_).count();
    add("FZF_IDLE_TIME", std::to_string(idle / 1000));
    add("FZF_IDLE_TIME_MS", std::to_string(idle));
    add("FZF_PROMPT", prompt_string_);
    add("FZF_GHOST", ghost_);
    add("FZF_POINTER", pointer_);
    add("FZF_PREVIEW_LABEL", preview_label_);
    add("FZF_BORDER_LABEL", border_label_);
    add("FZF_LIST_LABEL", opts_.list_label.label);
    add("FZF_INPUT_LABEL", opts_.input_label.label);
    add("FZF_HEADER_LABEL", opts_.header_label.label);
    add("FZF_DIRECTION", opts_.layout == LayoutType::Default ? "up" : "down");
    if (!opts_.nth.empty()) add("FZF_NTH", ranges_to_string(opts_.nth));
    if (!opts_.with_nth_expr.empty()) add("FZF_WITH_NTH", opts_.with_nth_expr);
    const char* input_state = "enabled";
    if (inputless_) input_state = "hidden";
    else if (paused_) input_state = "disabled";
    add("FZF_INPUT_STATE", input_state);
    add("FZF_TOTAL_COUNT", std::to_string(display_list_ ? display_list_->count() : 0));
    add("FZF_MATCH_COUNT", std::to_string(merger_->size()));
    add("FZF_SELECT_COUNT", std::to_string(selected_.size()));
    add("FZF_LINES", std::to_string(layout_.area_lines));
    add("FZF_COLUMNS", std::to_string(layout_.area_columns));
    add("FZF_POS", std::to_string(std::min(list_count(), cy_ + 1)));
    if (ItemRef item = current_item()) {
        // Skip if the value contains a NUL byte (exec(2) would reject the
        // env) or is too large (fzf: maxCurrentItemEnvSize).
        std::string s = builder_.original_text(item);
        if (s.find('\0') == std::string::npos && s.size() <= 64 * 1024) add("FZF_CURRENT_ITEM", s);
    }
    add("FZF_CLICK_HEADER_LINE", std::to_string(click_header_line_));
    add("FZF_CLICK_HEADER_COLUMN", std::to_string(click_header_column_));
    add("FZF_CLICK_FOOTER_LINE", "0");
    add("FZF_CLICK_FOOTER_COLUMN", "0");
    if (layout_.preview && layout_.pwindow.height > 0) {
        std::string lines = std::to_string(layout_.pwindow.height);
        std::string columns = std::to_string(layout_.pwindow.width);
        if (for_preview) {
            add("LINES", lines);
            add("COLUMNS", columns);
        }
        add("FZF_PREVIEW_LINES", lines);
        add("FZF_PREVIEW_COLUMNS", columns);
        add("FZF_PREVIEW_TOP", std::to_string(layout_.pwindow.top));
        add("FZF_PREVIEW_LEFT", std::to_string(layout_.pwindow.left));
    }
    return env;
}

// fzf: Terminal.buildPlusList
Terminal::PlusList Terminal::build_plus_list(const std::string& tmpl, bool force_plus,
                                             const ItemRef* as_current) {
    PlusList list;
    ItemRef current = as_current ? *as_current : current_item();
    TemplateFlags flags = has_preview_flags(tmpl);
    auto item_of = [&](const ItemRef& ref) {
        return PlaceholderItem{builder_.original_text(ref), static_cast<int32_t>(ref.index())};
    };
    if (!(!flags.slot || flags.force_update || flags.asterisk ||
          ((force_plus || flags.plus) && !selected_.empty()))) {
        if (!current) return list;   // invalid
        list.valid = true;
        list.has_current = list.has_selected = true;
        list.current.push_back(item_of(current));
        list.selected.push_back(item_of(current));
        return list;
    }
    // We would still want to update the preview window even if there is no
    // match if the command template contains {q}, or {+} with selections.
    // An empty item stands in for fzf's minItem.
    list.valid = true;
    list.has_current = true;
    list.current.push_back(current ? item_of(current) : PlaceholderItem{"", kMinItemIndex});
    if (flags.asterisk) {
        list.has_matched = true;
        uint32_t n = merger_->size();
        list.matched.reserve(n);
        for (uint32_t i = 0; i < n; ++i) list.matched.push_back(item_of(merger_->get(i)));
    }
    list.has_selected = true;
    if (selected_.empty()) {
        list.selected.push_back(list.current.front());
    } else {
        for (const auto& sel : sorted_selected()) list.selected.push_back(item_of(sel.ref));
    }
    return list;
}

Expansion Terminal::expand(const std::string& tmpl, bool force_plus, const PlusList& list) {
    PlaceholderParams p;
    p.tmpl = tmpl;
    p.delimiter = &opts_.delimiter;
    p.printsep = opts_.print0 ? std::string("\0", 1) : std::string("\n");
    p.force_plus = force_plus;
    p.query = query_utf8();
    p.current = list.has_current ? &list.current : nullptr;
    p.selected = list.has_selected ? &list.selected : nullptr;
    p.matched = list.has_matched ? &list.matched : nullptr;
    p.last_action = action_kebab_name(last_action_);
    p.prompt = prompt_string_;
    p.executor = &executor_;
    return replace_placeholder(p);
}

// fzf: Terminal.executeCommand
std::string Terminal::execute_command(const std::string& tmpl, bool force_plus, bool background,
                                      bool capture, bool first_line_only) {
    std::string line;
    PlusList list = build_plus_list(tmpl, force_plus);
    // 'capture' is used for transform-* and we don't want to return an
    // empty string in those cases
    if (!list.valid && !capture) return line;
    Expansion exp = expand(tmpl, force_plus, list);
    std::vector<std::string> env = environ(false);
    if (!background) {
        pause_ui();
        shell_run_foreground(executor_, exp.command, env, tty_in_, tty_out_);
        resume_ui();
    } else if (capture) {
        line = shell_capture(executor_, exp.command, env, first_line_only);
    } else {
        shell_run_silent(executor_, exp.command, env);
    }
    remove_files(exp.temp_files);
    return line;
}

// fzf: actBecome + Executor.Become
void Terminal::become(const std::string& tmpl) {
    PlusList list = build_plus_list(tmpl, false);
    if (!list.valid) return;
    // We do not remove temp files in this case
    Expansion exp = expand(tmpl, false, list);
    std::vector<std::string> env = environ(false);
    leave_screen();
    preview_.shutdown();
    shell_become(executor_, exp.command, env, tty_in_, output_fd_);
    // Only reached when exec failed.
    std::fprintf(stderr, "fzf (become): %s: %s\n", executor_.shell.c_str(), std::strerror(errno));
    request_exit(Exit::Become);
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

void Terminal::cache_preview(const std::string& key, const std::string& content) {
    auto it = preview_cache_.find(key);
    if (it != preview_cache_.end()) {
        it->second = content;
        preview_lru_.remove(key);
        preview_lru_.push_front(key);
        return;
    }
    preview_cache_[key] = content;
    preview_lru_.push_front(key);
    if (preview_cache_.size() > kPreviewCacheMax) {
        preview_cache_.erase(preview_lru_.back());
        preview_lru_.pop_back();
    }
}

bool Terminal::cached_preview(const std::string& key, std::string& out) {
    auto it = preview_cache_.find(key);
    if (it == preview_cache_.end()) return false;
    out = it->second;
    preview_lru_.remove(key);
    preview_lru_.push_front(key);
    return true;
}

void Terminal::clear_preview_cache() {
    preview_cache_.clear();
    preview_lru_.clear();
}

// fzf: evaluateScrollOffset -- the +SCROLL[OFFSETS][/DENOM] expression of
// --preview-window, with {n}-style placeholders expanded on the current item.
int Terminal::evaluate_scroll_offset() {
    if (preview_opts_.scroll.empty() || !layout_.preview) return 0;
    PlusList list;
    list.valid = true;
    list.has_current = true;
    if (ItemRef cur = current_item()) {
        list.current.push_back(PlaceholderItem{builder_.original_text(cur), static_cast<int32_t>(cur.index())});
    } else {
        list.has_current = false;
    }
    Expansion exp = expand(preview_opts_.scroll, false, list);
    remove_files(exp.temp_files);
    std::string expr;
    for (char c : exp.command) {
        if ((c >= '0' && c <= '9') || c == '/' || c == '+' || c == '-') expr += c;
    }
    int base = -1;
    int height = std::max(0, layout_.pwindow.height - preview_opts_.header_lines);
    // Components: ([+-][0-9]+) | (-?/[1-9][0-9]*)
    size_t i = 0;
    while (i < expr.size()) {
        char c = expr[i];
        if ((c == '+' || c == '-') && i + 1 < expr.size() && expr[i + 1] >= '0' && expr[i + 1] <= '9') {
            size_t j = i + 1;
            while (j < expr.size() && expr[j] >= '0' && expr[j] <= '9') ++j;
            base += std::atoi(expr.substr(i, j - i).c_str());
            i = j;
            continue;
        }
        if ((c == '/' || (c == '-' && i + 1 < expr.size() && expr[i + 1] == '/'))) {
            size_t j = c == '/' ? i + 1 : i + 2;
            if (j < expr.size() && expr[j] >= '1' && expr[j] <= '9') {
                size_t k = j;
                while (k < expr.size() && expr[k] >= '0' && expr[k] <= '9') ++k;
                int denom = std::atoi(expr.substr(j, k - j).c_str());
                if (denom != 0) base -= height / denom;
                break;
            }
        }
        ++i;
    }
    return std::max(0, base);
}

// fzf: refreshPreview
void Terminal::refresh_preview(const std::string& command, bool bypass_cache) {
    if (command.empty() || !has_preview_window()) return;
    PlusList list = build_plus_list(command, false);
    preview_dirty_ = true;
    arm_prefetch(false);
    if (!list.valid) {
        // We don't display preview window if no match
        preview_.cancel();
        preview_.set_content(std::string(), true);
        preview_request_key_.clear();
        return;
    }
    Expansion exp = expand(command, false, list);
    preview_offset_ = evaluate_scroll_offset();
    std::string cached;
    if (!bypass_cache && cached_preview(exp.command, cached)) {
        remove_files(exp.temp_files);
        preview_.cancel();
        preview_.set_content(cached, true);
        preview_request_key_.clear();
        return;
    }
    preview_request_key_ = exp.command;
    preview_request_version_ = preview_.request(PreviewWorker::Request{exp.command, environ(true), exp.temp_files});
}

void Terminal::cancel_preview() {
    preview_.cancel();
    preview_request_key_.clear();
    prefetch_pending_ = false;
}

// Re-arm the idle timer that schedules the neighbours' previews; the
// queued ones are stale once the cursor, the list or the query moved.
void Terminal::arm_prefetch(bool kill_running) {
    if (prefetch_n_ <= 0) return;
    preview_.cancel_prefetch(kill_running);
    prefetch_pending_ = true;
    prefetch_after_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPrefetchIdleMs);
}

// Queue the preview commands of the N items below and above the cursor
// (nearest first, below before above) that are not cached yet.
void Terminal::schedule_prefetch() {
    prefetch_pending_ = false;
    if (prefetch_n_ <= 0 || !has_preview_window() || preview_opts_.command.empty()) return;
    const std::string& tmpl = preview_opts_.command;
    std::vector<PreviewWorker::Request> reqs;
    std::string cached;
    int count = list_count();
    for (int d = 1; d <= prefetch_n_; ++d) {
        for (int idx : {cy_ + d, cy_ - d}) {
            if (idx < 0 || idx >= count) continue;
            ItemRef item = merger_->get(static_cast<uint32_t>(idx));
            PlusList list = build_plus_list(tmpl, false, &item);
            if (!list.valid) continue;
            Expansion exp = expand(tmpl, false, list);
            if (!exp.temp_files.empty()) {
                // {f}: a fresh temp file per expansion, never a cache hit.
                remove_files(exp.temp_files);
                continue;
            }
            if (exp.command == preview_request_key_ || cached_preview(exp.command, cached)) continue;
            reqs.push_back(PreviewWorker::Request{exp.command, environ(true), {}});
        }
    }
    if (!reqs.empty()) preview_.prefetch(std::move(reqs));
}

// fzf: scrollPreviewTo
void Terminal::scroll_preview_to(int new_offset) {
    int num_lines = static_cast<int>(preview_total_lines_);
    if (num_lines <= 0) return;
    int header_lines = preview_opts_.header_lines;
    if (preview_opts_.cycle) {
        int range = num_lines - header_lines;
        if (range > 0) new_offset = ((new_offset - header_lines) % range + range) % range + header_lines;
    }
    new_offset = std::clamp(new_offset, std::min(header_lines, num_lines - 1), num_lines - 1);
    if (new_offset < 0) new_offset = 0;
    if (preview_offset_ != new_offset) {
        preview_offset_ = new_offset;
        preview_dirty_ = true;
    }
}

void Terminal::scroll_preview_by(int amount) { scroll_preview_to(preview_offset_ + amount); }

// ---------------------------------------------------------------------------
// Layout and rendering
// ---------------------------------------------------------------------------

// fzf: printHeaderImpl -- --header lines and --header-lines records in the
// order they appear on screen (top-down). In the default and reverse-list
// layouts the block is drawn bottom-up, so the records come first in
// reverse order, then the --header lines in their natural order.
std::vector<std::string> Terminal::header_rows() const {
    std::vector<std::string> rows;
    if (!header_visible_) return rows;
    if (opts_.layout == LayoutType::Reverse) {
        for (const auto& line : header0_) rows.push_back(strip_ansi_codes(line));
        for (const auto& line : input_header_) rows.push_back(strip_ansi_codes(line));
    } else {
        for (auto it = input_header_.rbegin(); it != input_header_.rend(); ++it) rows.push_back(strip_ansi_codes(*it));
        for (const auto& line : header0_) rows.push_back(strip_ansi_codes(line));
    }
    return rows;
}

// fzf: noSeparatorLine
bool Terminal::no_separator_line() const {
    if (inputless_) return true;
    bool separator = !separator_.empty();
    switch (opts_.info_style) {
        case InfoStyle::Inline: return true;
        case InfoStyle::Hidden: case InfoStyle::InlineRight: return !separator;
        default: return false;
    }
}

namespace {

// fzf: calculateSize
int calculate_size(int base, const SizeSpec& size, int occupied, int min_size) {
    int max = base - occupied;
    if (max < min_size) max = min_size;
    if (size.percent) return std::clamp(static_cast<int>(base * 0.01 * size.size), min_size, max);
    return std::clamp(static_cast<int>(size.size) + min_size - 1, min_size, max);
}

int border_lines(BorderShape s) { return (border_has_top(s) ? 1 : 0) + (border_has_bottom(s) ? 1 : 0); }
int border_columns(BorderShape s, int bw) {
    return (border_has_left(s) ? 1 + bw : 0) + (border_has_right(s) ? 1 + bw : 0);
}

// fzf: previewOpts.Border(layout) -- "line" resolves to the side facing
// the list.
BorderShape preview_border_shape(const PreviewOpts& p) {
    if (p.border != BorderShape::Line) return p.border == BorderShape::Undefined ? BorderShape::Rounded : p.border;
    switch (p.position) {
        case WindowPosition::Up: return BorderShape::Bottom;
        case WindowPosition::Down: return BorderShape::Top;
        case WindowPosition::Left: return BorderShape::Right;
        default: return BorderShape::Left;
    }
}

} // namespace

// fzf: adjustMarginAndPadding + resizeWindows, reduced to the windows this
// renderer has: the outer border, the list window and the preview.
void Terminal::compute_layout() {
    Layout l;
    l.rows = term_rows_;
    l.cols = term_cols_;
    // Until inline height lands (T2.3) the --height area is the top of the
    // alternate screen.
    if (opts_.height.is_set() && opts_.height.size > 0) {
        int h = opts_.height.percent ? (term_rows_ * static_cast<int>(opts_.height.size)) / 100
                                     : static_cast<int>(opts_.height.size);
        l.rows = std::clamp(h, 1, term_rows_);
    }
    const int screen_w = l.cols, screen_h = l.rows;
    const int bw = 1;
    BorderShape shape = border_visible(opts_.border_shape) ? opts_.border_shape : BorderShape::None;
    l.border_shape = shape;

    auto spec_to_int = [&](int idx, const SizeSpec& spec) {
        if (spec.percent) {
            double max = idx % 2 == 0 ? screen_h : screen_w;
            return static_cast<int>(max * spec.size * 0.01);
        }
        return static_cast<int>(spec.size);
    };
    int margin[4], padding[4], extra[4] = {0, 0, 0, 0};   // TRBL
    for (int i = 0; i < 4; ++i) padding[i] = spec_to_int(i, opts_.padding[i]);
    for (int i = 0; i < 4; ++i) {
        switch (shape) {
            case BorderShape::Horizontal: extra[i] += 1 - i % 2; break;
            case BorderShape::Vertical: extra[i] += (1 + bw) * (i % 2); break;
            case BorderShape::Top: if (i == 0) extra[i]++; break;
            case BorderShape::Right: if (i == 1) extra[i] += 1 + bw; break;
            case BorderShape::Bottom: if (i == 2) extra[i]++; break;
            case BorderShape::Left: if (i == 3) extra[i] += 1 + bw; break;
            case BorderShape::Rounded: case BorderShape::Sharp: case BorderShape::Bold: case BorderShape::Block:
            case BorderShape::ThinBlock: case BorderShape::Double: case BorderShape::Dashed:
                extra[i] += 1 + bw * (i % 2); break;
            default: break;
        }
        margin[i] = spec_to_int(i, opts_.margin[i]) + extra[i];
    }

    l.prompt_lines = inputless_ ? 0 : (no_separator_line() ? 1 : 2);
    l.preview = has_preview_window();
    BorderShape pshape = l.preview ? preview_border_shape(preview_opts_) : BorderShape::None;
    l.preview_shape = pshape;
    int min_preview_w = 1 + border_columns(pshape, bw);
    int min_preview_h = 1 + border_lines(pshape);
    bool preview_horizontal = l.preview && (preview_opts_.position == WindowPosition::Left ||
                                            preview_opts_.position == WindowPosition::Right);
    if (preview_horizontal && !scrollbar_.empty() && !border_has_right(pshape)) min_preview_w++;

    int min_area_w = 4, min_area_h = 3;
    if (inputless_) min_area_h--;
    if (no_separator_line()) min_area_h--;
    if (l.preview) {
        if (preview_horizontal) {
            min_area_w += min_preview_w;
            min_area_h = std::max(min_preview_h, min_area_h);
        } else {
            min_area_h += min_preview_h;
            min_area_w = std::max(min_preview_w, min_area_w);
        }
    }
    auto adjust = [&](int i1, int i2, int maximum, int minimum) {
        if (minimum > maximum) minimum = maximum;
        int m = margin[i1] + margin[i2] + padding[i1] + padding[i2];
        if (m > 0 && maximum - m < minimum) {
            int desired = maximum - minimum;
            padding[i1] = desired * padding[i1] / m;
            padding[i2] = desired * padding[i2] / m;
            margin[i1] = std::max(extra[i1], desired * margin[i1] / m);
            margin[i2] = std::max(extra[i2], desired * margin[i2] / m);
        }
    };
    adjust(1, 3, screen_w, min_area_w);
    adjust(0, 2, screen_h, min_area_h);

    int width = screen_w - margin[1] - margin[3];
    int height = screen_h - margin[0] - margin[2];
    if (shape != BorderShape::None) {
        l.border.top = margin[0] - (border_has_top(shape) ? 1 : 0);
        l.border.left = margin[3] - (border_has_left(shape) ? 1 + bw : 0);
        l.border.width = width + (border_has_left(shape) ? 1 + bw : 0) + (border_has_right(shape) ? 1 + bw : 0);
        l.border.height = height + (border_has_top(shape) ? 1 : 0) + (border_has_bottom(shape) ? 1 : 0);
    }
    for (int i = 0; i < 4; ++i) margin[i] += padding[i];
    width -= padding[1] + padding[3];
    height -= padding[0] + padding[2];
    width = std::max(width, 1);
    height = std::max(height, 1);
    l.area_lines = height;
    l.area_columns = width;

    int available = height - l.prompt_lines;
    Rect window{margin[0], margin[3], width, height};
    if (l.preview) {
        switch (preview_opts_.position) {
            case WindowPosition::Up: case WindowPosition::Down: {
                int min_window_h = min_area_h - (preview_horizontal ? 0 : min_preview_h);
                int ph = calculate_size(height, preview_opts_.size, min_window_h, min_preview_h);
                ph = std::clamp(ph, min_preview_h, std::max(min_preview_h, available - 1));
                if (preview_opts_.position == WindowPosition::Up) {
                    window = Rect{margin[0] + ph, margin[3], width, height - ph};
                    l.pborder = Rect{margin[0], margin[3], width, ph};
                } else {
                    window = Rect{margin[0], margin[3], width, height - ph};
                    l.pborder = Rect{margin[0] + height - ph, margin[3], width, ph};
                }
                break;
            }
            case WindowPosition::Left: {
                int pw = calculate_size(width, preview_opts_.size, 4, min_preview_w);
                const int m = 1;   // a 1-column margin between the preview and the list
                window = Rect{margin[0], margin[3] + pw + m, std::max(1, width - pw - m), height};
                l.pborder = Rect{margin[0], margin[3], pw, height};
                break;
            }
            default: {
                int pw = calculate_size(width, preview_opts_.size, 4, min_preview_w);
                window = Rect{margin[0], margin[3], std::max(1, width - pw), height};
                l.pborder = Rect{margin[0], margin[3] + width - pw, pw, height};
                break;
            }
        }
        Rect pw = l.pborder;
        if (border_has_left(pshape)) { pw.left += 1 + bw; }
        if (border_has_top(pshape)) { pw.top += 1; }
        pw.width -= border_columns(pshape, bw);
        pw.height -= border_lines(pshape);
        if (preview_horizontal && !scrollbar_.empty() && !border_has_right(pshape)) pw.width -= 1;
        pw.width = std::max(pw.width, 0);
        pw.height = std::max(pw.height, 0);
        l.pwindow = pw;
    }
    l.window = window;
    l.bar_col = (scrollbar_.empty() && !border_has_right(shape) &&
                 !(l.preview && preview_opts_.position == WindowPosition::Right)) ? 0 : 1;

    // Rows inside the list window (fzf: move() for each layout).
    l.header_lines = static_cast<int>(header_rows().size());
    int h = window.height;
    l.list_rows = std::max(0, h - l.prompt_lines - l.header_lines);
    int top = window.top;
    switch (opts_.layout) {
        case LayoutType::Reverse:
            if (opts_.header_first) {
                l.header_top = l.header_lines > 0 ? top : -1;
                l.prompt_row = l.prompt_lines > 0 ? top + l.header_lines : -1;
                l.info_row = l.prompt_lines > 1 ? l.prompt_row + 1 : -1;
            } else {
                l.prompt_row = l.prompt_lines > 0 ? top : -1;
                l.info_row = l.prompt_lines > 1 ? top + 1 : -1;
                l.header_top = l.header_lines > 0 ? top + l.prompt_lines : -1;
            }
            l.items_top = top + l.prompt_lines + l.header_lines;
            l.items_bottom_up = false;
            break;
        case LayoutType::ReverseList:
            l.items_top = top;
            l.items_bottom_up = false;
            if (opts_.header_first) {
                l.info_row = l.prompt_lines > 1 ? top + l.list_rows : -1;
                l.prompt_row = l.prompt_lines > 0 ? top + l.list_rows + (l.prompt_lines - 1) : -1;
                l.header_top = l.header_lines > 0 ? top + l.list_rows + l.prompt_lines : -1;
            } else {
                l.header_top = l.header_lines > 0 ? top + l.list_rows : -1;
                l.info_row = l.prompt_lines > 1 ? top + l.list_rows + l.header_lines : -1;
                l.prompt_row = l.prompt_lines > 0 ? top + h - 1 : -1;
            }
            break;
        default:
            l.items_top = top;
            l.items_bottom_up = true;
            if (opts_.header_first) {
                l.info_row = l.prompt_lines > 1 ? top + l.list_rows : -1;
                l.prompt_row = l.prompt_lines > 0 ? top + l.list_rows + (l.prompt_lines - 1) : -1;
                l.header_top = l.header_lines > 0 ? top + l.list_rows + l.prompt_lines : -1;
            } else {
                l.header_top = l.header_lines > 0 ? top + l.list_rows : -1;
                l.info_row = l.prompt_lines > 1 ? top + h - 2 : -1;
                l.prompt_row = l.prompt_lines > 0 ? top + h - 1 : -1;
            }
            break;
    }
    layout_ = l;
}

// fzf: printInfoImpl's text
std::string Terminal::info_text() const {
    int found = list_count();
    int total = std::max<int>(found, display_list_ ? static_cast<int>(display_list_->count()) : 0);
    std::string output = std::to_string(found) + "/" + std::to_string(total);
    if (multi_ > 0) {
        if (multi_ == 2147483647) output += " (" + std::to_string(selected_.size()) + ")";
        else output += " (" + std::to_string(selected_.size()) + "/" + std::to_string(multi_) + ")";
    }
    if (opts_.toggle_sort) output += sort_ ? " +S" : " -S";
    if (!failed_command_.empty() && total == 0) output = "[Command failed: " + failed_command_ + "]";
    return output;
}

void Terminal::place_cursor() {
    if (inputless_ || layout_.prompt_row < 0) return;
    std::string before_cursor = to_utf8(input_.substr(0, cx_));
    int x = static_cast<int>(visible_width(prompt_string_) + visible_width(before_cursor));
    x = std::min(x, layout_.window.width - 1);
    write_all(tty_out_, "\x1b[" + std::to_string(layout_.prompt_row + 1) + ";" +
                            std::to_string(layout_.window.left + x + 1) + "H");
}

void Terminal::repaint() {
    update_terminal_size();
    compute_layout();
    constrain();
    const Layout& l = layout_;
    const ColorScheme& t = scheme_;
    FrameRenderer frame(term_rows_, term_cols_);
    const bool unicode = opts_.unicode;

    if (full_redraw_) {
        frame.clear_region(0, 0, term_rows_, term_cols_);
        full_redraw_ = false;
    }

    // fzf: ColorPair(fg, bg): the pair's attributes are the fg's plus the bg's
    auto pair = [](const ColorAttr& fg, const ColorAttr& bg) {
        Style st = style_of(fg, bg);
        st.attr = ColorAttr::merge_attr(fg.attr, bg.attr) & ~kAttrRegular;
        return st;
    };
    const Style col_normal = pair(t.list_fg, t.list_bg);
    const Style col_match = pair(t.match, t.list_bg);
    const Style col_selected = pair(t.selected_fg, t.selected_bg);
    const Style col_selected_match = pair(t.selected_match, t.selected_bg);
    const Style col_current = pair(t.current, t.dark_bg);
    const Style col_current_match = pair(t.current_match, t.dark_bg);
    const Style col_pointer_empty = pair(ColorAttr{kColorDefault, 0}, t.gutter);
    const Style col_current_pointer = pair(t.pointer, t.dark_bg);
    const Style col_current_empty = pair(ColorAttr{kColorDefault, 0}, t.dark_bg);
    const Style col_marker = pair(t.marker, t.list_bg);
    const Style col_current_marker = pair(t.marker, t.dark_bg);
    const Style col_prompt = pair(t.prompt, t.list_bg);
    const Style col_input = pair(paused_ ? t.disabled : t.input, t.list_bg);
    const Style col_info = pair(t.info, t.list_bg);
    const Style col_separator = pair(t.separator, t.list_bg);
    const Style col_scrollbar = pair(t.scrollbar, t.list_bg);
    const Style col_header = pair(t.header, t.list_bg);
    const Style col_border = pair(t.border, t.bg);
    const Style col_border_label = pair(t.border_label, t.bg);
    const Style col_preview_border = pair(t.preview_border, t.bg);
    const Style col_preview_label = pair(t.preview_label, t.bg);

    // Labels sit on the top (or bottom) edge of a border; fzf: printLabel
    auto draw_label = [&](const Rect& r, BorderShape shape, const LabelOpts& opts, const std::string& text,
                          Style style) {
        if (text.empty() || r.width < 3) return;
        bool bottom = opts.bottom;
        if ((bottom && !border_has_bottom(shape)) || (!bottom && !border_has_top(shape))) return;
        int row = bottom ? r.bottom() - 1 : r.top;
        int width = static_cast<int>(visible_width(text));
        int usable = r.width - 2;
        std::string label = text;
        if (width > usable) {
            label = truncate_ansi_text(text, static_cast<size_t>(std::max(0, usable)));
            width = usable;
        }
        int col;
        if (opts.column == 0) col = r.left + (r.width - width) / 2;
        else if (opts.column > 0) col = r.left + opts.column;
        else col = r.right() + opts.column - width + 1;
        col = std::clamp(col, r.left + 1, std::max(r.left + 1, r.right() - 1 - width));
        frame.draw_text(row, col, label, style, width);
    };

    if (l.border_shape != BorderShape::None) {
        frame.draw_box(l.border.top, l.border.left, l.border.width, l.border.height, l.border_shape, unicode, col_border);
        draw_label(l.border, l.border_shape, opts_.border_label, border_label_, col_border_label);
    }
    if (l.preview) {
        frame.draw_box(l.pborder.top, l.pborder.left, l.pborder.width, l.pborder.height, l.preview_shape, unicode,
                       col_preview_border);
        draw_label(l.pborder, l.preview_shape, opts_.preview_label, preview_label_, col_preview_label);
    }

    const Rect& w = l.window;
    const int text_width = w.width - l.bar_col;   // columns for prompt/info/header/items

    // --- prompt line (fzf: printPrompt + inline info styles) ---
    if (l.prompt_row >= 0) {
        Row spans;
        spans.push_back(Span{prompt_string_, col_prompt});
        std::string query = query_utf8();
        if (query.empty() && !ghost_.empty()) {
            spans.push_back(Span{ghost_, pair(t.ghost, t.list_bg)});
        } else {
            spans.push_back(Span{query, col_input});
        }
        if (opts_.info_style == InfoStyle::Inline || opts_.info_style == InfoStyle::InlineRight) {
            std::string info = info_text();
            int used = static_cast<int>(visible_width(prompt_string_) + visible_width(query)) + 1;
            if (opts_.info_style == InfoStyle::Inline) {
                spans.push_back(Span{opts_.info_prefix, col_prompt});
                spans.push_back(Span{info, col_info});
            } else {
                int target = std::max(used, text_width - static_cast<int>(info.size()) - 3);
                spans.push_back(Span{std::string(static_cast<size_t>(std::max(0, target - used + 1)), ' '), col_normal});
                spans.push_back(Span{"  ", col_normal});
                spans.push_back(Span{info, col_info});
            }
        }
        frame.draw_row(l.prompt_row, w.left, spans, text_width);
        if (l.bar_col) frame.draw_text(l.prompt_row, w.right() - 1, " ", col_normal, 1);
    }
    // --- info / separator line (fzf: printInfoImpl) ---
    if (l.info_row >= 0) {
        Row spans;
        int used = 0;
        if (opts_.info_style == InfoStyle::Default) {
            std::string info = info_text();
            spans.push_back(Span{reading_ ? "\xE2\xA0\x8B" : " ", pair(t.spinner, t.list_bg)});   // fzf: spinner
            spans.push_back(Span{" ", col_normal});
            spans.push_back(Span{info, col_info});
            used = 2 + static_cast<int>(info.size());
            int fill = (text_width - used - 1) - 1;
            if (fill > 0 && !separator_.empty()) {
                spans.push_back(Span{" ", col_separator});
                std::string sep;
                for (int i = 0; i < fill; ++i) sep += separator_;
                spans.push_back(Span{sep, col_separator});
            }
        } else if (opts_.info_style == InfoStyle::Right) {
            std::string info = info_text();
            int fill = text_width - static_cast<int>(info.size()) - 2;
            if (fill >= 0 && !separator_.empty()) {
                std::string sep;
                for (int i = 0; i < fill; ++i) sep += separator_;
                spans.push_back(Span{sep, col_separator});
                spans.push_back(Span{" ", col_normal});
            } else if (fill > 0) {
                spans.push_back(Span{std::string(static_cast<size_t>(fill + 1), ' '), col_normal});
            }
            spans.push_back(Span{info, col_info});
        } else if (!separator_.empty()) {
            // hidden / inline-right: the separator line alone
            std::string sep;
            for (int i = 0; i < text_width - 1; ++i) sep += separator_;
            spans.push_back(Span{sep, col_separator});
        }
        frame.draw_row(l.info_row, w.left, spans, text_width);
        if (l.bar_col) frame.draw_text(l.info_row, w.right() - 1, " ", col_normal, 1);
    }
    // --- header ---
    if (l.header_top >= 0) {
        std::vector<std::string> headers = header_rows();
        std::string indent(static_cast<size_t>(pointer_len_ + marker_len_), ' ');
        for (int i = 0; i < l.header_lines; ++i) {
            Row spans{Span{indent, col_normal}, Span{headers[static_cast<size_t>(i)], col_header}};
            frame.draw_row(l.header_top + i, w.left, spans, text_width);
            if (l.bar_col) frame.draw_text(l.header_top + i, w.right() - 1, " ", col_normal, 1);
        }
    }

    // --- items (fzf: printList / printItem / printHighlighted) ---
    const int count = list_count();
    const Pattern* pattern = merger_->pattern();
    std::vector<uint32_t> match_positions;
    int bar_length = 0, bar_start = 0;
    if (!scrollbar_.empty() && count > l.list_rows && l.list_rows > 0) {
        bar_length = std::max(1, l.list_rows * l.list_rows / count);
        bar_start = count == l.list_rows ? 0
                    : std::min(l.list_rows - bar_length, (l.list_rows - bar_length) * offset_ / (count - l.list_rows));
    }
    const int item_budget = w.width - l.bar_col;
    for (int line = 0; line < l.list_rows; ++line) {
        int row = l.item_row(line);
        if (row < 0 || row >= term_rows_) continue;
        int idx = offset_ + line;
        if (idx >= count) {
            frame.draw_row(row, w.left, {}, item_budget);
        } else {
            ItemRef ref = merger_->get(static_cast<uint32_t>(idx));
            bool current = idx == cy_;
            bool selected = selected_.count(ref.index()) > 0;
            Row spans;
            if (current) spans.push_back(Span{pointer_, col_current_pointer});
            else spans.push_back(Span{pointer_empty_, col_pointer_empty});
            if (selected) spans.push_back(Span{opts_.marker.value_or(">"), current ? col_current_marker : col_marker});
            else spans.push_back(Span{marker_empty_, current ? col_current_empty : col_normal});

            std::string item_text(ref.text());
            if (item_text.find('\x1b') != std::string::npos) item_text = strip_ansi_codes(item_text);
            const Style base = current ? col_current : (selected ? col_selected : col_normal);
            const Style hl = current ? col_current_match : (selected ? col_selected_match : col_match);

            match_positions.clear();
            if (pattern && !pattern->empty()) pattern->match(*ref.chunk, ref.idx, &match_positions);
            if (!match_positions.empty() && !item_text.empty()) {
                std::u32string u32 = to_utf32(item_text);
                std::vector<char> highlighted(u32.size(), 0);
                for (uint32_t p : match_positions) if (p < u32.size()) highlighted[p] = 1;
                size_t seg_start = 0;
                bool seg_hl = !u32.empty() && highlighted[0] != 0;
                for (size_t p = 1; p <= u32.size(); ++p) {
                    bool is_hl = p < u32.size() && highlighted[p] != 0;
                    if (p == u32.size() || is_hl != seg_hl) {
                        std::string seg;
                        utf8::unchecked::utf32to8(u32.begin() + static_cast<long>(seg_start),
                                                  u32.begin() + static_cast<long>(p), std::back_inserter(seg));
                        spans.push_back(Span{seg, seg_hl ? hl : base});
                        seg_start = p;
                        seg_hl = is_hl;
                    }
                }
            } else {
                spans.push_back(Span{item_text, base});
            }
            if (opts_.cursor_line && current) {
                // --highlight-line: the background covers the whole row
                int used = pointer_len_ + marker_len_ + static_cast<int>(visible_width(item_text));
                if (used < item_budget) spans.push_back(Span{std::string(static_cast<size_t>(item_budget - used), ' '), col_current_empty});
            }
            frame.draw_row(row, w.left, spans, item_budget);
        }
        if (l.bar_col) {
            bool has_bar = bar_length > 0 && line >= bar_start && line < bar_start + bar_length;
            frame.draw_text(row, w.right() - 1, has_bar ? scrollbar_ : " ", has_bar ? col_scrollbar : col_normal, 1);
        }
    }

    write_all(tty_out_, frame.bytes().data(), frame.bytes().size());
    place_cursor();
    if (!inputless_) show_cursor(tty_out_);
    else hide_cursor(tty_out_);

    if (l.preview && preview_dirty_) paint_preview(false);
    needs_repaint_ = false;
    preview_dirty_ = false;
}

void Terminal::paint_preview(bool force) {
    const Rect& pw = layout_.pwindow;
    PreviewWorker::Content c = preview_.content();
    // Completed output for the current request goes into the cache.
    if (c.complete && !preview_request_key_.empty() && c.version == preview_request_version_ &&
        !c.text.empty()) {
        cache_preview(preview_request_key_, c.text);
        preview_request_key_.clear();
    }
    if (pw.width <= 0 || pw.height <= 0) return;

    // Skip the write entirely when neither the content nor the scroll
    // position changed since the pane was last painted: a preview holding a
    // graphics blob (sixel / iTerm2 / kitty) must not be re-emitted on every
    // unrelated frame.
    if (preview_total_lines_ > 0 && preview_offset_ >= static_cast<int>(preview_total_lines_)) {
        preview_offset_ = static_cast<int>(preview_total_lines_) - 1;
    }
    if (preview_offset_ < 0) preview_offset_ = 0;
    if (!force && last_painted_valid_ && c.text == last_painted_preview_ &&
        preview_offset_ == last_painted_offset_) {
        return;
    }
    {
        FrameRenderer clear_frame(term_rows_, term_cols_);
        clear_frame.clear_region(pw.top, pw.left, pw.height, pw.width);
        write_all(tty_out_, clear_frame.bytes().data(), clear_frame.bytes().size());
    }
    int painted_offset = preview_offset_;
    size_t total = 0;
    write_preview_content(tty_out_, pw.top, pw.left, c.text, static_cast<size_t>(painted_offset),
                          pw.height, pw.width, total);
    preview_total_lines_ = total;
    if (preview_total_lines_ > 0 && preview_offset_ >= static_cast<int>(preview_total_lines_)) {
        preview_offset_ = static_cast<int>(preview_total_lines_) - 1;
    }
    last_painted_preview_ = c.text;
    last_painted_offset_ = painted_offset;
    last_painted_valid_ = true;
    place_cursor();
}

// ---------------------------------------------------------------------------
// Mouse (fzf: actMouse, reduced to the windows this renderer has)
// ---------------------------------------------------------------------------

bool Terminal::handle_mouse() {
    const MouseInfo& me = current_key_.mouse;
    const Layout& l = layout_;
    int mx = me.x, my = me.y;
    auto in_preview = [&]() { return l.preview && l.pborder.contains(my, mx); };
    auto actions_for = [&](EventType t) -> ActionList {
        auto it = keymap_.find(event_of(t));
        return it == keymap_.end() ? ActionList{} : it->second;
    };

    // Scrolling
    if (me.button == MouseInfo::Button::WheelUp || me.button == MouseInfo::Button::WheelDown) {
        bool up = me.button == MouseInfo::Button::WheelUp;
        if (l.window.contains(my, mx) && list_count() > 0) {
            EventType evt = up ? (me.shift ? EventType::SScrollUp : EventType::ScrollUp)
                               : (me.shift ? EventType::SScrollDown : EventType::ScrollDown);
            return do_actions(actions_for(evt));
        }
        if (in_preview()) {
            return do_actions(actions_for(up ? EventType::PreviewScrollUp : EventType::PreviewScrollDown));
        }
        return true;
    }
    if (me.motion != MouseInfo::Motion::Pressed) return true;
    if (in_preview()) return true;
    if (!l.window.contains(my, mx)) return true;

    // Prompt line: move the cursor
    if (my == l.prompt_row && !inputless_) {
        int prompt_len = static_cast<int>(visible_width(prompt_string_));
        int target = mx - l.window.left - prompt_len;
        cx_ = static_cast<size_t>(std::clamp(target, 0, static_cast<int>(input_.size())));
        return true;
    }
    if (my == l.info_row) return true;

    // Header
    if (l.header_top >= 0 && my >= l.header_top && my < l.header_top + l.header_lines) {
        int col = mx - l.window.left - pointer_len_ - marker_len_;
        if (col < 0) return true;
        int line = my - l.header_top;
        // fzf: FZF_CLICK_HEADER_LINE counts from the list side in the
        // default layout
        click_header_line_ = opts_.layout == LayoutType::Reverse ? line + 1 : l.header_lines - line;
        click_header_column_ = col + 1;
        return do_actions(actions_for(EventType::ClickHeader));
    }

    int line = l.line_of_row(my);
    if (line < 0 || line >= l.list_rows) return true;
    int cy = offset_ + line;
    if (cy < 0 || cy >= list_count()) return true;

    bool left = me.button == MouseInfo::Button::Left;
    bool right = me.button == MouseInfo::Button::Right;
    if (!left && !right) return true;

    auto now = std::chrono::steady_clock::now();
    bool is_double = false;
    if (left) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_click_time_);
        is_double = elapsed.count() < 500 && my == last_click_y_ && std::abs(mx - last_click_x_) <= 2;
        last_click_time_ = now;
        last_click_x_ = mx;
        last_click_y_ = my;
        if (is_double) last_click_time_ = now - std::chrono::seconds(10);   // three clicks are not two doubles
    }
    if (is_double) {
        if (vset(cy) && cy_ < list_count()) return do_actions(actions_for(EventType::DoubleClick));
        return true;
    }
    vset(cy);
    needs_repaint_ = true;
    EventType evt = right ? (me.shift ? EventType::SRightClick : EventType::RightClick)
                          : (me.shift ? EventType::SLeftClick : EventType::LeftClick);
    return do_actions(actions_for(evt));
}

// ---------------------------------------------------------------------------
// Event dispatch
// ---------------------------------------------------------------------------

void Terminal::process_event(const Event& ev, const KeyEvent& key) {
    // --expect keys end the session before anything else (fzf: t.expect).
    for (const auto& [k, name] : expect_) {
        if (key_match(k, ev)) {
            pressed_ = name;
            request_exit(Exit::Close);
            return;
        }
    }
    if (ev.type < EventType::Invalid) {
        last_key_ = event_key_name(ev);
        last_activity_ = std::chrono::steady_clock::now();
    }
    current_event_ = ev;
    current_key_ = key;
    std::u32string previous_input = input_;
    size_t previous_cx = cx_;
    uint64_t previous_version = version_;
    beof_ = false;

    ActionList actions;
    auto it = keymap_.find(ev);
    if (it != keymap_.end()) actions = it->second;
    if (actions.empty() && ev.type == EventType::Rune) {
        do_action(Action{ActionType::Char, ""});
    } else if (!do_actions(actions)) {
        return;
    }
    bool query_changed = input_ != previous_input;
    if (query_changed) {
        pending_change_ = true;
        query_changed_ = true;
        changed_ = true;
    }
    if (query_changed || cx_ != previous_cx) needs_repaint_ = true;
    if (beof_) fire_event(EventType::BackwardEOF);
    if (version_ != previous_version) fire_event(EventType::Multi);
}

void Terminal::request_exit(Exit e) {
    if (exit_ == Exit::None) exit_ = e;
    looping_ = false;
}

// fzf: Terminal.output plus the exit codes of the render loop's exit paths.
RunResult Terminal::finish() {
    RunResult r;
    switch (exit_) {
        case Exit::Close: {
            if (opts_.print_query) r.lines.push_back(query_utf8());
            if (!expect_.empty()) r.lines.push_back(pressed_);
            for (const auto& s : print_queue_) r.lines.push_back(s);
            bool found = !selected_.empty();
            if (!found) {
                if (ItemRef cur = current_item()) {
                    r.lines.push_back(builder_.output_text(cur));
                    found = true;
                }
            } else {
                for (const auto& sel : sorted_selected()) r.lines.push_back(builder_.output_text(sel.ref));
            }
            r.exit_code = found ? 0 : 1;
            break;
        }
        case Exit::PrintQuery:
            r.lines.push_back(query_utf8());
            r.exit_code = 0;
            break;
        case Exit::Become:
            r.exit_code = 126;
            break;
        case Exit::Fatal:
            r.exit_code = 2;
            break;
        case Exit::Quit:
        case Exit::None:
        default:
            r.exit_code = 130;
            break;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Main loop (fzf: Terminal.Loop, reduced to one thread with select())
// ---------------------------------------------------------------------------

RunResult Terminal::run() {
    set_tabstop(opts_.tabstop);
    update_terminal_size();

    if (!make_self_pipe(winch_read_fd_, winch_write_fd_)) winch_read_fd_ = winch_write_fd_ = -1;
    if (!make_self_pipe(wake_read_fd_, wake_write_fd_)) wake_read_fd_ = wake_write_fd_ = -1;
    if (winch_write_fd_ >= 0) install_sigwinch_handler(winch_write_fd_);

    // SIGTERM/SIGHUP: abort cleanly (restore the terminal, exit 130)
    // instead of dying mid-alt-screen with the tty in raw mode.
    g_termination_requested = 0;
    g_termination_wake_fd = winch_write_fd_;
    struct sigaction term_sa {};
    term_sa.sa_handler = termination_handler;
    sigemptyset(&term_sa.sa_mask);
    struct sigaction old_term_sa {}, old_hup_sa {};
    sigaction(SIGTERM, &term_sa, &old_term_sa);
    sigaction(SIGHUP, &term_sa, &old_hup_sa);

    enter_screen();

    reader_.set_wake_callback([this]() { wake_pipe(wake_write_fd_); });
    searcher_.set_wake_callback([this]() { wake_pipe(wake_write_fd_); });
    searcher_.start();
    if (can_preview()) preview_.start();

    reading_ = !reader_.finished();
    if (opts_.header_lines > 0) input_header_ = reader_.header_lines();
    compute_layout();
    submit_search();
    last_item_count_ = display_list_->count();
    last_finished_ = display_list_->finished();
    if (last_finished_) {
        reading_ = false;
        trigger_load_ = has_load_actions_;
        failed_command_ = reader_.failed_command();
    }

    // Fire the start: actions once before reading user input (fzf:
    // hasStartActions), so start:reload(...) replaces the initial list.
    fire_event(EventType::Start);
    auto end_of_batch = [&]() {
        if (pending_change_) {
            pending_change_ = false;
            fire_event(EventType::Change);
        }
        if (query_changed_ && can_preview() && has_preview_flags(preview_opts_.command).force_update) {
            ++version_;
        }
        query_changed_ = false;
        if (has_new_command_) {
            has_new_command_ = false;
            start_reload(new_command_, reload_sync_);
            new_command_.clear();
        } else if (changed_) {
            submit_search();
        }
        changed_ = false;
        if (exit_ != Exit::None) return;
        if (has_focus_actions_) {
            int32_t idx = current_index();
            if (idx != last_focus_) {
                last_focus_ = idx;
                fire_event(EventType::Focus);
            }
        }
        if (can_preview()) {
            int32_t idx = current_index();
            if (has_preview_window() && (idx != focused_index_ || version_ != preview_version_)) {
                preview_version_ = version_;
                focused_index_ = idx;
                refresh_preview(preview_opts_.command, false);
            }
        }
    };
    end_of_batch();

    KeyParser parser;
    int max_fd = tty_in_;
    if (winch_read_fd_ >= 0) max_fd = std::max(max_fd, winch_read_fd_);
    if (wake_read_fd_ >= 0) max_fd = std::max(max_fd, wake_read_fd_);

    // Escape-sequence timeout as an absolute deadline, re-armed on every
    // feed() that leaves bytes pending (one full window of silence).
    auto esc_deadline = std::chrono::steady_clock::now();
    bool esc_deadline_armed = false;

    needs_repaint_ = true;
    preview_dirty_ = true;
    if (exit_ == Exit::None) repaint();

    while (exit_ == Exit::None) {
        if (g_termination_requested) {
            request_exit(Exit::Quit);
            break;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(tty_in_, &read_fds);
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
        bool has_timeout = esc_deadline_armed || request_pending_ || prefetch_pending_;
        if (has_timeout) {
            auto deadline = std::chrono::steady_clock::time_point::max();
            if (esc_deadline_armed) deadline = esc_deadline;
            if (request_pending_ && request_deadline_ < deadline) deadline = request_deadline_;
            if (prefetch_pending_ && prefetch_after_ < deadline) deadline = prefetch_after_;
            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining < 0) remaining = 0;
            tv.tv_sec = static_cast<time_t>(remaining / 1000000);
            tv.tv_usec = static_cast<suseconds_t>(remaining % 1000000);
        }

        int n = select(max_fd + 1, &read_fds, nullptr, nullptr, has_timeout ? &tv : nullptr);
        if (n < 0) continue;   // EINTR: loop and re-check state

        if (esc_deadline_armed && parser.has_pending() &&
            std::chrono::steady_clock::now() >= esc_deadline) {
            esc_deadline_armed = false;
            for (const auto& ev : parser.timeout_tick(KeyParser::kEscapeTimeoutMs)) {
                process_event(to_event(ev), ev);
                needs_repaint_ = true;
                if (exit_ != Exit::None) break;
            }
        }

        if (winch_read_fd_ >= 0 && FD_ISSET(winch_read_fd_, &read_fds)) {
            drain_pipe(winch_read_fd_);
            if (g_termination_requested) continue;   // handled at loop top
            update_terminal_size();
            compute_layout();
            needs_repaint_ = true;
            preview_dirty_ = true;
            last_painted_valid_ = false;
            if (can_preview()) {
                // Cached previews were rendered at the old FZF_PREVIEW_COLUMNS/
                // LINES; serving them into the new pane paints a wrong-size
                // image.
                clear_preview_cache();
                refresh_preview(preview_opts_.command, true);
            }
            if (has_resize_actions_) fire_event(EventType::Resize);
        }

        if (wake_read_fd_ >= 0 && FD_ISSET(wake_read_fd_, &read_fds)) {
            drain_pipe(wake_read_fd_);
            reader_.ack_wake();
            preview_dirty_ = true;
            needs_repaint_ = true;
            for (auto& p : preview_.take_prefetched()) cache_preview(p.command, p.text);
        }
        if (prefetch_pending_ && std::chrono::steady_clock::now() >= prefetch_after_) schedule_prefetch();

        on_reader_progress();
        if (auto merger = searcher_.take()) install_merger(std::move(merger));

        if (FD_ISSET(tty_in_, &read_fds)) {
            char buf[256];
            ssize_t r = read(tty_in_, buf, sizeof(buf));
            if (r == 0) {
                // EOF on the tty (hangup with no SIGHUP delivered): abort.
                request_exit(Exit::Quit);
                break;
            }
            if (r > 0) {
                esc_deadline_armed = false;
                arm_prefetch(true);
                auto events = parser.feed(std::string(buf, static_cast<size_t>(r)));
                for (const auto& ev : events) {
                    // Every mouse report goes through the Mouse action
                    // (fzf: actMouse), which hit-tests the layout and then
                    // runs the LeftClick / ScrollUp / ... bindings.
                    Event e = ev.type == KeyType::Mouse ? event_of(EventType::Mouse) : to_event(ev);
                    if (ev.type == KeyType::Mouse && !mouse_enabled_) continue;
                    // Re-run matching before a non-editing event that
                    // follows a query change in the same read() batch (a
                    // fast "query\n" paste), so accept sees the new list.
                    if (pending_change_ && !(e.type == EventType::Rune || e.type == EventType::Backspace ||
                                             e.type == EventType::Delete)) {
                        pending_change_ = false;
                        fire_event(EventType::Change);
                        if (has_new_command_) {
                            has_new_command_ = false;
                            start_reload(new_command_, reload_sync_);
                            new_command_.clear();
                            changed_ = false;
                        } else if (changed_) {
                            submit_search_sync();
                            changed_ = false;
                        }
                    }
                    process_event(e, ev);
                    needs_repaint_ = true;
                    if (exit_ != Exit::None) break;
                }
            }
        }

        end_of_batch();
        if (exit_ != Exit::None) break;
        if (needs_repaint_ || preview_dirty_) repaint();
    }

    // Teardown. Everything else is explicit so become()/execute paths that
    // already left the screen are not touched twice.
    sigaction(SIGTERM, &old_term_sa, nullptr);
    sigaction(SIGHUP, &old_hup_sa, nullptr);
    g_termination_wake_fd = -1;
    if (winch_write_fd_ >= 0) restore_sigwinch_handler();
    reader_.set_wake_callback(nullptr);
    searcher_.set_wake_callback(nullptr);
    preview_.shutdown();
    leave_screen();

    if (winch_read_fd_ >= 0) close(winch_read_fd_);
    if (winch_write_fd_ >= 0) close(winch_write_fd_);
    if (wake_read_fd_ >= 0) close(wake_read_fd_);
    if (wake_write_fd_ >= 0) close(wake_write_fd_);
    winch_read_fd_ = winch_write_fd_ = wake_read_fd_ = wake_write_fd_ = -1;

    return finish();
}

} // namespace fzf
