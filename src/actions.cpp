// Action executor (fzf: src/terminal.go doAction / doActions and the
// query, cursor and selection helpers they use). Every ActionType the
// --bind parser accepts has a case here; the ones outside the Tier 1 set
// are no-ops (docs/DESIGN.md section 10).

#include "terminal.hpp"

#include "shellcmd.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <functional>

#include <unistd.h>
#include <utf8.h>

namespace fzf {

namespace {

constexpr int kMaxFocusEvents = 10000;   // fzf: maxFocusEvents
constexpr int kMaxMulti = 2147483647;    // fzf: maxMulti

// \pL\pN approximation for the word-motion patterns (fzf: wordRubout,
// wordNext): ASCII letters and digits, and any non-ASCII codepoint that is
// not a space or general punctuation.
bool is_word_char(char32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (is_unicode_space(c)) return false;
    if (c >= 0x2000 && c <= 0x206F) return false;   // general punctuation
    if (c >= 0x3000 && c <= 0x303F) return false;   // CJK punctuation
    if (c >= 0xFF00 && c <= 0xFF0F) return false;   // fullwidth punctuation
    return true;
}

bool is_space(char32_t c) { return is_unicode_space(c); }

} // namespace

// ---------------------------------------------------------------------------
// Query buffer
// ---------------------------------------------------------------------------

// fzf: delChar
bool Terminal::del_char() {
    if (!input_.empty() && cx_ < input_.size()) {
        input_.erase(cx_, 1);
        return true;
    }
    return false;
}

// fzf: findLastMatch(pattern, input[:cx]) + 1. `kind`: 0 = "\s\S"
// (unix-word-rubout), 1 = word boundary, 2 = sub-word boundary.
static size_t last_boundary(const std::u32string& s, size_t end, int kind, bool file_word) {
    auto boundary = [&](char32_t a, char32_t b) {
        switch (kind) {
            case 0: return is_space(a) && !is_space(b);
            case 2: if ((a >= 'a' && a <= 'z') && (b >= 'A' && b <= 'Z')) return true; [[fallthrough]];
            default:
                if (file_word) return a == U'/' && b != U'/';
                return !is_word_char(a) && is_word_char(b);
        }
    };
    if (end < 2) return 0;
    for (size_t i = end - 1; i-- > 0;) {
        if (boundary(s[i], s[i + 1])) return i + 1;
    }
    return 0;
}

// fzf: cx + findFirstMatch(pattern, input[cx:]) + 1 -- the pattern also
// matches the last character ("(.$)").
static size_t next_boundary(const std::u32string& s, size_t start, int kind, bool file_word) {
    auto boundary = [&](char32_t a, char32_t b) {
        if (kind == 2 && (a >= 'a' && a <= 'z') && (b >= 'A' && b <= 'Z')) return true;
        if (file_word) return a != U'/' && b == U'/';
        return is_word_char(a) && !is_word_char(b);
    };
    size_t n = s.size();
    if (start >= n) return start;
    for (size_t i = start; i < n; ++i) {
        if (i + 1 < n && boundary(s[i], s[i + 1])) return i + 1;
        if (i + 1 == n) return i + 1;
    }
    return n;
}

size_t Terminal::find_last_boundary(bool sub) const {
    return last_boundary(input_, cx_, sub ? 2 : 1, opts_.file_word);
}

size_t Terminal::find_next_boundary(bool sub) const {
    return next_boundary(input_, cx_, sub ? 2 : 1, opts_.file_word);
}

// fzf: rubout
void Terminal::rubout(bool word, bool sub) {
    size_t pcx = cx_;
    std::u32string after = input_.substr(cx_);
    cx_ = last_boundary(input_, cx_, word ? (sub ? 2 : 1) : 0, word && opts_.file_word);
    yanked_ = input_.substr(cx_, pcx - cx_);
    input_ = input_.substr(0, cx_) + after;
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

ItemRef Terminal::current_item() const {
    int cnt = list_count();
    if (cy_ >= 0 && cnt > 0 && cnt > cy_) return merger_->get(static_cast<uint32_t>(cy_));
    return ItemRef{};
}

int32_t Terminal::current_index() const {
    ItemRef item = current_item();
    return item ? static_cast<int32_t>(item.index()) : kMinItemIndex;
}

// fzf: vmove. Positive `o` moves towards the top of the screen. The list
// is drawn top-down (item 0 first), so that is a decreasing cy.
bool Terminal::vmove(int o, bool allow_cycle) {
    o = -o;
    int dest = cy_ + o;
    if (opts_.cycle && allow_cycle) {
        int max = list_count() - 1;
        if (dest > max) {
            if (cy_ == max) dest = 0;
        } else if (dest < 0) {
            if (cy_ == 0) dest = max;
        }
    }
    return vset(dest);
}

// fzf: vset
bool Terminal::vset(int o) {
    cy_ = std::clamp(o, 0, std::max(0, list_count() - 1));
    return cy_ == o;
}

// fzf: constrain (single-line items)
void Terminal::constrain() {
    int count = list_count();
    int max_lines = max_items();
    offset_ = std::clamp(offset_, 0, std::max(0, count));
    cy_ = std::clamp(cy_, 0, std::max(0, count - 1));
    for (int iter = 0; iter < max_lines; ++iter) {
        int num_items = max_lines;
        cy_ = std::clamp(cy_, 0, std::max(0, count - 1));
        int min_offset = std::max(cy_ - num_items + 1, 0);
        int max_offset = std::max(std::min(count - num_items, cy_), 0);
        int prev_offset = offset_;
        offset_ = std::clamp(offset_, min_offset, max_offset);
        if (opts_.scroll_off > 0) {
            int scroll_off = std::min(max_lines / 2, opts_.scroll_off);
            int new_offset = offset_;
            // 2-phase adjustment to avoid alternating between moving up and down
            for (int phase = 0; phase < 2; ++phase) {
                for (;;) {
                    int prev = new_offset;
                    int lines_before = cy_ - new_offset;
                    int lines_after = max_lines - (lines_before + 1);
                    // Stuck in the middle, nothing to do
                    if (lines_before < scroll_off && lines_after < scroll_off) break;
                    if (phase == 0 && lines_before < scroll_off) {
                        new_offset = std::max(min_offset, new_offset - 1);
                    } else if (phase == 1 && lines_after < scroll_off) {
                        new_offset = std::min(max_offset, new_offset + 1);
                    }
                    if (new_offset == prev) break;
                }
                offset_ = new_offset;
            }
        }
        if (offset_ == prev_offset) break;
    }
}

// ---------------------------------------------------------------------------
// Selection (fzf: selectItem & co.)
// ---------------------------------------------------------------------------

bool Terminal::select_item(const ItemRef& item) {
    if (static_cast<int64_t>(selected_.size()) >= static_cast<int64_t>(multi_)) return false;
    uint32_t idx = item.index();
    if (selected_.count(idx)) return true;
    selected_[idx] = Selected{++select_seq_, item};
    ++version_;
    return true;
}

bool Terminal::select_item_changed(const ItemRef& item) {
    if (selected_.count(item.index())) return false;
    return select_item(item);
}

void Terminal::deselect_item(const ItemRef& item) {
    selected_.erase(item.index());
    ++version_;
}

bool Terminal::deselect_item_changed(const ItemRef& item) {
    if (selected_.count(item.index())) {
        deselect_item(item);
        return true;
    }
    return false;
}

bool Terminal::toggle_item(const ItemRef& item) {
    if (!selected_.count(item.index())) return select_item(item);
    deselect_item(item);
    return true;
}

std::vector<Terminal::Selected> Terminal::sorted_selected() const {
    std::vector<Selected> sels;
    sels.reserve(selected_.size());
    for (const auto& [idx, sel] : selected_) sels.push_back(sel);
    std::sort(sels.begin(), sels.end(), [](const Selected& a, const Selected& b) { return a.seq < b.seq; });
    return sels;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

// fzf: doActions -- runs the list, then the `focus` actions (repeatedly)
// whenever the current item changed.
bool Terminal::do_actions(const ActionList& actions_in) {
    ActionList actions = actions_in;
    for (int iter = 0; iter <= kMaxFocusEvents; ++iter) {
        int32_t current = current_index();
        for (const auto& a : actions) {
            if (!do_action(a)) return false;
            // A terminal action performed. We should stop processing more.
            if (!looping_) break;
        }
        auto on_focus = keymap_.find(event_of(EventType::Focus));
        if (on_focus != keymap_.end() && iter < kMaxFocusEvents && looping_) {
            int32_t new_index = current_index();
            if (new_index != current) {
                last_focus_ = new_index;
                actions = on_focus->second;
                continue;
            }
        }
        break;
    }
    return true;
}

// fzf: doAction
bool Terminal::do_action(const Action& a) {
    // Keep track of the current query before the action is executed, so
    // we can restore it when the input section is hidden (--no-input).
    std::u32string current_input = input_;
    const std::string& arg = a.arg;

    auto toggle = [&]() -> bool {
        ItemRef current = current_item();
        return current && toggle_item(current);
    };
    // change-* takes the argument verbatim; transform-* runs it and
    // captures the output; bg-transform-* is run synchronously as well.
    auto capture = [&](bool first_line_only, const std::function<void(const std::string&)>& fn) {
        if (a.type >= ActionType::BgTransformBorderLabel && a.type <= ActionType::BgTransform) {
            fn(execute_command(arg, false, true, true, first_line_only));
        } else if (a.type >= ActionType::TransformBorderLabel && a.type <= ActionType::Transform) {
            fn(execute_command(arg, false, true, true, first_line_only));
        } else {
            fn(arg);
        }
    };
    auto update_preview_window = [&]() {
        needs_repaint_ = true;
        preview_dirty_ = true;
        last_painted_valid_ = false;
        compute_layout();
    };

    switch (a.type) {
        case ActionType::Ignore: case ActionType::Start: case ActionType::Click:
            break;
        case ActionType::Become:
            become(arg);
            break;
        case ActionType::Bell:
            write_all(tty_out_, "\a", 1);
            break;
        case ActionType::Execute: case ActionType::ExecuteSilent:
            execute_command(arg, false, a.type == ActionType::ExecuteSilent, false, false);
            break;
        case ActionType::ExecuteMulti:
            execute_command(arg, true, false, false, false);
            break;
        case ActionType::Invalid:
            return false;
        case ActionType::TogglePreview: case ActionType::ShowPreview: case ActionType::HidePreview: {
            bool act = false;
            switch (a.type) {
                case ActionType::ShowPreview: act = !has_preview_window() && can_preview(); break;
                case ActionType::HidePreview: act = has_preview_window(); break;
                default: act = has_preview_window() || can_preview(); break;
            }
            if (act) {
                preview_hidden_ = !preview_hidden_;
                update_preview_window();
                if (has_preview_window()) {
                    refresh_preview(preview_opts_.command, false);
                } else {
                    cancel_preview();
                }
            }
            break;
        }
        case ActionType::TogglePreviewWrap: case ActionType::TogglePreviewWrapWord:
            if (has_preview_window()) {
                if (a.type == ActionType::TogglePreviewWrapWord) {
                    preview_opts_.wrap_word = !preview_opts_.wrap_word;
                    preview_opts_.wrap = preview_opts_.wrap_word;
                } else {
                    preview_opts_.wrap = !preview_opts_.wrap;
                    if (!preview_opts_.wrap) preview_opts_.wrap_word = false;
                }
                preview_dirty_ = true;
            }
            break;
        case ActionType::TransformPrompt: case ActionType::BgTransformPrompt:
            capture(true, [&](const std::string& prompt) { prompt_string_ = prompt; needs_repaint_ = true; });
            break;
        case ActionType::TransformQuery: case ActionType::BgTransformQuery:
            capture(true, [&](const std::string& query) {
                std::u32string q;
                utf8::unchecked::utf8to32(query.begin(), query.end(), std::back_inserter(q));
                set_input(q);
            });
            break;
        case ActionType::ToggleSort:
            sort_ = !sort_;
            changed_ = true;
            break;
        case ActionType::PreviewTop:
            if (has_preview_window()) scroll_preview_to(0);
            break;
        case ActionType::PreviewBottom:
            if (has_preview_window()) scroll_preview_to(static_cast<int>(preview_total_lines_) - layout_.preview_lines);
            break;
        case ActionType::PreviewUp:
            if (has_preview_window()) scroll_preview_by(-1);
            break;
        case ActionType::PreviewDown:
            if (has_preview_window()) scroll_preview_by(1);
            break;
        case ActionType::PreviewPageUp:
            if (has_preview_window()) scroll_preview_by(-layout_.preview_lines);
            break;
        case ActionType::PreviewPageDown:
            if (has_preview_window()) scroll_preview_by(layout_.preview_lines);
            break;
        case ActionType::PreviewHalfPageUp:
            if (has_preview_window()) scroll_preview_by(-layout_.preview_lines / 2);
            break;
        case ActionType::PreviewHalfPageDown:
            if (has_preview_window()) scroll_preview_by(layout_.preview_lines / 2);
            break;
        case ActionType::BeginningOfLine:
            cx_ = 0;
            break;
        case ActionType::BackwardChar:
            if (cx_ > 0) --cx_;
            break;
        case ActionType::PrintQuery:
            request_exit(Exit::PrintQuery);
            break;
        case ActionType::ChangeMulti: {
            int multi = multi_;
            if (arg.empty()) {
                multi = kMaxMulti;
            } else {
                char* end = nullptr;
                long n = std::strtol(arg.c_str(), &end, 10);
                if (end && *end == '\0' && n >= 0) multi = static_cast<int>(std::min<long>(n, kMaxMulti));
            }
            if (multi_ > 0 && multi != multi_) {
                selected_.clear();
                ++version_;
            }
            multi_ = multi;
            needs_repaint_ = true;
            break;
        }
        case ActionType::ChangeQuery: {
            std::u32string q;
            utf8::unchecked::utf8to32(arg.begin(), arg.end(), std::back_inserter(q));
            set_input(q);
            break;
        }
        case ActionType::ChangeHeader: case ActionType::TransformHeader: case ActionType::BgTransformHeader:
            capture(false, [&](const std::string& header) {
                header0_.clear();
                if (!header.empty()) {
                    std::string h = header;
                    if (h.back() == '\n') h.pop_back();
                    header0_ = str_lines(h);
                }
                click_header_line_ = 0;
                click_header_column_ = 0;
                needs_repaint_ = true;
                compute_layout();
            });
            break;
        case ActionType::ChangeBorderLabel: case ActionType::TransformBorderLabel: case ActionType::BgTransformBorderLabel:
            capture(true, [&](const std::string& label) { border_label_ = label; });
            break;
        case ActionType::ChangePreviewLabel: case ActionType::TransformPreviewLabel: case ActionType::BgTransformPreviewLabel:
            capture(true, [&](const std::string& label) { preview_label_ = label; });
            break;
        case ActionType::ChangeGhost: case ActionType::TransformGhost: case ActionType::BgTransformGhost:
            capture(true, [&](const std::string& ghost) { ghost_ = ghost; needs_repaint_ = true; });
            break;
        case ActionType::ChangePointer: case ActionType::TransformPointer: case ActionType::BgTransformPointer:
            capture(true, [&](const std::string& pointer) { pointer_ = pointer; needs_repaint_ = true; });
            break;
        case ActionType::Transform: case ActionType::BgTransform:
            capture(false, [&](const std::string& body_in) {
                std::string body = body_in;
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
                size_t start = 0;
                while (start < body.size() && (body[start] == '\n' || body[start] == '\r')) ++start;
                body = body.substr(start);
                // Allow 'put' if the triggering key is a printable character
                try {
                    ActionList actions = parse_single_action_list(body, current_event_.printable());
                    do_actions(actions);
                } catch (const OptionError&) {
                    // fzf ignores unparsable output
                }
            });
            break;
        case ActionType::ChangePrompt:
            prompt_string_ = arg;
            needs_repaint_ = true;
            break;
        case ActionType::Preview:
            // One-shot preview: force the window open and run this command.
            if (!has_preview_window()) {
                preview_hidden_ = false;
                if (preview_opts_.command.empty()) preview_opts_.command = arg;
                update_preview_window();
            }
            refresh_preview(arg, false);
            break;
        case ActionType::RefreshPreview:
            refresh_preview(preview_opts_.command, true);
            break;
        case ActionType::ReplaceQuery:
            if (ItemRef current = current_item()) {
                std::string text(current.text());
                std::u32string q;
                utf8::unchecked::utf8to32(text.begin(), text.end(), std::back_inserter(q));
                set_input(q);
            }
            break;
        case ActionType::Fatal:
            request_exit(Exit::Fatal);
            break;
        case ActionType::Abort:
            request_exit(Exit::Quit);
            break;
        case ActionType::DeleteChar:
            del_char();
            break;
        case ActionType::DeleteCharEof:
            if (!del_char() && cx_ == 0) request_exit(Exit::Quit);
            break;
        case ActionType::EndOfLine:
            cx_ = input_.size();
            break;
        case ActionType::Cancel:
            if (input_.empty()) {
                request_exit(Exit::Quit);
            } else {
                yanked_ = input_;
                input_.clear();
                cx_ = 0;
            }
            break;
        case ActionType::BackwardDeleteCharEof:
            if (input_.empty()) {
                request_exit(Exit::Quit);
            } else if (cx_ > 0) {
                input_.erase(cx_ - 1, 1);
                --cx_;
            }
            break;
        case ActionType::ForwardChar:
            if (cx_ < input_.size()) ++cx_;
            break;
        case ActionType::BackwardDeleteChar:
            beof_ = input_.empty();
            if (cx_ > 0) {
                input_.erase(cx_ - 1, 1);
                --cx_;
            }
            break;
        case ActionType::SelectAll:
            if (multi_ > 0) {
                for (uint32_t i = 0; i < merger_->size(); ++i) {
                    if (!select_item(merger_->get(i))) break;
                }
                needs_repaint_ = true;
            }
            break;
        case ActionType::DeselectAll:
            if (multi_ > 0) {
                for (uint32_t i = 0; i < merger_->size() && !selected_.empty(); ++i) deselect_item(merger_->get(i));
                needs_repaint_ = true;
            }
            break;
        case ActionType::Close:
            if (has_preview_window()) {
                preview_hidden_ = true;
                update_preview_window();
                cancel_preview();
            } else {
                request_exit(Exit::Quit);
            }
            break;
        case ActionType::Select:
            if (ItemRef current = current_item(); multi_ > 0 && current && select_item_changed(current)) needs_repaint_ = true;
            break;
        case ActionType::Deselect:
            if (ItemRef current = current_item(); multi_ > 0 && current && deselect_item_changed(current)) needs_repaint_ = true;
            break;
        case ActionType::Toggle:
            if (multi_ > 0 && list_count() > 0 && toggle()) needs_repaint_ = true;
            break;
        case ActionType::ToggleAll:
            if (multi_ > 0) {
                std::vector<char> prev(merger_->size(), 0);
                for (uint32_t i = 0; i < merger_->size() && !selected_.empty(); ++i) {
                    ItemRef item = merger_->get(i);
                    if (selected_.count(item.index())) {
                        prev[i] = 1;
                        deselect_item(item);
                    }
                }
                for (uint32_t i = 0; i < merger_->size(); ++i) {
                    if (!prev[i] && !select_item(merger_->get(i))) break;
                }
                needs_repaint_ = true;
            }
            break;
        case ActionType::ToggleIn:
            return do_action(Action{opts_.layout != LayoutType::Default ? ActionType::ToggleUp : ActionType::ToggleDown, ""});
        case ActionType::ToggleOut:
            return do_action(Action{opts_.layout != LayoutType::Default ? ActionType::ToggleDown : ActionType::ToggleUp, ""});
        case ActionType::ToggleDown:
            if (multi_ > 0 && list_count() > 0 && toggle()) {
                vmove(-1, true);
                needs_repaint_ = true;
            }
            break;
        case ActionType::ToggleUp:
            if (multi_ > 0 && list_count() > 0 && toggle()) {
                vmove(1, true);
                needs_repaint_ = true;
            }
            break;
        case ActionType::Down: case ActionType::DownMatch:
            vmove(-1, true);
            needs_repaint_ = true;
            break;
        case ActionType::Up: case ActionType::UpMatch:
            vmove(1, true);
            needs_repaint_ = true;
            break;
        case ActionType::Accept:
            request_exit(Exit::Close);
            break;
        case ActionType::AcceptNonEmpty:
            if (!selected_.empty() || list_count() > 0 || (!reading_ && display_list_->count() == 0)) {
                request_exit(Exit::Close);
            }
            break;
        case ActionType::AcceptOrPrintQuery:
            if (!selected_.empty() || list_count() > 0) request_exit(Exit::Close);
            else request_exit(Exit::PrintQuery);
            break;
        case ActionType::ClearScreen:
            full_redraw_ = true;
            last_painted_valid_ = false;
            needs_repaint_ = true;
            preview_dirty_ = true;
            break;
        case ActionType::ClearQuery:
            input_.clear();
            cx_ = 0;
            break;
        case ActionType::ClearSelection:
            if (multi_ > 0) {
                selected_.clear();
                ++version_;
                needs_repaint_ = true;
            }
            break;
        case ActionType::First: case ActionType::Best:
            vset(0);
            constrain();
            needs_repaint_ = true;
            break;
        case ActionType::Last:
            vset(list_count() - 1);
            constrain();
            needs_repaint_ = true;
            break;
        case ActionType::Position: {
            char* end = nullptr;
            long n = std::strtol(arg.c_str(), &end, 10);
            if (end && *end == '\0' && !arg.empty()) {
                if (n > 0) --n;
                else if (n < 0) n += list_count();
                vset(static_cast<int>(n));
                constrain();
                needs_repaint_ = true;
            }
            break;
        }
        case ActionType::Put: {
            std::u32string str;
            utf8::unchecked::utf8to32(arg.begin(), arg.end(), std::back_inserter(str));
            input_.insert(cx_, str);
            cx_ += str.size();
            break;
        }
        case ActionType::Print:
            print_queue_.push_back(arg);
            break;
        case ActionType::UnixLineDiscard:
            beof_ = input_.empty();
            if (cx_ > 0) {
                yanked_ = input_.substr(0, cx_);
                input_ = input_.substr(cx_);
                cx_ = 0;
            }
            break;
        case ActionType::UnixWordRubout:
            beof_ = input_.empty();
            if (cx_ > 0) rubout(false, false);
            break;
        case ActionType::BackwardKillWord:
            beof_ = input_.empty();
            if (cx_ > 0) rubout(true, false);
            break;
        case ActionType::BackwardKillSubWord:
            beof_ = input_.empty();
            if (cx_ > 0) rubout(true, true);
            break;
        case ActionType::Yank:
            input_.insert(cx_, yanked_);
            cx_ += yanked_.size();
            break;
        case ActionType::PageUp: case ActionType::PageDown: case ActionType::HalfPageUp: case ActionType::HalfPageDown: {
            int max_lines = max_items();
            int lines = max_lines - 1;
            if (a.type == ActionType::HalfPageUp || a.type == ActionType::HalfPageDown) lines = max_lines / 2;
            lines = std::max(1, lines);
            int direction = (a.type == ActionType::PageUp || a.type == ActionType::HalfPageUp) ? 1 : -1;
            vmove(direction * lines, false);
            needs_repaint_ = true;
            break;
        }
        case ActionType::OffsetUp: case ActionType::OffsetDown: {
            // Scroll the list by one row, keeping the cursor on screen.
            int diff = a.type == ActionType::OffsetUp ? -1 : 1;
            offset_ += diff;
            int before = offset_;
            constrain();
            if (before != offset_) {
                offset_ = before;
                vmove(-diff, false);
            }
            needs_repaint_ = true;
            break;
        }
        case ActionType::OffsetMiddle: {
            int soff = opts_.scroll_off;
            opts_.scroll_off = layout_.list_rows;
            constrain();
            opts_.scroll_off = soff;
            needs_repaint_ = true;
            break;
        }
        case ActionType::BackwardWord:
            cx_ = find_last_boundary(false);
            break;
        case ActionType::ForwardWord:
            cx_ = find_next_boundary(false);
            break;
        case ActionType::BackwardSubWord:
            cx_ = find_last_boundary(true);
            break;
        case ActionType::ForwardSubWord:
            cx_ = find_next_boundary(true);
            break;
        case ActionType::KillWord: case ActionType::KillSubWord: {
            size_t ncx = find_next_boundary(a.type == ActionType::KillSubWord);
            if (ncx > cx_) {
                yanked_ = input_.substr(cx_, ncx - cx_);
                input_.erase(cx_, ncx - cx_);
            }
            break;
        }
        case ActionType::KillLine:
            if (cx_ < input_.size()) {
                yanked_ = input_.substr(cx_);
                input_.resize(cx_);
            }
            break;
        case ActionType::Char:
            input_.insert(cx_, 1, current_event_.ch);
            ++cx_;
            break;
        case ActionType::ToggleSearch:
            paused_ = !paused_;
            changed_ = !paused_;
            needs_repaint_ = true;
            break;
        case ActionType::EnableSearch:
            paused_ = false;
            changed_ = true;
            needs_repaint_ = true;
            break;
        case ActionType::DisableSearch:
            paused_ = true;
            needs_repaint_ = true;
            break;
        case ActionType::ShowHeader:
            header_visible_ = true;
            needs_repaint_ = true;
            break;
        case ActionType::HideHeader:
            header_visible_ = false;
            needs_repaint_ = true;
            break;
        case ActionType::ToggleHeader:
            header_visible_ = !header_visible_;
            needs_repaint_ = true;
            break;
        case ActionType::ToggleInput: case ActionType::ShowInput: case ActionType::HideInput: {
            bool before = inputless_;
            switch (a.type) {
                case ActionType::ToggleInput: inputless_ = !inputless_; break;
                case ActionType::ShowInput: inputless_ = false; break;
                default: inputless_ = true; break;
            }
            if (inputless_ != before) {
                if (inputless_) hide_cursor(tty_out_);
                needs_repaint_ = true;
            }
            break;
        }
        case ActionType::Search: {
            std::u32string q;
            utf8::unchecked::utf8to32(arg.begin(), arg.end(), std::back_inserter(q));
            set_input(q);
            changed_ = true;
            break;
        }
        case ActionType::TransformSearch: case ActionType::BgTransformSearch:
            capture(true, [&](const std::string& query) {
                std::u32string q;
                utf8::unchecked::utf8to32(query.begin(), query.end(), std::back_inserter(q));
                set_input(q);
                changed_ = true;
            });
            break;
        case ActionType::Trigger: {
            std::vector<Event> chords;
            try {
                chords = parse_key_chords(arg, "");
            } catch (const OptionError&) {
                break;
            }
            for (const Event& chord : chords) {
                if (triggering_.count(chord)) continue;   // avoid recursive triggering
                auto acts = keymap_.find(chord);
                if (acts == keymap_.end()) continue;
                triggering_.insert(chord);
                ActionList list = acts->second;
                do_actions(list);
                triggering_.erase(chord);
            }
            break;
        }
        case ActionType::SigStop: {
            // fzf: notifyStop -- restore the terminal, stop the process
            // group, re-initialize when continued.
            leave_screen();
            kill(0, SIGTSTP);
            enter_screen();
            full_redraw_ = true;
            last_painted_valid_ = false;
            needs_repaint_ = true;
            preview_dirty_ = true;
            break;
        }
        case ActionType::Mouse:
            if (!handle_mouse()) return false;
            break;
        case ActionType::Reload: case ActionType::ReloadSync: {
            failed_command_.clear();
            PlusList list = build_plus_list(arg, false);
            bool valid = list.valid;
            if (!valid) {
                // We run the command even when there's no match: if the
                // template has no slots, or if it has {q}.
                TemplateFlags flags = has_preview_flags(arg);
                valid = !flags.slot || flags.force_update;
            }
            if (valid) {
                Expansion exp = expand(arg, false, list);
                remove_files(new_command_temps_);
                new_command_ = exp.command;
                new_command_temps_ = exp.temp_files;
                has_new_command_ = true;
                reload_sync_ = a.type == ActionType::ReloadSync;
                reading_ = true;
            }
            break;
        }
        case ActionType::Unbind: case ActionType::Rebind: case ActionType::ToggleBind: {
            std::vector<Event> keys;
            try {
                keys = parse_key_chords(arg, "PANIC");
            } catch (const OptionError&) {
                break;
            }
            for (const Event& key : keys) {
                if (a.type == ActionType::Unbind) {
                    keymap_.erase(key);
                } else if (a.type == ActionType::Rebind) {
                    auto org = keymap_org_.find(key);
                    if (org != keymap_org_.end()) keymap_[key] = org->second;
                } else if (keymap_.count(key)) {
                    keymap_.erase(key);
                } else {
                    auto org = keymap_org_.find(key);
                    if (org != keymap_org_.end()) keymap_[key] = org->second;
                }
            }
            break;
        }
        case ActionType::ChangePreview:
            if (preview_opts_.command != arg) {
                preview_opts_.command = arg;
                update_preview_window();
                clear_preview_cache();
                if (!can_preview()) {
                    cancel_preview();
                    preview_.set_content(std::string(), true);
                } else {
                    refresh_preview(preview_opts_.command, false);
                }
            }
            break;
        case ActionType::ChangePreviewWindow: {
            // NOTE: We intentionally use "previewOpts" instead of "activePreviewOpts" here
            PreviewOpts current = preview_opts_;
            preview_opts_ = initial_preview_opts_;
            preview_opts_.command = current.command;
            preview_opts_.wrap = current.wrap;
            preview_opts_.wrap_word = current.wrap_word;
            // Split window options; the list rotates so repeated presses
            // cycle through the alternatives.
            std::string spec = arg;
            std::string first = spec;
            size_t bar = spec.find('|');
            if (bar != std::string::npos) {
                first = spec.substr(0, bar);
                std::string rest = spec.substr(bar + 1);
                const_cast<Action&>(a).arg = rest + "|" + first;
            }
            if (!first.empty() && initial_preview_opts_.hidden) preview_opts_.hidden = false;
            try {
                apply_preview_window(preview_opts_, first);
            } catch (const OptionError&) {
                // validated when the bind was parsed
            }
            bool was_hidden = preview_hidden_;
            preview_hidden_ = preview_opts_.hidden;
            update_preview_window();
            if (has_preview_window()) {
                if (was_hidden || current.command != preview_opts_.command) {
                    refresh_preview(preview_opts_.command, false);
                } else if (current.scroll != preview_opts_.scroll) {
                    scroll_preview_to(evaluate_scroll_offset());
                }
            } else {
                cancel_preview();
            }
            break;
        }
        case ActionType::NextSelected: case ActionType::PrevSelected:
            if (!selected_.empty()) {
                int total = list_count();
                for (int i = 1; i < total; ++i) {
                    int y = (cy_ + i) % total;
                    bool backwards = (opts_.layout == LayoutType::Default && a.type == ActionType::NextSelected) ||
                                     (opts_.layout != LayoutType::Default && a.type == ActionType::PrevSelected);
                    if (backwards) y = (cy_ - i + total) % total;
                    if (selected_.count(merger_->get(static_cast<uint32_t>(y)).index())) {
                        vset(y);
                        needs_repaint_ = true;
                        break;
                    }
                }
            }
            break;
        case ActionType::ToggleWrap: case ActionType::ToggleWrapWord:
        case ActionType::ToggleMultiLine: case ActionType::ToggleHscroll:
        case ActionType::ToggleRaw: case ActionType::EnableRaw: case ActionType::DisableRaw:
        case ActionType::ToggleTrack: case ActionType::ToggleTrackCurrent:
        case ActionType::TrackCurrent: case ActionType::UntrackCurrent:
        case ActionType::Jump: case ActionType::JumpAccept:
        case ActionType::PrevHistory: case ActionType::NextHistory:
        case ActionType::Exclude: case ActionType::ExcludeMulti:
        case ActionType::ChangeHeaderLines: case ActionType::TransformHeaderLines: case ActionType::BgTransformHeaderLines:
        case ActionType::ChangeNth: case ActionType::TransformNth: case ActionType::BgTransformNth:
        case ActionType::ChangeWithNth: case ActionType::TransformWithNth: case ActionType::BgTransformWithNth:
        case ActionType::ChangeFooter: case ActionType::TransformFooter: case ActionType::BgTransformFooter:
        case ActionType::ChangeInputLabel: case ActionType::TransformInputLabel: case ActionType::BgTransformInputLabel:
        case ActionType::ChangeHeaderLabel: case ActionType::TransformHeaderLabel: case ActionType::BgTransformHeaderLabel:
        case ActionType::ChangeFooterLabel: case ActionType::TransformFooterLabel: case ActionType::BgTransformFooterLabel:
        case ActionType::ChangeListLabel: case ActionType::TransformListLabel: case ActionType::BgTransformListLabel:
        case ActionType::BgCancel: case ActionType::Wait: case ActionType::Refresh:
        case ActionType::Forward: case ActionType::Backward: case ActionType::ChangePreviewOneShot:
        case ActionType::BracketedPasteBegin: case ActionType::BracketedPasteEnd:
        default:
            // Accepted, no effect (docs/DESIGN.md non-goals and Tier 2 items).
            break;
    }

    // fzf: processExecution -- execute-* do not become the last action
    switch (a.type) {
        case ActionType::Execute: case ActionType::ExecuteSilent: case ActionType::ExecuteMulti:
        case ActionType::Become: case ActionType::Transform: case ActionType::BgTransform:
        case ActionType::Reload: case ActionType::ReloadSync: case ActionType::Preview:
            break;
        default:
            if (a.type >= ActionType::TransformBorderLabel && a.type <= ActionType::BgTransform) break;
            last_action_ = a.type;
            break;
    }

    if (inputless_) {
        // Always just discard the change
        input_ = current_input;
        cx_ = input_.size();
        beof_ = false;
    }
    return true;
}

} // namespace fzf
