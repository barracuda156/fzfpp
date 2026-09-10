// Port of fzf's key-chord / action-list parsing and default keymap.
// fzf: src/options.go (parseKeyChords, parseKeymap, parseActionList,
// maskActionContents, isExecuteAction, parseEveryEvent, parseToggleSort),
// src/terminal.go (defaultKeymap).

#include "keymap.hpp"
#include "options.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <regex>
#include <utf8.h>

namespace fzf {

namespace {

// fzf uses three unprintable runes to protect escaped separators while the
// spec is being split; same trick here.
constexpr char kEscapedColon = '\x00';
constexpr char kEscapedComma = '\x01';
constexpr char kEscapedPlus = '\x02';

[[noreturn]] void fail(const std::string& message) { throw OptionError{message}; }

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has_prefix(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
bool has_suffix(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}
bool is_alpha(char c) { return c >= 'a' && c <= 'z'; }

std::u32string to_u32(const std::string& s) {
    std::u32string out;
    try { utf8::utf8to32(s.begin(), s.end(), std::back_inserter(out)); }
    catch (...) { for (unsigned char c : s) out.push_back(c); }
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); } else cur += c;
    }
    out.push_back(cur);
    return out;
}

// fzf: parseEveryEvent
Event parse_every_event(const std::string& arg) {
    std::string s = arg;
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    if (a == std::string::npos) fail("every() requires a positive number of seconds");
    s = s.substr(a, b - a + 1);
    char* end = nullptr;
    double secs = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || std::isnan(secs) || std::isinf(secs) || secs <= 0) {
        fail("every() requires a positive number of seconds");
    }
    if (secs < 0.01) secs = 0.01;
    double ms = std::round(secs * 1000);
    if (ms > 2147483647.0) fail("every() interval is too large");
    return Event{EventType::Every, static_cast<char32_t>(static_cast<int32_t>(ms))};
}

struct NamedEvent { const char* name; EventType type; };

// fzf: parseKeyChords switch, minus the cases that need a rune payload.
const NamedEvent kNamedEvents[] = {
    {"up", EventType::Up}, {"down", EventType::Down}, {"left", EventType::Left}, {"right", EventType::Right},
    {"enter", EventType::Enter}, {"return", EventType::Enter},
    {"backspace", EventType::Backspace}, {"bspace", EventType::Backspace}, {"bs", EventType::Backspace},
    {"ctrl-space", EventType::CtrlSpace}, {"ctrl-^", EventType::CtrlCaret}, {"ctrl-6", EventType::CtrlCaret},
    {"ctrl-/", EventType::CtrlSlash}, {"ctrl-_", EventType::CtrlSlash},
    {"ctrl-\\", EventType::CtrlBackSlash}, {"ctrl-]", EventType::CtrlRightBracket},
    {"change", EventType::Change}, {"backward-eof", EventType::BackwardEOF}, {"start", EventType::Start},
    {"load", EventType::Load}, {"focus", EventType::Focus}, {"result", EventType::Result},
    {"result-final", EventType::ResultFinal}, {"resize", EventType::Resize}, {"one", EventType::One},
    {"zero", EventType::Zero}, {"jump", EventType::Jump}, {"jump-cancel", EventType::JumpCancel},
    {"click-header", EventType::ClickHeader}, {"click-footer", EventType::ClickFooter}, {"multi", EventType::Multi},
    {"alt-bs", EventType::AltBackspace}, {"alt-bspace", EventType::AltBackspace}, {"alt-backspace", EventType::AltBackspace},
    {"ctrl-bs", EventType::CtrlBackspace}, {"ctrl-bspace", EventType::CtrlBackspace}, {"ctrl-backspace", EventType::CtrlBackspace},
    {"ctrl-alt-bs", EventType::CtrlAltBackspace}, {"ctrl-alt-bspace", EventType::CtrlAltBackspace}, {"ctrl-alt-backspace", EventType::CtrlAltBackspace},
    {"alt-up", EventType::AltUp}, {"alt-down", EventType::AltDown}, {"alt-left", EventType::AltLeft}, {"alt-right", EventType::AltRight},
    {"alt-home", EventType::AltHome}, {"alt-end", EventType::AltEnd}, {"alt-delete", EventType::AltDelete},
    {"alt-page-up", EventType::AltPageUp}, {"alt-page-down", EventType::AltPageDown},
    {"tab", EventType::Tab}, {"btab", EventType::ShiftTab}, {"shift-tab", EventType::ShiftTab}, {"esc", EventType::Esc},
    {"delete", EventType::Delete}, {"del", EventType::Delete}, {"home", EventType::Home}, {"end", EventType::End},
    {"insert", EventType::Insert}, {"pgup", EventType::PageUp}, {"page-up", EventType::PageUp},
    {"pgdn", EventType::PageDown}, {"page-down", EventType::PageDown},
    {"alt-shift-up", EventType::AltShiftUp}, {"shift-alt-up", EventType::AltShiftUp},
    {"alt-shift-down", EventType::AltShiftDown}, {"shift-alt-down", EventType::AltShiftDown},
    {"alt-shift-left", EventType::AltShiftLeft}, {"shift-alt-left", EventType::AltShiftLeft},
    {"alt-shift-right", EventType::AltShiftRight}, {"shift-alt-right", EventType::AltShiftRight},
    {"alt-shift-home", EventType::AltShiftHome}, {"shift-alt-home", EventType::AltShiftHome},
    {"alt-shift-end", EventType::AltShiftEnd}, {"shift-alt-end", EventType::AltShiftEnd},
    {"alt-shift-delete", EventType::AltShiftDelete}, {"shift-alt-delete", EventType::AltShiftDelete},
    {"alt-shift-page-up", EventType::AltShiftPageUp}, {"shift-alt-page-up", EventType::AltShiftPageUp},
    {"alt-shift-page-down", EventType::AltShiftPageDown}, {"shift-alt-page-down", EventType::AltShiftPageDown},
    {"ctrl-up", EventType::CtrlUp}, {"ctrl-down", EventType::CtrlDown}, {"ctrl-right", EventType::CtrlRight},
    {"ctrl-left", EventType::CtrlLeft}, {"ctrl-home", EventType::CtrlHome}, {"ctrl-end", EventType::CtrlEnd},
    {"ctrl-delete", EventType::CtrlDelete}, {"ctrl-page-up", EventType::CtrlPageUp}, {"ctrl-page-down", EventType::CtrlPageDown},
    {"ctrl-alt-up", EventType::CtrlAltUp}, {"alt-ctrl-up", EventType::CtrlAltUp},
    {"ctrl-alt-down", EventType::CtrlAltDown}, {"alt-ctrl-down", EventType::CtrlAltDown},
    {"ctrl-alt-right", EventType::CtrlAltRight}, {"alt-ctrl-right", EventType::CtrlAltRight},
    {"ctrl-alt-left", EventType::CtrlAltLeft}, {"alt-ctrl-left", EventType::CtrlAltLeft},
    {"ctrl-alt-home", EventType::CtrlAltHome}, {"alt-ctrl-home", EventType::CtrlAltHome},
    {"ctrl-alt-end", EventType::CtrlAltEnd}, {"alt-ctrl-end", EventType::CtrlAltEnd},
    {"ctrl-alt-delete", EventType::CtrlAltDelete}, {"alt-ctrl-delete", EventType::CtrlAltDelete},
    {"ctrl-alt-page-up", EventType::CtrlAltPageUp}, {"alt-ctrl-page-up", EventType::CtrlAltPageUp},
    {"ctrl-alt-page-down", EventType::CtrlAltPageDown}, {"alt-ctrl-page-down", EventType::CtrlAltPageDown},
    {"ctrl-shift-up", EventType::CtrlShiftUp}, {"shift-ctrl-up", EventType::CtrlShiftUp},
    {"ctrl-shift-down", EventType::CtrlShiftDown}, {"shift-ctrl-down", EventType::CtrlShiftDown},
    {"ctrl-shift-right", EventType::CtrlShiftRight}, {"shift-ctrl-right", EventType::CtrlShiftRight},
    {"ctrl-shift-left", EventType::CtrlShiftLeft}, {"shift-ctrl-left", EventType::CtrlShiftLeft},
    {"ctrl-shift-home", EventType::CtrlShiftHome}, {"shift-ctrl-home", EventType::CtrlShiftHome},
    {"ctrl-shift-end", EventType::CtrlShiftEnd}, {"shift-ctrl-end", EventType::CtrlShiftEnd},
    {"ctrl-shift-delete", EventType::CtrlShiftDelete}, {"shift-ctrl-delete", EventType::CtrlShiftDelete},
    {"ctrl-shift-page-up", EventType::CtrlShiftPageUp}, {"shift-ctrl-page-up", EventType::CtrlShiftPageUp},
    {"ctrl-shift-page-down", EventType::CtrlShiftPageDown}, {"shift-ctrl-page-down", EventType::CtrlShiftPageDown},
    {"ctrl-alt-shift-up", EventType::CtrlAltShiftUp}, {"ctrl-alt-shift-down", EventType::CtrlAltShiftDown},
    {"ctrl-alt-shift-right", EventType::CtrlAltShiftRight}, {"ctrl-alt-shift-left", EventType::CtrlAltShiftLeft},
    {"ctrl-alt-shift-home", EventType::CtrlAltShiftHome}, {"ctrl-alt-shift-end", EventType::CtrlAltShiftEnd},
    {"ctrl-alt-shift-delete", EventType::CtrlAltShiftDelete}, {"ctrl-alt-shift-page-up", EventType::CtrlAltShiftPageUp},
    {"ctrl-alt-shift-page-down", EventType::CtrlAltShiftPageDown},
    {"shift-up", EventType::ShiftUp}, {"shift-down", EventType::ShiftDown}, {"shift-left", EventType::ShiftLeft},
    {"shift-right", EventType::ShiftRight}, {"shift-home", EventType::ShiftHome}, {"shift-end", EventType::ShiftEnd},
    {"shift-delete", EventType::ShiftDelete}, {"shift-page-up", EventType::ShiftPageUp}, {"shift-page-down", EventType::ShiftPageDown},
    {"left-click", EventType::LeftClick}, {"right-click", EventType::RightClick},
    {"shift-left-click", EventType::SLeftClick}, {"shift-right-click", EventType::SRightClick},
    {"double-click", EventType::DoubleClick}, {"scroll-up", EventType::ScrollUp}, {"scroll-down", EventType::ScrollDown},
    {"shift-scroll-up", EventType::SScrollUp}, {"shift-scroll-down", EventType::SScrollDown},
    {"preview-scroll-up", EventType::PreviewScrollUp}, {"preview-scroll-down", EventType::PreviewScrollDown},
    {"f10", EventType::F10}, {"f11", EventType::F11}, {"f12", EventType::F12},
};

struct NamedAction { const char* name; ActionType type; };

// fzf: parseActionList switch (simple actions; toggle-down/up expand to two).
const NamedAction kNamedActions[] = {
    {"ignore", ActionType::Ignore}, {"beginning-of-line", ActionType::BeginningOfLine},
    {"abort", ActionType::Abort}, {"accept", ActionType::Accept}, {"accept-non-empty", ActionType::AcceptNonEmpty},
    {"accept-or-print-query", ActionType::AcceptOrPrintQuery}, {"print-query", ActionType::PrintQuery},
    {"refresh-preview", ActionType::RefreshPreview}, {"replace-query", ActionType::ReplaceQuery},
    {"backward-char", ActionType::BackwardChar}, {"backward-delete-char", ActionType::BackwardDeleteChar},
    {"backward-delete-char/eof", ActionType::BackwardDeleteCharEof}, {"backward-word", ActionType::BackwardWord},
    {"backward-subword", ActionType::BackwardSubWord}, {"clear-screen", ActionType::ClearScreen},
    {"delete-char", ActionType::DeleteChar}, {"delete-char/eof", ActionType::DeleteCharEof},
    {"deselect", ActionType::Deselect}, {"end-of-line", ActionType::EndOfLine}, {"cancel", ActionType::Cancel},
    {"clear-query", ActionType::ClearQuery}, {"clear-multi", ActionType::ClearSelection},
    {"clear-selection", ActionType::ClearSelection}, {"forward-char", ActionType::ForwardChar},
    {"forward-word", ActionType::ForwardWord}, {"forward-subword", ActionType::ForwardSubWord},
    {"jump", ActionType::Jump}, {"jump-accept", ActionType::JumpAccept}, {"kill-line", ActionType::KillLine},
    {"kill-word", ActionType::KillWord}, {"kill-subword", ActionType::KillSubWord},
    {"unix-line-discard", ActionType::UnixLineDiscard}, {"line-discard", ActionType::UnixLineDiscard},
    {"unix-word-rubout", ActionType::UnixWordRubout}, {"word-rubout", ActionType::UnixWordRubout},
    {"yank", ActionType::Yank}, {"backward-kill-word", ActionType::BackwardKillWord},
    {"backward-kill-subword", ActionType::BackwardKillSubWord},
    {"toggle-in", ActionType::ToggleIn}, {"toggle-out", ActionType::ToggleOut}, {"toggle-all", ActionType::ToggleAll},
    {"toggle-search", ActionType::ToggleSearch}, {"toggle-track", ActionType::ToggleTrack},
    {"toggle-track-current", ActionType::ToggleTrackCurrent}, {"toggle-input", ActionType::ToggleInput},
    {"hide-input", ActionType::HideInput}, {"show-input", ActionType::ShowInput},
    {"toggle-header", ActionType::ToggleHeader}, {"toggle-wrap", ActionType::ToggleWrap},
    {"toggle-wrap-word", ActionType::ToggleWrapWord}, {"toggle-multi-line", ActionType::ToggleMultiLine},
    {"toggle-hscroll", ActionType::ToggleHscroll}, {"toggle-raw", ActionType::ToggleRaw},
    {"enable-raw", ActionType::EnableRaw}, {"disable-raw", ActionType::DisableRaw},
    {"show-header", ActionType::ShowHeader}, {"hide-header", ActionType::HideHeader},
    {"track", ActionType::TrackCurrent}, {"track-current", ActionType::TrackCurrent},
    {"untrack-current", ActionType::UntrackCurrent}, {"select", ActionType::Select},
    {"select-all", ActionType::SelectAll}, {"deselect-all", ActionType::DeselectAll}, {"close", ActionType::Close},
    {"toggle", ActionType::Toggle}, {"down", ActionType::Down}, {"down-match", ActionType::DownMatch},
    {"up", ActionType::Up}, {"up-match", ActionType::UpMatch}, {"first", ActionType::First}, {"top", ActionType::First},
    {"last", ActionType::Last}, {"best", ActionType::Best}, {"page-up", ActionType::PageUp},
    {"page-down", ActionType::PageDown}, {"half-page-up", ActionType::HalfPageUp},
    {"half-page-down", ActionType::HalfPageDown}, {"prev-history", ActionType::PrevHistory},
    {"previous-history", ActionType::PrevHistory}, {"next-history", ActionType::NextHistory},
    {"up-selected", ActionType::PrevSelected}, {"prev-selected", ActionType::PrevSelected},
    {"down-selected", ActionType::NextSelected}, {"next-selected", ActionType::NextSelected},
    {"show-preview", ActionType::ShowPreview}, {"hide-preview", ActionType::HidePreview},
    {"toggle-preview", ActionType::TogglePreview}, {"toggle-preview-wrap", ActionType::TogglePreviewWrap},
    {"toggle-preview-wrap-word", ActionType::TogglePreviewWrapWord}, {"toggle-sort", ActionType::ToggleSort},
    {"offset-up", ActionType::OffsetUp}, {"offset-down", ActionType::OffsetDown},
    {"offset-middle", ActionType::OffsetMiddle}, {"preview-top", ActionType::PreviewTop},
    {"preview-bottom", ActionType::PreviewBottom}, {"preview-up", ActionType::PreviewUp},
    {"preview-down", ActionType::PreviewDown}, {"preview-page-up", ActionType::PreviewPageUp},
    {"preview-page-down", ActionType::PreviewPageDown}, {"preview-half-page-up", ActionType::PreviewHalfPageUp},
    {"preview-half-page-down", ActionType::PreviewHalfPageDown}, {"enable-search", ActionType::EnableSearch},
    {"disable-search", ActionType::DisableSearch}, {"wait", ActionType::Wait}, {"bell", ActionType::Bell},
    {"exclude", ActionType::Exclude}, {"exclude-multi", ActionType::ExcludeMulti}, {"bg-cancel", ActionType::BgCancel},
};

// fzf: isExecuteAction prefix table (actions that take an argument).
const NamedAction kArgActions[] = {
    {"become", ActionType::Become}, {"reload", ActionType::Reload}, {"reload-sync", ActionType::ReloadSync},
    {"unbind", ActionType::Unbind}, {"rebind", ActionType::Rebind}, {"toggle-bind", ActionType::ToggleBind},
    {"preview", ActionType::Preview}, {"change-header", ActionType::ChangeHeader},
    {"change-header-lines", ActionType::ChangeHeaderLines}, {"change-footer", ActionType::ChangeFooter},
    {"change-list-label", ActionType::ChangeListLabel}, {"change-border-label", ActionType::ChangeBorderLabel},
    {"change-preview-label", ActionType::ChangePreviewLabel}, {"change-input-label", ActionType::ChangeInputLabel},
    {"change-header-label", ActionType::ChangeHeaderLabel}, {"change-footer-label", ActionType::ChangeFooterLabel},
    {"change-ghost", ActionType::ChangeGhost}, {"change-pointer", ActionType::ChangePointer},
    {"change-preview-window", ActionType::ChangePreviewWindow}, {"change-preview", ActionType::ChangePreview},
    {"change-prompt", ActionType::ChangePrompt}, {"change-query", ActionType::ChangeQuery},
    {"change-multi", ActionType::ChangeMulti}, {"change-nth", ActionType::ChangeNth},
    {"change-with-nth", ActionType::ChangeWithNth}, {"pos", ActionType::Position},
    {"execute", ActionType::Execute}, {"execute-silent", ActionType::ExecuteSilent},
    {"execute-multi", ActionType::ExecuteMulti}, {"print", ActionType::Print}, {"put", ActionType::Put},
    {"transform", ActionType::Transform}, {"transform-list-label", ActionType::TransformListLabel},
    {"transform-border-label", ActionType::TransformBorderLabel},
    {"transform-preview-label", ActionType::TransformPreviewLabel},
    {"transform-input-label", ActionType::TransformInputLabel},
    {"transform-header-label", ActionType::TransformHeaderLabel},
    {"transform-footer-label", ActionType::TransformFooterLabel}, {"transform-footer", ActionType::TransformFooter},
    {"transform-header", ActionType::TransformHeader}, {"transform-header-lines", ActionType::TransformHeaderLines},
    {"transform-ghost", ActionType::TransformGhost}, {"transform-nth", ActionType::TransformNth},
    {"transform-with-nth", ActionType::TransformWithNth}, {"transform-pointer", ActionType::TransformPointer},
    {"transform-prompt", ActionType::TransformPrompt}, {"transform-query", ActionType::TransformQuery},
    {"transform-search", ActionType::TransformSearch}, {"bg-transform", ActionType::BgTransform},
    {"bg-transform-list-label", ActionType::BgTransformListLabel},
    {"bg-transform-border-label", ActionType::BgTransformBorderLabel},
    {"bg-transform-preview-label", ActionType::BgTransformPreviewLabel},
    {"bg-transform-input-label", ActionType::BgTransformInputLabel},
    {"bg-transform-header-label", ActionType::BgTransformHeaderLabel},
    {"bg-transform-footer-label", ActionType::BgTransformFooterLabel},
    {"bg-transform-footer", ActionType::BgTransformFooter}, {"bg-transform-header", ActionType::BgTransformHeader},
    {"bg-transform-header-lines", ActionType::BgTransformHeaderLines},
    {"bg-transform-ghost", ActionType::BgTransformGhost}, {"bg-transform-nth", ActionType::BgTransformNth},
    {"bg-transform-with-nth", ActionType::BgTransformWithNth},
    {"bg-transform-pointer", ActionType::BgTransformPointer}, {"bg-transform-prompt", ActionType::BgTransformPrompt},
    {"bg-transform-query", ActionType::BgTransformQuery}, {"bg-transform-search", ActionType::BgTransformSearch},
    {"trigger", ActionType::Trigger}, {"search", ActionType::Search},
};

// fzf: argActionRegexp -- the action names that take an argument, preceded
// by ':' or '+'. Case-insensitive.
const std::regex& arg_action_regex() {
    static const std::regex re(
        "[:+](become|execute(?:-multi|-silent)?|reload(?:-sync)?|preview|"
        "(?:change|bg-transform|transform)-(?:query|prompt|(?:border|list|preview|input|header|footer)-label|"
        "header-lines|header|footer|search|with-nth|nth|pointer|ghost)|bg-transform|transform|"
        "change-(?:preview-window|preview|multi)|(?:re|un|toggle-)bind|pos|put|print|search|trigger)",
        std::regex::ECMAScript | std::regex::icase);
    return re;
}

// fzf: actionNameRegexp "(?i)^[a-z-]+"
size_t action_name_length(const std::string& s) {
    size_t n = 0;
    while (n < s.size()) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[n])));
        if ((c >= 'a' && c <= 'z') || c == '-') ++n; else break;
    }
    return n;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
        s.replace(p, from.size(), to);
    }
}

// fzf: isExecuteAction
ActionType arg_action_type(const std::string& str) {
    std::string masked = mask_action_contents(":" + str).substr(1);
    if (masked == str) return ActionType::Ignore;   // not masked: not an arg action
    std::string prefix = to_lower(str.substr(0, action_name_length(str)));
    for (const auto& a : kArgActions) {
        if (prefix == a.name) return a.type;
    }
    return ActionType::Ignore;
}

// fzf: parseActionList
ActionList parse_action_list(const std::string& masked, const std::string& original,
                             const ActionList& prev_actions, bool put_allowed) {
    auto masked_parts = split(masked, '+');
    std::vector<std::string> original_parts(masked_parts.size());
    size_t idx = 0;
    for (size_t i = 0; i < masked_parts.size(); ++i) {
        original_parts[i] = original.substr(idx, masked_parts[i].size());
        idx += masked_parts[i].size() + 1;
    }

    ActionList actions;
    std::string prev_spec;
    for (size_t spec_index = 0; spec_index < original_parts.size(); ++spec_index) {
        std::string spec = prev_spec + original_parts[spec_index];
        std::string lower = to_lower(spec);

        bool matched = false;
        if (lower == "toggle-down") { actions.push_back({ActionType::Toggle, ""}); actions.push_back({ActionType::Down, ""}); matched = true; }
        else if (lower == "toggle-up") { actions.push_back({ActionType::Toggle, ""}); actions.push_back({ActionType::Up, ""}); matched = true; }
        else if (lower == "put") {
            if (!put_allowed) fail("unable to put non-printable character");
            actions.push_back({ActionType::Char, ""});
            matched = true;
        } else {
            for (const auto& a : kNamedActions) {
                if (lower == a.name) { actions.push_back({a.type, ""}); matched = true; break; }
            }
        }
        if (matched) { prev_spec.clear(); continue; }

        ActionType t = arg_action_type(lower);
        if (t == ActionType::Ignore) {
            if (spec_index == 0 && lower.empty()) {
                // `key:+action` appends to the existing binding
                actions.insert(actions.begin(), prev_actions.begin(), prev_actions.end());
            } else if (lower == "change-multi") {
                actions.push_back({ActionType::ChangeMulti, ""});
            } else {
                fail("unknown action: " + spec);
            }
            prev_spec.clear();
            continue;
        }

        size_t offset = action_name_length(spec);
        std::string action_arg;
        if (offset < spec.size() && spec[offset] == ':') {
            if (spec_index == original_parts.size() - 1) {
                action_arg = spec.substr(offset + 1);
                actions.push_back({t, action_arg});
            } else {
                // The argument extends to the end of the string; the '+'
                // split cut it, so glue the next part back on.
                prev_spec = spec + "+";
                continue;
            }
        } else {
            if (spec.size() < offset + 2) fail("unknown action: " + spec);
            action_arg = spec.substr(offset + 1, spec.size() - offset - 2);
            actions.push_back({t, action_arg});
        }
        if (t == ActionType::Unbind || t == ActionType::Rebind || t == ActionType::ToggleBind) {
            parse_key_chords(action_arg, spec.substr(0, offset) + " target required");
        } else if (t == ActionType::ChangePreviewWindow) {
            // Each '|' alternative must be a valid --preview-window spec.
            Options probe;
            for (const auto& alt : split(action_arg, '|')) {
                std::vector<std::string> args{"--preview-window", alt};
                parse_option_args(args, false);
            }
        }
        prev_spec.clear();
    }
    return actions;
}

} // namespace

bool Event::printable() const {
    return type == EventType::Rune && ch >= 0x20 && ch != 0x7f && !(ch >= 0x80 && ch < 0xa0);
}

// fzf: maskActionContents
std::string mask_action_contents(const std::string& action_in) {
    std::string masked;
    std::string action = action_in;
    while (!action.empty()) {
        std::smatch m;
        if (!std::regex_search(action, m, arg_action_regex())) {
            masked += action;
            break;
        }
        size_t end = static_cast<size_t>(m.position(0) + m.length(0));
        masked += action.substr(0, end);
        action = action.substr(end);
        if (action.empty()) break;
        char cs = action[0];
        char ce;
        switch (cs) {
            case ':':
                masked += std::string(action.size(), ' ');
                action.clear();
                continue;
            case '(': ce = ')'; break;
            case '{': ce = '}'; break;
            case '[': ce = ']'; break;
            case '<': ce = '>'; break;
            case '~': case '!': case '@': case '#': case '$': case '%': case '^':
            case '&': case '*': case ';': case '/': case '|':
                ce = cs; break;
            default:
                continue;
        }
        if (action.empty()) break;
        // Non-greedy: the first closer that is followed by '+', ',' or the end.
        size_t close = std::string::npos;
        for (size_t i = 1; i < action.size(); ++i) {
            if (action[i] != ce) continue;
            if (i + 1 == action.size() || action[i + 1] == '+' || action[i + 1] == ',') { close = i; break; }
        }
        if (close == std::string::npos) {
            masked += action;
            break;
        }
        size_t len = close + 1;
        masked += std::string(len, ' ');
        action = action.substr(len);
    }
    replace_all(masked, ",,,", std::string(",") + kEscapedComma + ",");
    replace_all(masked, ",:,", std::string(",") + kEscapedColon + ",");
    replace_all(masked, "::", std::string(1, kEscapedColon) + ":");
    replace_all(masked, ",:", std::string(1, kEscapedComma) + ":");
    replace_all(masked, "+:", std::string(1, kEscapedPlus) + ":");
    return masked;
}

// fzf: parseKeyChords
std::vector<Event> parse_key_chords(const std::string& str_in, const std::string& message,
                                    std::map<Event, std::string>* names) {
    if (str_in.empty()) fail(message);
    std::vector<Event> list;

    // "alt-," is a key; protect its comma before splitting.
    std::string str = str_in;
    {
        std::string out;
        for (size_t i = 0; i < str.size(); ++i) {
            if (i + 4 < str.size() && to_lower(str.substr(i, 4)) == "alt-" && str[i + 4] == ',') {
                out += str.substr(i, 4);
                out += kEscapedComma;
                i += 4;
            } else {
                out += str[i];
            }
        }
        str = out;
    }
    auto tokens = split(str, ',');
    if (str == "," || has_prefix(str, ",,") || has_suffix(str, ",,") || str.find(",,,") != std::string::npos) {
        tokens.push_back(",");
    }

    auto add = [&](Event e, const std::string& name) {
        if (names) (*names)[e] = name;
        list.push_back(e);
    };

    for (std::string key : tokens) {
        if (key.empty()) continue;
        replace_all(key, std::string(1, kEscapedComma), ",");
        std::string lkey = to_lower(key);

        bool found = false;
        if (lkey == "space") { add(rune_event(U' '), key); found = true; }
        else if (lkey == "alt-enter" || lkey == "alt-return") { add(ctrl_alt_event(U'm'), key); found = true; }
        else if (lkey == "alt-space") { add(alt_event(U' '), key); found = true; }
        else {
            for (const auto& ne : kNamedEvents) {
                if (lkey == ne.name) { add(event_of(ne.type), key); found = true; break; }
            }
        }
        if (found) continue;

        std::u32string runes = to_u32(key);
        if (has_prefix(lkey, "every(") && has_suffix(lkey, ")")) {
            add(parse_every_event(key.substr(6, key.size() - 7)), key);
        } else if (key.size() == 10 && has_prefix(lkey, "ctrl-alt-") && is_alpha(lkey[9])) {
            char32_t r = static_cast<char32_t>(lkey[9]);
            Event evt = (r == U'h') ? event_of(EventType::CtrlAltBackspace) : ctrl_alt_event(r);
            add(evt, key);
        } else if (key.size() == 6 && has_prefix(lkey, "ctrl-") && is_alpha(lkey[5])) {
            char r = lkey[5];
            EventType et = (r == 'h') ? EventType::CtrlBackspace
                : static_cast<EventType>(static_cast<int>(EventType::CtrlA) + (r - 'a'));
            add(event_of(et), key);
        } else if (runes.size() == 5 && has_prefix(lkey, "alt-")) {
            char32_t r = runes[4];
            if (r == static_cast<char32_t>(kEscapedColon)) r = U':';
            else if (r == static_cast<char32_t>(kEscapedComma)) r = U',';
            else if (r == static_cast<char32_t>(kEscapedPlus)) r = U'+';
            add(alt_event(r), key);
        } else if (key.size() == 2 && lkey[0] == 'f' && key[1] >= '1' && key[1] <= '9') {
            add(event_of(static_cast<EventType>(static_cast<int>(EventType::F1) + (key[1] - '1'))), key);
        } else if (runes.size() == 1) {
            add(rune_event(runes[0]), key);
        } else {
            fail("unsupported key: " + key);
        }
    }
    return list;
}

// fzf: parseKeymap
void parse_keymap(Keymap& keymap, const std::string& str) {
    std::string masked = mask_action_contents(str);
    size_t idx = 0;
    std::vector<std::string> keys;
    for (const auto& pair_masked : split(masked, ',')) {
        std::string orig_pair = str.substr(idx, pair_masked.size());
        idx += pair_masked.size() + 1;

        size_t colon = pair_masked.find(':');
        std::string key_name = colon == std::string::npos ? pair_masked : pair_masked.substr(0, colon);
        if (key_name.empty()) fail("key name required");
        keys.push_back(key_name);
        if (colon == std::string::npos) continue;

        std::string masked_actions = pair_masked.substr(colon + 1);
        std::string orig_actions = orig_pair.substr(key_name.size() + 1);
        for (const auto& kn : keys) {
            Event key;
            if (kn.size() == 1 && kn[0] == kEscapedColon) key = rune_event(U':');
            else if (kn.size() == 1 && kn[0] == kEscapedComma) key = rune_event(U',');
            else if (kn.size() == 1 && kn[0] == kEscapedPlus) key = rune_event(U'+');
            else {
                auto events = parse_key_chords(kn, "key name required");
                key = events.front();
            }
            ActionList prev = keymap.count(key) ? keymap[key] : ActionList{};
            keymap[key] = parse_action_list(masked_actions, orig_actions, prev, key.printable());
        }
        keys.clear();
    }
    if (!keys.empty()) {
        std::string joined;
        for (size_t i = 0; i < keys.size(); ++i) joined += (i ? ", " : "") + keys[i];
        fail("bind action not specified: " + joined);
    }
}

// fzf: parseSingleActionList
ActionList parse_single_action_list(const std::string& str, bool put_allowed) {
    std::string masked = mask_action_contents(":" + str).substr(1);
    return parse_action_list(masked, str, {}, put_allowed);
}

// fzf: terminal.go defaultKeymap
Keymap default_keymap() {
    Keymap k;
    auto add = [&](EventType e, ActionType a) { k[event_of(e)] = {{a, ""}}; };
    auto add_event = [&](Event e, ActionType a) { k[e] = {{a, ""}}; };
    add(EventType::Fatal, ActionType::Fatal);
    add(EventType::Invalid, ActionType::Invalid);
    add(EventType::BracketedPasteBegin, ActionType::BracketedPasteBegin);
    add(EventType::BracketedPasteEnd, ActionType::BracketedPasteEnd);
    add(EventType::CtrlA, ActionType::BeginningOfLine);
    add(EventType::CtrlB, ActionType::BackwardChar);
    add(EventType::CtrlC, ActionType::Abort);
    add(EventType::CtrlG, ActionType::Abort);
    add(EventType::CtrlQ, ActionType::Abort);
    add(EventType::Esc, ActionType::Abort);
    add(EventType::CtrlD, ActionType::DeleteCharEof);
    add(EventType::CtrlE, ActionType::EndOfLine);
    add(EventType::CtrlF, ActionType::ForwardChar);
    add(EventType::Backspace, ActionType::BackwardDeleteChar);
    add(EventType::CtrlBackspace, ActionType::BackwardDeleteChar);
    k[event_of(EventType::Tab)] = {{ActionType::Toggle, ""}, {ActionType::Down, ""}};
    k[event_of(EventType::ShiftTab)] = {{ActionType::Toggle, ""}, {ActionType::Up, ""}};
    add(EventType::CtrlJ, ActionType::Down);
    add(EventType::CtrlK, ActionType::Up);
    add(EventType::CtrlL, ActionType::ClearScreen);
    add(EventType::Enter, ActionType::Accept);
    add(EventType::CtrlN, ActionType::DownMatch);
    add(EventType::CtrlP, ActionType::UpMatch);
    add(EventType::AltDown, ActionType::DownMatch);
    add(EventType::AltUp, ActionType::UpMatch);
    add(EventType::CtrlU, ActionType::UnixLineDiscard);
    add(EventType::CtrlW, ActionType::UnixWordRubout);
    add(EventType::CtrlY, ActionType::Yank);
    add(EventType::CtrlZ, ActionType::SigStop);
    add(EventType::CtrlSlash, ActionType::ToggleWrapWord);
    add_event(alt_event(U'/'), ActionType::ToggleWrapWord);
    add_event(alt_event(U'b'), ActionType::BackwardWord);
    add(EventType::ShiftLeft, ActionType::BackwardWord);
    add(EventType::AltLeft, ActionType::BackwardWord);
    add_event(alt_event(U'f'), ActionType::ForwardWord);
    add(EventType::ShiftRight, ActionType::ForwardWord);
    add(EventType::AltRight, ActionType::ForwardWord);
    add_event(alt_event(U'd'), ActionType::KillWord);
    add(EventType::AltBackspace, ActionType::BackwardKillWord);
    add(EventType::Up, ActionType::Up);
    add(EventType::Down, ActionType::Down);
    add(EventType::Left, ActionType::BackwardChar);
    add(EventType::Right, ActionType::ForwardChar);
    add(EventType::Home, ActionType::BeginningOfLine);
    add(EventType::End, ActionType::EndOfLine);
    add(EventType::Delete, ActionType::DeleteChar);
    add(EventType::PageUp, ActionType::PageUp);
    add(EventType::PageDown, ActionType::PageDown);
    add(EventType::ShiftUp, ActionType::PreviewUp);
    add(EventType::ShiftDown, ActionType::PreviewDown);
    add(EventType::Mouse, ActionType::Mouse);
    add(EventType::LeftClick, ActionType::Click);
    add(EventType::RightClick, ActionType::Toggle);
    add(EventType::SLeftClick, ActionType::Toggle);
    add(EventType::SRightClick, ActionType::Toggle);
    add(EventType::ScrollUp, ActionType::Up);
    add(EventType::ScrollDown, ActionType::Down);
    k[event_of(EventType::SScrollUp)] = {{ActionType::Toggle, ""}, {ActionType::Up, ""}};
    k[event_of(EventType::SScrollDown)] = {{ActionType::Toggle, ""}, {ActionType::Down, ""}};
    add(EventType::PreviewScrollUp, ActionType::PreviewUp);
    add(EventType::PreviewScrollDown, ActionType::PreviewDown);
    return k;
}

// fzf: the keymap part of parseOptions (--bind, --toggle-sort) and
// postProcessOptions.
Keymap build_keymap(Options& opts) {
    Keymap user;
    for (const auto& spec : opts.bind_specs) parse_keymap(user, spec);
    for (const auto& spec : opts.toggle_sort_specs) {
        auto events = parse_key_chords(spec, "key name required");
        if (events.size() != 1) fail("multiple keys specified");
        user[events.front()] = {{ActionType::ToggleSort, ""}};
    }

    // --history: ctrl-n/p navigate history unless bound
    if (opts.history) {
        if (!user.count(event_of(EventType::CtrlP))) user[event_of(EventType::CtrlP)] = {{ActionType::PrevHistory, ""}};
        if (!user.count(event_of(EventType::CtrlN))) user[event_of(EventType::CtrlN)] = {{ActionType::NextHistory, ""}};
    }

    Keymap keymap = default_keymap();
    opts.toggle_sort = false;
    for (auto& [key, actions] : user) {
        ActionList reordered;
        for (const auto& act : actions) {
            switch (act.type) {
                case ActionType::ToggleSort: opts.toggle_sort = true; break;
                case ActionType::TogglePreview: case ActionType::ShowPreview:
                case ActionType::HidePreview: case ActionType::ChangePreviewWindow:
                    reordered.push_back(act); break;
                default: break;
            }
        }
        // Preview-window changes go first so a following preview(...) sees them.
        if (!reordered.empty()) {
            for (const auto& act : actions) {
                switch (act.type) {
                    case ActionType::TogglePreview: case ActionType::ShowPreview:
                    case ActionType::HidePreview: case ActionType::ChangePreviewWindow: break;
                    default: reordered.push_back(act);
                }
            }
            keymap[key] = reordered;
        } else {
            keymap[key] = actions;
        }
    }
    if (!keymap.count(event_of(EventType::DoubleClick))) {
        keymap[event_of(EventType::DoubleClick)] = keymap[event_of(EventType::Enter)];
    }
    return keymap;
}

std::map<Event, std::string> build_expect(const Options& opts) {
    std::map<Event, std::string> expect;
    for (const auto& spec : opts.expect_specs) {
        parse_key_chords(spec, "key names required", &expect);
    }
    return expect;
}

// KeyEvent (keyparser.cpp) -> Event. Control bytes arrive as Character
// events carrying the raw byte; named keys as Special events; alt-<char>
// as "ESC <char>".
Event to_event(const KeyEvent& ev) {
    if (ev.type == KeyType::Mouse) {
        const MouseInfo& m = ev.mouse;
        if (m.button == MouseInfo::Button::WheelUp) return event_of(m.shift ? EventType::SScrollUp : EventType::ScrollUp);
        if (m.button == MouseInfo::Button::WheelDown) return event_of(m.shift ? EventType::SScrollDown : EventType::ScrollDown);
        if (m.motion == MouseInfo::Motion::Pressed) {
            if (m.button == MouseInfo::Button::Left) return event_of(m.shift ? EventType::SLeftClick : EventType::LeftClick);
            if (m.button == MouseInfo::Button::Right) return event_of(m.shift ? EventType::SRightClick : EventType::RightClick);
        }
        return event_of(EventType::Mouse);
    }
    if (ev.type == KeyType::Resize) return event_of(EventType::Resize);

    if (ev.type == KeyType::Special) {
        switch (ev.special) {
            case SpecialKey::Return: return event_of(EventType::Enter);
            case SpecialKey::Escape: return event_of(EventType::Esc);
            case SpecialKey::Tab: return event_of(EventType::Tab);
            case SpecialKey::BackTab: return event_of(EventType::ShiftTab);
            case SpecialKey::Backspace: return event_of(EventType::Backspace);
            case SpecialKey::Delete: return event_of(EventType::Delete);
            case SpecialKey::ArrowUp: return event_of(EventType::Up);
            case SpecialKey::ArrowDown: return event_of(EventType::Down);
            case SpecialKey::ArrowLeft: return event_of(EventType::Left);
            case SpecialKey::ArrowRight: return event_of(EventType::Right);
            case SpecialKey::PageUp: return event_of(EventType::PageUp);
            case SpecialKey::PageDown: return event_of(EventType::PageDown);
            case SpecialKey::Home: return event_of(EventType::Home);
            case SpecialKey::End: return event_of(EventType::End);
            case SpecialKey::Insert: return event_of(EventType::Insert);
            case SpecialKey::F1: return event_of(EventType::F1);
            case SpecialKey::F2: return event_of(EventType::F2);
            case SpecialKey::F3: return event_of(EventType::F3);
            case SpecialKey::F4: return event_of(EventType::F4);
            case SpecialKey::F5: return event_of(EventType::F5);
            case SpecialKey::F6: return event_of(EventType::F6);
            case SpecialKey::F7: return event_of(EventType::F7);
            case SpecialKey::F8: return event_of(EventType::F8);
            case SpecialKey::F9: return event_of(EventType::F9);
            case SpecialKey::F10: return event_of(EventType::F10);
            case SpecialKey::F11: return event_of(EventType::F11);
            case SpecialKey::F12: return event_of(EventType::F12);
            case SpecialKey::ShiftArrowUp: return event_of(EventType::ShiftUp);
            case SpecialKey::ShiftArrowDown: return event_of(EventType::ShiftDown);
            case SpecialKey::ShiftArrowLeft: return event_of(EventType::ShiftLeft);
            case SpecialKey::ShiftArrowRight: return event_of(EventType::ShiftRight);
            case SpecialKey::CtrlArrowUp: return event_of(EventType::CtrlUp);
            case SpecialKey::CtrlArrowDown: return event_of(EventType::CtrlDown);
            case SpecialKey::CtrlArrowLeft: return event_of(EventType::CtrlLeft);
            case SpecialKey::CtrlArrowRight: return event_of(EventType::CtrlRight);
            case SpecialKey::AltArrowUp: return event_of(EventType::AltUp);
            case SpecialKey::AltArrowDown: return event_of(EventType::AltDown);
            case SpecialKey::AltArrowLeft: return event_of(EventType::AltLeft);
            case SpecialKey::AltArrowRight: return event_of(EventType::AltRight);
            default: break;
        }
        // Unrecognized escape sequence: fzf reports it as Invalid.
        return event_of(EventType::Invalid);
    }

    const std::string& in = ev.input;
    // alt-<char>: ESC followed by one (UTF-8) character; alt-backspace.
    if (in.size() >= 2 && in[0] == '\x1b') {
        unsigned char c1 = static_cast<unsigned char>(in[1]);
        if (c1 == 0x7f || c1 == 0x08) return event_of(EventType::AltBackspace);
        if (c1 >= 0x01 && c1 <= 0x1a) {
            // ctrl-alt-<letter>: ESC + control byte
            char32_t r = static_cast<char32_t>('a' + (c1 - 1));
            if (r == U'h') return event_of(EventType::CtrlAltBackspace);
            return ctrl_alt_event(r);
        }
        std::u32string rest = to_u32(in.substr(1));
        if (rest.size() == 1) return alt_event(rest[0]);
        return event_of(EventType::Invalid);
    }
    if (in.size() == 1) {
        unsigned char c = static_cast<unsigned char>(in[0]);
        if (c == 0x00) return event_of(EventType::CtrlSpace);
        if (c >= 0x01 && c <= 0x1a) {
            return event_of(static_cast<EventType>(static_cast<int>(EventType::CtrlA) + (c - 1)));
        }
        if (c == 0x1b) return event_of(EventType::Esc);
        if (c == 0x1c) return event_of(EventType::CtrlBackSlash);
        if (c == 0x1d) return event_of(EventType::CtrlRightBracket);
        if (c == 0x1e) return event_of(EventType::CtrlCaret);
        if (c == 0x1f) return event_of(EventType::CtrlSlash);
        if (c == 0x7f) return event_of(EventType::Backspace);
    }
    if (ev.is_character() && ev.codepoints.size() == 1) return rune_event(ev.codepoints[0]);
    if (ev.is_character() && !ev.codepoints.empty()) return rune_event(ev.codepoints[0]);
    return event_of(EventType::Invalid);
}

const char* action_name(ActionType t) {
    for (const auto& a : kNamedActions) if (a.type == t) return a.name;
    for (const auto& a : kArgActions) if (a.type == t) return a.name;
    switch (t) {
        case ActionType::Char: return "put";
        case ActionType::Click: return "click";
        case ActionType::Mouse: return "mouse";
        case ActionType::SigStop: return "sigstop";
        default: return "unknown";
    }
}

} // namespace fzf
