#pragma once

// Key events, actions and the keymap: a port of fzf's tui.Event /
// tui.EventType (src/tui/tui.go), actionType (src/terminal.go) and the
// --bind / --expect parsers (src/options.go parseKeyChords, parseKeymap,
// parseActionList, maskActionContents, isExecuteAction, defaultKeymap).
//
// Everything is parsed once at startup into structured values; the event
// loop and the action executor never look at bind strings again.

#include "keyevent.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace fzf {

struct Options;

// fzf: tui.EventType. The order matters in two places: CtrlA..CtrlZ are
// consecutive (ctrl-<letter> is computed arithmetically, with Tab and
// Enter standing in for ctrl-i / ctrl-m), and F1..F12 are consecutive.
enum class EventType : uint16_t {
    Rune,
    CtrlA, CtrlB, CtrlC, CtrlD, CtrlE, CtrlF, CtrlG, CtrlH, Tab, CtrlJ, CtrlK,
    CtrlL, Enter, CtrlN, CtrlO, CtrlP, CtrlQ, CtrlR, CtrlS, CtrlT, CtrlU, CtrlV,
    CtrlW, CtrlX, CtrlY, CtrlZ,
    Esc, CtrlSpace, CtrlBackSlash, CtrlRightBracket, CtrlCaret, CtrlSlash,
    ShiftTab, Backspace, Delete, PageUp, PageDown, Up, Down, Left, Right,
    Home, End, Insert,
    ShiftUp, ShiftDown, ShiftLeft, ShiftRight, ShiftDelete, ShiftHome, ShiftEnd,
    ShiftPageUp, ShiftPageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    AltBackspace, AltUp, AltDown, AltLeft, AltRight, AltDelete, AltHome, AltEnd,
    AltPageUp, AltPageDown,
    AltShiftUp, AltShiftDown, AltShiftLeft, AltShiftRight, AltShiftDelete,
    AltShiftHome, AltShiftEnd, AltShiftPageUp, AltShiftPageDown,
    CtrlUp, CtrlDown, CtrlLeft, CtrlRight, CtrlHome, CtrlEnd, CtrlBackspace,
    CtrlDelete, CtrlPageUp, CtrlPageDown,
    Alt, CtrlAlt,
    CtrlAltUp, CtrlAltDown, CtrlAltLeft, CtrlAltRight, CtrlAltHome, CtrlAltEnd,
    CtrlAltBackspace, CtrlAltDelete, CtrlAltPageUp, CtrlAltPageDown,
    CtrlShiftUp, CtrlShiftDown, CtrlShiftLeft, CtrlShiftRight, CtrlShiftHome,
    CtrlShiftEnd, CtrlShiftDelete, CtrlShiftPageUp, CtrlShiftPageDown,
    CtrlAltShiftUp, CtrlAltShiftDown, CtrlAltShiftLeft, CtrlAltShiftRight,
    CtrlAltShiftHome, CtrlAltShiftEnd, CtrlAltShiftDelete, CtrlAltShiftPageUp,
    CtrlAltShiftPageDown,
    Mouse, DoubleClick, LeftClick, RightClick, SLeftClick, SRightClick,
    ScrollUp, ScrollDown, SScrollUp, SScrollDown, PreviewScrollUp, PreviewScrollDown,
    Invalid, Fatal, BracketedPasteBegin, BracketedPasteEnd,
    // Non-key events
    Resize, Change, BackwardEOF, Start, Load, Focus, One, Zero, Result, Jump,
    JumpCancel, ClickHeader, ClickFooter, Multi, Every, ResultFinal,
};

// fzf: tui.Event (without the mouse payload, which lives in KeyEvent).
// `ch` carries the rune for Rune / Alt / CtrlAlt events and the interval in
// milliseconds for Every.
struct Event {
    EventType type = EventType::Invalid;
    char32_t ch = 0;

    bool operator==(const Event& o) const { return type == o.type && ch == o.ch; }
    bool operator!=(const Event& o) const { return !(*this == o); }
    bool operator<(const Event& o) const {
        return type != o.type ? type < o.type : ch < o.ch;
    }
    // fzf: Event.Printable -- a graphic rune that `put` may insert.
    bool printable() const;
};

inline Event event_of(EventType t) { return Event{t, 0}; }
inline Event rune_event(char32_t r) { return Event{EventType::Rune, r}; }      // fzf: tui.Key
inline Event alt_event(char32_t r) { return Event{EventType::Alt, r}; }        // fzf: tui.AltKey
inline Event ctrl_alt_event(char32_t r) { return Event{EventType::CtrlAlt, r}; } // fzf: tui.CtrlAltKey

struct EventHash {
    size_t operator()(const Event& e) const {
        return std::hash<uint32_t>()((static_cast<uint32_t>(e.type) << 21) ^ static_cast<uint32_t>(e.ch));
    }
};

// fzf: actionType. Every action name fzf 0.74 accepts, plus the internal
// ones (Char, Mouse, Click, Invalid, Fatal, BracketedPaste*, SigStop).
enum class ActionType : uint16_t {
    Ignore, Start, Click, Invalid, Fatal, BracketedPasteBegin, BracketedPasteEnd,
    Char, Mouse, BeginningOfLine, Abort, Accept, AcceptNonEmpty,
    AcceptOrPrintQuery, BackwardChar, BackwardDeleteChar, BackwardDeleteCharEof,
    BackwardWord, BackwardSubWord, Cancel, ChangeBorderLabel, ChangeGhost,
    ChangeHeader, ChangeHeaderLines, ChangeFooter, ChangeInputLabel,
    ChangeHeaderLabel, ChangeFooterLabel, ChangeListLabel, ChangeMulti,
    ChangeNth, ChangeWithNth, ChangePointer, ChangePreview, ChangePreviewLabel,
    ChangePreviewWindow, ChangePrompt, ChangeQuery, ClearScreen, ClearQuery,
    ClearSelection, Close, DeleteChar, DeleteCharEof, EndOfLine, Forward, Backward,
    ForwardChar, ForwardWord, ForwardSubWord, KillLine, KillWord, KillSubWord,
    UnixLineDiscard, UnixWordRubout, Yank, BackwardKillWord, BackwardKillSubWord,
    SelectAll, DeselectAll, Toggle, ToggleSearch, ToggleAll, ToggleDown, ToggleUp,
    ToggleIn, ToggleOut, ToggleTrack, ToggleTrackCurrent, ToggleHeader,
    ToggleWrap, ToggleWrapWord, ToggleMultiLine, ToggleHscroll, ToggleRaw,
    EnableRaw, DisableRaw, ToggleInput, HideInput, ShowInput, TrackCurrent,
    UntrackCurrent, Down, DownMatch, Up, UpMatch, PageUp, PageDown, HalfPageUp,
    HalfPageDown, OffsetUp, OffsetDown, OffsetMiddle, Jump, JumpAccept, Print,
    PrintQuery, Put, Refresh, RefreshPreview, ReplaceQuery, ToggleSort,
    ShowPreview, HidePreview, TogglePreview, TogglePreviewWrap,
    TogglePreviewWrapWord, TransformBorderLabel, TransformGhost, TransformHeader,
    TransformHeaderLines, TransformFooter, TransformInputLabel,
    TransformHeaderLabel, TransformFooterLabel, TransformListLabel, TransformNth,
    TransformWithNth, TransformPointer, TransformPreviewLabel, TransformPrompt,
    TransformQuery, TransformSearch, Transform, BgTransformBorderLabel,
    BgTransformGhost, BgTransformHeader, BgTransformHeaderLines, BgTransformFooter,
    BgTransformInputLabel, BgTransformHeaderLabel, BgTransformFooterLabel,
    BgTransformListLabel, BgTransformNth, BgTransformWithNth, BgTransformPointer,
    BgTransformPreviewLabel, BgTransformPrompt, BgTransformQuery, BgTransformSearch,
    BgTransform, BgCancel, Trigger, Search, Preview, ChangePreviewOneShot,
    PreviewTop, PreviewBottom, PreviewUp, PreviewDown, PreviewPageUp,
    PreviewPageDown, PreviewHalfPageUp, PreviewHalfPageDown, PrevHistory,
    PrevSelected, NextHistory, NextSelected, Execute, ExecuteSilent, ExecuteMulti,
    SigStop, First, Last, Best, Position, Reload, ReloadSync, Unbind, Rebind,
    ToggleBind, Become, ShowHeader, HideHeader, Select, Deselect, Exclude,
    ExcludeMulti, EnableSearch, DisableSearch, Wait, Bell,
};

struct Action {
    ActionType type = ActionType::Ignore;
    std::string arg;
    bool operator==(const Action& o) const { return type == o.type && arg == o.arg; }
};
using ActionList = std::vector<Action>;
using Keymap = std::unordered_map<Event, ActionList, EventHash>;

// --- Parsing (throws OptionError from options.hpp) ---

// fzf: parseKeyChords. Returns the events in order of appearance; `names`
// (optional) receives the original spelling of each key, which is what
// --expect prints. `message` is the error when the string is empty.
std::vector<Event> parse_key_chords(const std::string& str, const std::string& message,
                                    std::map<Event, std::string>* names = nullptr);

// fzf: parseKeymap. Adds the bindings in `str` to `keymap`, replacing the
// action list of every key mentioned (or appending, for `key:+action`).
void parse_keymap(Keymap& keymap, const std::string& str);

// fzf: parseSingleActionList (used by `transform` output and tests).
ActionList parse_single_action_list(const std::string& str, bool put_allowed);

// fzf: defaultKeymap
Keymap default_keymap();

// The keymap for a parsed Options: defaults, then --bind and --toggle-sort
// in order, with fzf's post-processing (double-click inherits enter, preview
// window actions moved first, --history ctrl-n/p).
Keymap build_keymap(Options& opts);

// --expect as event -> original key name.
std::map<Event, std::string> build_expect(const Options& opts);

// fzf: maskActionContents (exposed for tests).
std::string mask_action_contents(const std::string& action);

// Map a decoded terminal KeyEvent to the Event the keymap uses.
Event to_event(const KeyEvent& ev);

const char* action_name(ActionType t);

} // namespace fzf
