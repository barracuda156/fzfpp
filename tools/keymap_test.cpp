// Unit tests for src/keymap.cpp (T1.2): key-chord names, --bind action
// lists with every delimiter form, the default keymap, --expect, and the
// KeyEvent -> Event mapping. The fixtures are ported from fzf's
// src/options_test.go (TestParseKeys, TestParseKeysWithComma, TestBind,
// TestParseEveryEvent, TestParseSingleActionList*, TestMaskActionContents,
// TestToggle, TestDefaultCtrlNP, TestAdditiveExpect).

#include "keymap.hpp"
#include "options.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace fzf;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string chord_error(const std::string& s) {
    try { parse_key_chords(s, "key names required"); } catch (const OptionError& e) { return e.message; }
    return "";
}

static std::string keymap_error(const std::string& s) {
    Keymap k;
    try { parse_keymap(k, s); } catch (const OptionError& e) { return e.message; }
    return "";
}

static bool has_actions(const Keymap& k, Event e, std::vector<ActionType> types, const std::string& arg1 = "") {
    auto it = k.find(e);
    if (it == k.end()) return types.empty();
    if (it->second.size() != types.size()) return false;
    for (size_t i = 0; i < types.size(); ++i) {
        if (it->second[i].type != types[i]) return false;
    }
    if (!arg1.empty() && it->second[0].arg != arg1) return false;
    return true;
}

static void test_parse_keys() {
    std::map<Event, std::string> names;
    auto list = parse_key_chords("ctrl-z,alt-z,f2,@,Alt-a,!,ctrl-G,J,g,ctrl-alt-a,ALT-enter,alt-SPACE", "", &names);
    CHECK(list.size() == 12 && names.size() == 12);
    CHECK(names[event_of(EventType::CtrlZ)] == "ctrl-z");
    CHECK(names[event_of(EventType::F2)] == "f2");
    CHECK(names[event_of(EventType::CtrlG)] == "ctrl-G");
    CHECK(names[alt_event(U'z')] == "alt-z");
    CHECK(names[rune_event(U'@')] == "@");
    CHECK(names[alt_event(U'a')] == "Alt-a");
    CHECK(names[rune_event(U'!')] == "!");
    CHECK(names[rune_event(U'J')] == "J");
    CHECK(names[rune_event(U'g')] == "g");
    CHECK(names[ctrl_alt_event(U'a')] == "ctrl-alt-a");
    CHECK(names[ctrl_alt_event(U'm')] == "ALT-enter");
    CHECK(names[alt_event(U' ')] == "alt-SPACE");

    names.clear();
    parse_key_chords("enter,Return,space,tab,btab,esc,up,down,left,right", "", &names);
    CHECK(names.size() == 9);
    CHECK(names[event_of(EventType::Enter)] == "Return");
    CHECK(names[rune_event(U' ')] == "space");
    CHECK(names[event_of(EventType::Tab)] == "tab");
    CHECK(names[event_of(EventType::ShiftTab)] == "btab");
    CHECK(names[event_of(EventType::Esc)] == "esc");
    CHECK(names[event_of(EventType::Up)] == "up");
    CHECK(names[event_of(EventType::Right)] == "right");

    names.clear();
    parse_key_chords("Tab,Ctrl-I,PgUp,page-up,pgdn,Page-Down,Home,End,Alt-BS,Alt-BSpace,shift-left,shift-right,btab,shift-tab,return,Enter,bspace", "", &names);
    CHECK(names.size() == 11);
    CHECK(names[event_of(EventType::Tab)] == "Ctrl-I");
    CHECK(names[event_of(EventType::PageUp)] == "page-up");
    CHECK(names[event_of(EventType::PageDown)] == "Page-Down");
    CHECK(names[event_of(EventType::AltBackspace)] == "Alt-BSpace");
    CHECK(names[event_of(EventType::ShiftLeft)] == "shift-left");
    CHECK(names[event_of(EventType::ShiftTab)] == "shift-tab");
    CHECK(names[event_of(EventType::Enter)] == "Enter");
    CHECK(names[event_of(EventType::Backspace)] == "bspace");

    // ctrl-h is backspace on Unix, f1..f12, insert, del, ctrl-^ etc.
    names.clear();
    parse_key_chords("ctrl-h,f1,f9,f10,f12,insert,del,delete,ctrl-^,ctrl-6,ctrl-/,ctrl-_,ctrl-\\,ctrl-],ctrl-space,double-click,left-click,scroll-up,preview-scroll-down,load,zero,one,result,change,focus,start,resize,multi,backward-eof,jump,jump-cancel,click-header", "", &names);
    CHECK(names.count(event_of(EventType::CtrlBackspace)) == 1);
    CHECK(names[event_of(EventType::F1)] == "f1" && names[event_of(EventType::F9)] == "f9");
    CHECK(names[event_of(EventType::F10)] == "f10" && names[event_of(EventType::F12)] == "f12");
    CHECK(names.count(event_of(EventType::Insert)) && names.count(event_of(EventType::Delete)));
    CHECK(names.count(event_of(EventType::CtrlCaret)) && names.count(event_of(EventType::CtrlSlash)));
    CHECK(names.count(event_of(EventType::CtrlBackSlash)) && names.count(event_of(EventType::CtrlRightBracket)));
    CHECK(names.count(event_of(EventType::CtrlSpace)) && names.count(event_of(EventType::DoubleClick)));
    CHECK(names.count(event_of(EventType::Load)) && names.count(event_of(EventType::Zero)) && names.count(event_of(EventType::One)));
    CHECK(names.count(event_of(EventType::ClickHeader)) && names.count(event_of(EventType::BackwardEOF)));

    CHECK(chord_error("ctrl-1") == "unsupported key: ctrl-1");
    CHECK(chord_error("foo") == "unsupported key: foo");
    CHECK(chord_error("") == "key names required");
    // Unicode single-rune keys are fine
    names.clear();
    parse_key_chords("é,alt-é", "", &names);
    CHECK(names[rune_event(U'é')] == "é" && names[alt_event(U'é')] == "alt-é");
}

static void test_parse_keys_with_comma() {
    std::map<Event, std::string> names;
    parse_key_chords(",", "", &names);
    CHECK(names.size() == 1 && names[rune_event(U',')] == ",");
    names.clear();
    parse_key_chords(",,a,b", "", &names);
    CHECK(names.size() == 3 && names[rune_event(U'a')] == "a" && names[rune_event(U',')] == ",");
    names.clear();
    parse_key_chords("a,b,,", "", &names);
    CHECK(names.size() == 3 && names[rune_event(U',')] == ",");
    names.clear();
    parse_key_chords("a,,,b", "", &names);
    CHECK(names.size() == 3 && names[rune_event(U'b')] == "b" && names[rune_event(U',')] == ",");
    names.clear();
    parse_key_chords("a,,,b,c", "", &names);
    CHECK(names.size() == 4 && names[rune_event(U'c')] == "c");
    names.clear();
    parse_key_chords(",,,", "", &names);
    CHECK(names.size() == 1 && names[rune_event(U',')] == ",");
    names.clear();
    parse_key_chords(",ALT-,,", "", &names);
    CHECK(names.size() == 1 && names[alt_event(U',')] == "ALT-,");
}

static void test_bind() {
    Keymap keymap = default_keymap();
    CHECK(has_actions(keymap, event_of(EventType::CtrlA), {ActionType::BeginningOfLine}));
    parse_keymap(keymap,
        "ctrl-a:kill-line,ctrl-b:toggle-sort+up+down,c:page-up,alt-z:page-down,"
        "f1:execute(ls {+})+abort+execute(echo \n{+})+select-all,f2:execute/echo {}, {}, {}/,f3:execute[echo '({})'],f4:execute;less {};,"
        "alt-a:execute-Multi@echo (,),[,],/,:,;,%,{}@,alt-b:execute;echo (,),[,],/,:,@,%,{};,"
        "x:Execute(foo+bar),X:execute/bar+baz/"
        ",f1:+first,f1:+top"
        ",,:abort,::accept,+:execute:++\nfoobar,Y:execute(baz)+up");
    CHECK(has_actions(keymap, event_of(EventType::CtrlA), {ActionType::KillLine}));
    CHECK(has_actions(keymap, event_of(EventType::CtrlB), {ActionType::ToggleSort, ActionType::Up, ActionType::Down}));
    CHECK(has_actions(keymap, rune_event(U'c'), {ActionType::PageUp}));
    CHECK(has_actions(keymap, rune_event(U','), {ActionType::Abort}));
    CHECK(has_actions(keymap, rune_event(U':'), {ActionType::Accept}));
    CHECK(has_actions(keymap, alt_event(U'z'), {ActionType::PageDown}));
    CHECK(has_actions(keymap, event_of(EventType::F1),
        {ActionType::Execute, ActionType::Abort, ActionType::Execute, ActionType::SelectAll, ActionType::First, ActionType::First}, "ls {+}"));
    CHECK(has_actions(keymap, event_of(EventType::F2), {ActionType::Execute}, "echo {}, {}, {}"));
    CHECK(has_actions(keymap, event_of(EventType::F3), {ActionType::Execute}, "echo '({})'"));
    CHECK(has_actions(keymap, event_of(EventType::F4), {ActionType::Execute}, "less {}"));
    CHECK(has_actions(keymap, rune_event(U'x'), {ActionType::Execute}, "foo+bar"));
    CHECK(has_actions(keymap, rune_event(U'X'), {ActionType::Execute}, "bar+baz"));
    CHECK(has_actions(keymap, alt_event(U'a'), {ActionType::ExecuteMulti}, "echo (,),[,],/,:,;,%,{}"));
    CHECK(has_actions(keymap, alt_event(U'b'), {ActionType::Execute}, "echo (,),[,],/,:,@,%,{}"));
    CHECK(has_actions(keymap, rune_event(U'+'), {ActionType::Execute}, "++\nfoobar,Y:execute(baz)+up"));

    const char delims[] = {'~', '!', '@', '#', '$', '%', '^', '&', '*', '|', ';', '/'};
    for (size_t i = 0; i < sizeof(delims); ++i) {
        char key = static_cast<char>('0' + (i % 10));
        std::string spec = std::string(1, key) + ":execute" + delims[i] + "foobar" + delims[i];
        parse_keymap(keymap, spec);
        CHECK(has_actions(keymap, rune_event(static_cast<char32_t>(key)), {ActionType::Execute}, "foobar"));
    }

    parse_keymap(keymap, "f1:abort");
    CHECK(has_actions(keymap, event_of(EventType::F1), {ActionType::Abort}));

    // Trailing-colon form keeps commas and pluses; the port's real consumers
    parse_keymap(keymap, "focus:transform-header:case $a in a,b) echo x+y;; esac");
    CHECK(has_actions(keymap, event_of(EventType::Focus), {ActionType::TransformHeader}, "case $a in a,b) echo x+y;; esac"));
    parse_keymap(keymap, "change:reload:rg --column {q} || true");
    CHECK(has_actions(keymap, event_of(EventType::Change), {ActionType::Reload}, "rg --column {q} || true"));
    parse_keymap(keymap, "ctrl-r:reload(date +%s)+first");
    CHECK(has_actions(keymap, event_of(EventType::CtrlR), {ActionType::Reload, ActionType::First}, "date +%s"));
    parse_keymap(keymap, "enter:become(vim {})");
    CHECK(has_actions(keymap, event_of(EventType::Enter), {ActionType::Become}, "vim {}"));
    parse_keymap(keymap, "ctrl-/:toggle-preview,ctrl-space:toggle-wrap+toggle-preview-wrap");
    CHECK(has_actions(keymap, event_of(EventType::CtrlSlash), {ActionType::TogglePreview}));
    CHECK(has_actions(keymap, event_of(EventType::CtrlSpace), {ActionType::ToggleWrap, ActionType::TogglePreviewWrap}));
    parse_keymap(keymap, "space:accept,bspace:abort,pgup:first,ctrl-alt-a:last");
    CHECK(has_actions(keymap, rune_event(U' '), {ActionType::Accept}));
    CHECK(has_actions(keymap, event_of(EventType::Backspace), {ActionType::Abort}));
    CHECK(has_actions(keymap, event_of(EventType::PageUp), {ActionType::First}));
    CHECK(has_actions(keymap, ctrl_alt_event(U'a'), {ActionType::Last}));
    parse_keymap(keymap, "tab:toggle-down,btab:toggle-up");
    CHECK(has_actions(keymap, event_of(EventType::Tab), {ActionType::Toggle, ActionType::Down}));
    CHECK(has_actions(keymap, event_of(EventType::ShiftTab), {ActionType::Toggle, ActionType::Up}));
    parse_keymap(keymap, "ctrl-t:unbind(ctrl-t)+rebind(ctrl-u)");
    CHECK(has_actions(keymap, event_of(EventType::CtrlT), {ActionType::Unbind, ActionType::Rebind}, "ctrl-t"));
    parse_keymap(keymap, "ctrl-v:change-preview-window(right,70%|down,40%,border-horizontal|hidden|right)");
    CHECK(has_actions(keymap, event_of(EventType::CtrlV), {ActionType::ChangePreviewWindow}));
    parse_keymap(keymap, "a:put,b:change-multi,c:change-multi(3),d:pos(2),e:transform(echo accept)");
    CHECK(has_actions(keymap, rune_event(U'a'), {ActionType::Char}));
    CHECK(has_actions(keymap, rune_event(U'b'), {ActionType::ChangeMulti}));
    CHECK(has_actions(keymap, rune_event(U'c'), {ActionType::ChangeMulti}, "3"));
    CHECK(has_actions(keymap, rune_event(U'd'), {ActionType::Position}, "2"));
    CHECK(has_actions(keymap, rune_event(U'e'), {ActionType::Transform}, "echo accept"));
    // Multiple keys sharing one action list
    parse_keymap(keymap, "ctrl-x,ctrl-y:abort");
    CHECK(has_actions(keymap, event_of(EventType::CtrlX), {ActionType::Abort}));
    CHECK(has_actions(keymap, event_of(EventType::CtrlY), {ActionType::Abort}));

    CHECK(keymap_error("ctrl-a:nonsense") == "unknown action: nonsense");
    CHECK(keymap_error("ctrl-a") == "bind action not specified: ctrl-a");
    CHECK(keymap_error(":accept") == "key name required");
    CHECK(keymap_error("foo:accept") == "unsupported key: foo");
    CHECK(keymap_error("ctrl-a:unbind(bogus)") == "unsupported key: bogus");
    CHECK(keymap_error("ctrl-a:change-preview-window(sideways)") == "invalid preview window option: sideways");
    CHECK(keymap_error("ctrl-a:put") == "unable to put non-printable character");
    CHECK(keymap_error("change-query(foobar)baz") != "");
}

static void test_every_event() {
    std::map<Event, std::string> names;
    parse_key_chords("every(2),every(0.5)", "", &names);
    CHECK(names.size() == 2);
    CHECK((names[Event{EventType::Every, 2000}] == "every(2)"));
    CHECK((names[Event{EventType::Every, 500}] == "every(0.5)"));
    names.clear();
    parse_key_chords("every(0.001)", "", &names);
    CHECK((names[Event{EventType::Every, 10}] == "every(0.001)"));
    for (const char* bad : {"every(0)", "every(-1)", "every(abc)", "every()", "every(2147484)"}) {
        CHECK(!chord_error(bad).empty());
    }
}

static void test_single_action_list() {
    ActionList actions = parse_single_action_list("Execute@foo+bar,baz@+up+up+reload:down+down", false);
    CHECK(actions.size() == 4);
    CHECK(actions[0].type == ActionType::Execute && actions[0].arg == "foo+bar,baz");
    CHECK(actions[1].type == ActionType::Up && actions[2].type == ActionType::Up);
    CHECK(actions[3].type == ActionType::Reload && actions[3].arg == "down+down");
    bool threw = false;
    try { parse_single_action_list("change-query(foobar)baz", false); } catch (const OptionError&) { threw = true; }
    CHECK(threw);
}

static void test_mask_action_contents() {
    std::string original = ":execute((f)(o)(o)(b)(a)(r))+change-query@qu@ry@+up,x:reload:hello:world";
    std::string expected = ":execute                    +change-query       +up,x:reload            ";
    CHECK(mask_action_contents(original) == expected);
}

static void test_build_keymap_and_expect() {
    auto opts_for = [](std::vector<std::string> args) { return parse_option_args(args, false); };

    Options o = opts_for({});
    Keymap k = build_keymap(o);
    CHECK(!o.toggle_sort);
    CHECK(has_actions(k, event_of(EventType::CtrlN), {ActionType::DownMatch}));
    CHECK(has_actions(k, event_of(EventType::CtrlP), {ActionType::UpMatch}));
    CHECK(has_actions(k, event_of(EventType::DoubleClick), {ActionType::Accept}));   // inherits enter
    CHECK(has_actions(k, event_of(EventType::CtrlL), {ActionType::ClearScreen}));
    CHECK(has_actions(k, event_of(EventType::CtrlZ), {ActionType::SigStop}));

    o = opts_for({"--bind=a:toggle-sort"});
    build_keymap(o);
    CHECK(o.toggle_sort);
    o = opts_for({"--bind=a:toggle-sort", "--bind=a:up"});
    build_keymap(o);
    CHECK(!o.toggle_sort);
    o = opts_for({"--toggle-sort", "ctrl-r"});
    k = build_keymap(o);
    CHECK(o.toggle_sort && has_actions(k, event_of(EventType::CtrlR), {ActionType::ToggleSort}));

    o = opts_for({"--bind=ctrl-n:accept"});
    k = build_keymap(o);
    CHECK(has_actions(k, event_of(EventType::CtrlN), {ActionType::Accept}));
    o = opts_for({"--history=/tmp/fzfpp-history-test"});
    k = build_keymap(o);
    CHECK(has_actions(k, event_of(EventType::CtrlN), {ActionType::NextHistory}));
    CHECK(has_actions(k, event_of(EventType::CtrlP), {ActionType::PrevHistory}));
    o = opts_for({"--history=/tmp/fzfpp-history-test", "--bind=ctrl-n:accept"});
    k = build_keymap(o);
    CHECK(has_actions(k, event_of(EventType::CtrlN), {ActionType::Accept}));
    CHECK(has_actions(k, event_of(EventType::CtrlP), {ActionType::PrevHistory}));

    // Preview-window actions are moved to the front of the list
    o = opts_for({"--bind", "ctrl-v:preview(sleep 1)+change-preview-window(up,+10)+up"});
    k = build_keymap(o);
    CHECK(has_actions(k, event_of(EventType::CtrlV), {ActionType::ChangePreviewWindow, ActionType::Preview, ActionType::Up}, "up,+10"));

    // enter bound to become: double-click follows
    o = opts_for({"--bind", "enter:become(echo {})"});
    k = build_keymap(o);
    CHECK(has_actions(k, event_of(EventType::DoubleClick), {ActionType::Become}, "echo {}"));

    // --expect: additive, names preserved
    o = opts_for({"--expect=a", "--expect", "b", "--expect=c,ctrl-alt-x,F1"});
    auto expect = build_expect(o);
    CHECK(expect.size() == 5);
    CHECK(expect[rune_event(U'a')] == "a" && expect[event_of(EventType::F1)] == "F1");
    CHECK(expect[ctrl_alt_event(U'x')] == "ctrl-alt-x");
    o = opts_for({"--expect=a", "--no-expect"});
    CHECK(build_expect(o).empty());
}

static KeyEvent special(SpecialKey k, const char* in) { return KeyEvent::make_special(k, in); }
static KeyEvent chr(const char* in, char32_t cp) { return KeyEvent::make_character(in, std::u32string(1, cp)); }

static void test_to_event() {
    CHECK(to_event(special(SpecialKey::Return, "\r")) == event_of(EventType::Enter));
    CHECK(to_event(special(SpecialKey::Escape, "\x1b")) == event_of(EventType::Esc));
    CHECK(to_event(special(SpecialKey::Tab, "\t")) == event_of(EventType::Tab));
    CHECK(to_event(special(SpecialKey::BackTab, "\x1b[Z")) == event_of(EventType::ShiftTab));
    CHECK(to_event(special(SpecialKey::F1, "\x1bOP")) == event_of(EventType::F1));
    CHECK(to_event(special(SpecialKey::ShiftArrowLeft, "\x1b[1;2D")) == event_of(EventType::ShiftLeft));
    CHECK(to_event(chr("\x01", 1)) == event_of(EventType::CtrlA));
    CHECK(to_event(chr("\x1a", 26)) == event_of(EventType::CtrlZ));
    CHECK(to_event(KeyEvent::make_character(std::string(1, '\0'), std::u32string(1, 0))) == event_of(EventType::CtrlSpace));
    CHECK(to_event(chr("\x1f", 0x1f)) == event_of(EventType::CtrlSlash));
    CHECK(to_event(chr("\x1c", 0x1c)) == event_of(EventType::CtrlBackSlash));
    CHECK(to_event(chr("a", U'a')) == rune_event(U'a'));
    CHECK(to_event(chr(" ", U' ')) == rune_event(U' '));
    CHECK(to_event(chr("\xc3\xa9", U'é')) == rune_event(U'é'));
    CHECK(to_event(chr("\x1b" "b", U'b')) == alt_event(U'b'));
    CHECK(to_event(chr("\x1b" "\x7f", 0x7f)) == event_of(EventType::AltBackspace));
    CHECK(to_event(chr("\x1b" "\x01", 1)) == ctrl_alt_event(U'a'));
    MouseInfo m; m.button = MouseInfo::Button::WheelUp;
    CHECK(to_event(KeyEvent::make_mouse(m)) == event_of(EventType::ScrollUp));
    m.button = MouseInfo::Button::Left; m.motion = MouseInfo::Motion::Pressed;
    CHECK(to_event(KeyEvent::make_mouse(m)) == event_of(EventType::LeftClick));
    m.shift = true;
    CHECK(to_event(KeyEvent::make_mouse(m)) == event_of(EventType::SLeftClick));
    m.shift = false; m.button = MouseInfo::Button::Right;
    CHECK(to_event(KeyEvent::make_mouse(m)) == event_of(EventType::RightClick));
    m.button = MouseInfo::Button::Left; m.motion = MouseInfo::Motion::Moved;
    CHECK(to_event(KeyEvent::make_mouse(m)) == event_of(EventType::Mouse));
}

int main() {
    test_parse_keys();
    test_parse_keys_with_comma();
    test_bind();
    test_every_event();
    test_single_action_list();
    test_mask_action_contents();
    test_build_keymap_and_expect();
    test_to_event();
    std::printf("keymap_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
