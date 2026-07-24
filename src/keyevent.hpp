#pragma once

#include <string>

namespace fzf {

enum class KeyType {
    Character,  // A decoded UTF-8 character (or a raw control byte fzf treats
                // as "characters" for bind-key lookup purposes, e.g. ctrl-a)
    Special,    // A named key: Return, Escape, Tab, arrows, Home/End, etc.
    Mouse,
    Resize,
    WakeUp,     // A background thread or SIGWINCH signaled the main loop;
                // carries no payload beyond its type.
};

enum class SpecialKey {
    None,
    Return,
    Escape,
    Tab,
    Backspace,
    Delete,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    PageUp,
    PageDown,
    Home,
    End,
    BackTab,
    Insert,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    ShiftArrowUp,
    ShiftArrowDown,
    ShiftArrowLeft,
    ShiftArrowRight,
    CtrlArrowUp,
    CtrlArrowDown,
    CtrlArrowLeft,
    CtrlArrowRight,
    AltArrowUp,
    AltArrowDown,
    AltArrowLeft,
    AltArrowRight,
};

struct MouseInfo {
    enum class Button { Left, Middle, Right, WheelUp, WheelDown, None };
    enum class Motion { Pressed, Released, Moved };

    Button button = Button::None;
    Motion motion = Motion::Pressed;
    int x = 0;
    int y = 0;
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
};

struct KeyEvent {
    KeyType type = KeyType::Character;
    SpecialKey special = SpecialKey::None;

    // Raw decoded byte string for this event, mirroring ftxui::Event::input()
    // — e.g. "\x01" for ctrl-a, "a" for a plain 'a', "\x1b[1;5A" for
    // ctrl-up. This is what check_expect_key/event_to_bind_key pattern-match
    // against, so it's populated for Character and Special events alike.
    std::string input;

    // Decoded UTF-8 codepoints for Character events (normally length 1;
    // plural to allow for pasted/multi-codepoint reads without redesigning
    // the type later).
    std::u32string codepoints;

    MouseInfo mouse;

    int resize_rows = 0;
    int resize_cols = 0;

    static KeyEvent make_character(std::string input, std::u32string codepoints) {
        KeyEvent e;
        e.type = KeyType::Character;
        e.input = std::move(input);
        e.codepoints = std::move(codepoints);
        return e;
    }

    static KeyEvent make_special(SpecialKey key, std::string input) {
        KeyEvent e;
        e.type = KeyType::Special;
        e.special = key;
        e.input = std::move(input);
        return e;
    }

    static KeyEvent make_mouse(MouseInfo mouse) {
        KeyEvent e;
        e.type = KeyType::Mouse;
        e.mouse = mouse;
        return e;
    }

    static KeyEvent make_resize(int rows, int cols) {
        KeyEvent e;
        e.type = KeyType::Resize;
        e.resize_rows = rows;
        e.resize_cols = cols;
        return e;
    }

    static KeyEvent make_wakeup() {
        KeyEvent e;
        e.type = KeyType::WakeUp;
        return e;
    }

    bool is_character() const { return type == KeyType::Character; }
    bool is_mouse() const { return type == KeyType::Mouse; }
};

} // namespace fzf
