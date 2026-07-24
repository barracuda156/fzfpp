// Standalone unit tests for src/keyparser.cpp — feeds raw byte strings and
// asserts on the resulting KeyEvents, with no pty/tty involvement at all.
// This is the regression guard for the ANSI tables ported from
// Terminal::check_expect_key/event_to_bind_key (fixed this session).
//
// Run directly: ./keyparser_test

#include "../src/keyparser.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using fzf::KeyEvent;
using fzf::KeyParser;
using fzf::KeyType;
using fzf::MouseInfo;
using fzf::SpecialKey;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::vector<KeyEvent> feed_all(const std::string& bytes) {
    KeyParser p;
    return p.feed(bytes);
}

void test_plain_ascii() {
    auto events = feed_all("a");
    check(events.size() == 1, "plain 'a' yields one event");
    check(events[0].type == KeyType::Character, "plain 'a' is Character");
    check(events[0].input == "a", "plain 'a' input byte is 'a'");
    check(events[0].codepoints.size() == 1 && events[0].codepoints[0] == U'a',
          "plain 'a' codepoint decoded");
}

void test_ctrl_letters() {
    for (char letter = 'a'; letter <= 'z'; ++letter) {
        char ctrl_byte = static_cast<char>(letter - 'a' + 1);
        // ctrl-i (0x09) is Tab, ctrl-m (0x0d) is Return, ctrl-h (0x08) is
        // Backspace — these are intentionally special-cased, not plain
        // Character events, so skip them here (covered in their own tests).
        // ctrl-j (0x0a) is deliberately NOT skipped: it must fall through to
        // the plain Character path like every other ctrl-letter (see
        // test_ctrl_j_is_character), not be special-cased to Return.
        if (ctrl_byte == 0x09 || ctrl_byte == 0x0d || ctrl_byte == 0x08) {
            continue;
        }
        std::string bytes(1, ctrl_byte);
        auto events = feed_all(bytes);
        check(events.size() == 1, "ctrl-letter yields one event");
        if (events.size() == 1) {
            check(events[0].type == KeyType::Character, "ctrl-letter is Character type");
            check(events[0].input.size() == 1 && events[0].input[0] == ctrl_byte,
                  "ctrl-letter input byte matches");
        }
    }
}

void test_ctrl_special_bytes() {
    struct Case { unsigned char byte; const char* name; };
    Case cases[] = {
        {0x00, "ctrl-space (NUL)"},
        {0x1f, "ctrl-/ (0x1f)"},
        {0x1c, "ctrl-\\ (0x1c)"},
        {0x1d, "ctrl-] (0x1d)"},
    };
    for (auto& c : cases) {
        std::string bytes(1, static_cast<char>(c.byte));
        auto events = feed_all(bytes);
        check(events.size() == 1, c.name);
        if (events.size() == 1) {
            check(events[0].type == KeyType::Character, "special ctrl byte is Character type");
            check(static_cast<unsigned char>(events[0].input[0]) == c.byte,
                  "special ctrl byte value matches");
        }
    }
}

void test_named_keys() {
    struct Case { std::string bytes; SpecialKey key; const char* name; };
    Case cases[] = {
        {"\r", SpecialKey::Return, "CR is Return"},
        {"\t", SpecialKey::Tab, "0x09 is Tab"},
        {"\x7f", SpecialKey::Backspace, "0x7f is Backspace"},
        {"\x08", SpecialKey::Backspace, "0x08 is Backspace"},
        {"\x1b[A", SpecialKey::ArrowUp, "CSI A is ArrowUp"},
        {"\x1b[B", SpecialKey::ArrowDown, "CSI B is ArrowDown"},
        {"\x1b[C", SpecialKey::ArrowRight, "CSI C is ArrowRight"},
        {"\x1b[D", SpecialKey::ArrowLeft, "CSI D is ArrowLeft"},
        {"\x1bOA", SpecialKey::ArrowUp, "SS3 A is ArrowUp"},
        {"\x1bOD", SpecialKey::ArrowLeft, "SS3 D is ArrowLeft"},
        {"\x1b[H", SpecialKey::Home, "CSI H is Home"},
        {"\x1b[F", SpecialKey::End, "CSI F is End"},
        {"\x1b[1~", SpecialKey::Home, "CSI 1~ is Home"},
        {"\x1b[4~", SpecialKey::End, "CSI 4~ is End"},
        {"\x1b[3~", SpecialKey::Delete, "CSI 3~ is Delete"},
        {"\x1b[5~", SpecialKey::PageUp, "CSI 5~ is PageUp"},
        {"\x1b[6~", SpecialKey::PageDown, "CSI 6~ is PageDown"},
        {"\x1b[Z", SpecialKey::BackTab, "CSI Z is shift-Tab (BackTab)"},
        {"\x1b[1;2D", SpecialKey::ShiftArrowLeft, "CSI 1;2D is ShiftArrowLeft"},
        {"\x1b[1;2C", SpecialKey::ShiftArrowRight, "CSI 1;2C is ShiftArrowRight"},
        {"\x1b[1;2A", SpecialKey::ShiftArrowUp, "CSI 1;2A is ShiftArrowUp"},
        {"\x1b[1;2B", SpecialKey::ShiftArrowDown, "CSI 1;2B is ShiftArrowDown"},
        {"\x1b[2D", SpecialKey::ShiftArrowLeft, "CSI 2D (legacy) is ShiftArrowLeft"},
        {"\x1b[2C", SpecialKey::ShiftArrowRight, "CSI 2C (legacy) is ShiftArrowRight"},
        {"\x1b[2A", SpecialKey::ShiftArrowUp, "CSI 2A (legacy) is ShiftArrowUp"},
        {"\x1b[2B", SpecialKey::ShiftArrowDown, "CSI 2B (legacy) is ShiftArrowDown"},
        {"\x1b[1;5D", SpecialKey::CtrlArrowLeft, "CSI 1;5D is CtrlArrowLeft"},
        {"\x1b[1;5C", SpecialKey::CtrlArrowRight, "CSI 1;5C is CtrlArrowRight"},
        {"\x1b[1;5A", SpecialKey::CtrlArrowUp, "CSI 1;5A is CtrlArrowUp"},
        {"\x1b[1;5B", SpecialKey::CtrlArrowDown, "CSI 1;5B is CtrlArrowDown"},
        {"\x1b[1;3D", SpecialKey::AltArrowLeft, "CSI 1;3D is AltArrowLeft"},
        {"\x1b[1;3C", SpecialKey::AltArrowRight, "CSI 1;3C is AltArrowRight"},
        {"\x1b[1;3A", SpecialKey::AltArrowUp, "CSI 1;3A is AltArrowUp"},
        {"\x1b[1;3B", SpecialKey::AltArrowDown, "CSI 1;3B is AltArrowDown"},
    };
    for (auto& c : cases) {
        auto events = feed_all(c.bytes);
        check(events.size() == 1, c.name);
        if (events.size() == 1) {
            check(events[0].type == KeyType::Special, (std::string(c.name) + " is Special type").c_str());
            check(events[0].special == c.key, (std::string(c.name) + " key matches").c_str());
        }
    }
}

// --- F1-F12, Insert, urxvt Home/End (fix 9) ---
// Previously unrecognized -> dropped, meaning `--bind f1:...` etc. could
// never fire.

void test_function_and_misc_keys() {
    struct Case { std::string bytes; SpecialKey key; const char* name; };
    Case cases[] = {
        {"\x1bOP", SpecialKey::F1, "SS3 P is F1"},
        {"\x1bOQ", SpecialKey::F2, "SS3 Q is F2"},
        {"\x1bOR", SpecialKey::F3, "SS3 R is F3"},
        {"\x1bOS", SpecialKey::F4, "SS3 S is F4"},
        {"\x1b[11~", SpecialKey::F1, "CSI 11~ is F1"},
        {"\x1b[12~", SpecialKey::F2, "CSI 12~ is F2"},
        {"\x1b[13~", SpecialKey::F3, "CSI 13~ is F3"},
        {"\x1b[14~", SpecialKey::F4, "CSI 14~ is F4"},
        {"\x1b[15~", SpecialKey::F5, "CSI 15~ is F5"},
        {"\x1b[17~", SpecialKey::F6, "CSI 17~ is F6"},
        {"\x1b[18~", SpecialKey::F7, "CSI 18~ is F7"},
        {"\x1b[19~", SpecialKey::F8, "CSI 19~ is F8"},
        {"\x1b[20~", SpecialKey::F9, "CSI 20~ is F9"},
        {"\x1b[21~", SpecialKey::F10, "CSI 21~ is F10"},
        {"\x1b[23~", SpecialKey::F11, "CSI 23~ is F11"},
        {"\x1b[24~", SpecialKey::F12, "CSI 24~ is F12"},
        {"\x1b[2~", SpecialKey::Insert, "CSI 2~ is Insert"},
        {"\x1b[7~", SpecialKey::Home, "CSI 7~ is urxvt Home"},
        {"\x1b[8~", SpecialKey::End, "CSI 8~ is urxvt End"},
    };
    for (auto& c : cases) {
        auto events = feed_all(c.bytes);
        check(events.size() == 1, c.name);
        if (events.size() == 1) {
            check(events[0].type == KeyType::Special, (std::string(c.name) + " is Special type").c_str());
            check(events[0].special == c.key, (std::string(c.name) + " key matches").c_str());
        }
    }
}

void test_alt_letter() {
    std::string alt_a = "\x1b";
    alt_a += "a";
    auto events = feed_all(alt_a);
    check(events.size() == 1, "alt-a yields one event");
    if (events.size() == 1) {
        check(events[0].input == alt_a, "alt-a input is ESC + 'a'");
    }
}

// --- ctrl-j / Return (fix 3) ---
// 0x0d (CR) is Return; 0x0a (LF / ctrl-j) must NOT be — it needs to fall
// through to the generic C0 Character path so the dispatch layer can bind it
// to "ctrl-j" (fzf's default down-in-list) instead of it silently accepting.

void test_cr_is_return_lf_is_character() {
    auto cr = feed_all("\r");
    check(cr.size() == 1, "CR yields one event");
    if (cr.size() == 1) {
        check(cr[0].type == KeyType::Special, "CR is Special type");
        check(cr[0].special == SpecialKey::Return, "CR decodes to Return");
    }

    auto lf = feed_all("\n");
    check(lf.size() == 1, "LF (ctrl-j) yields one event");
    if (lf.size() == 1) {
        check(lf[0].type == KeyType::Character, "LF is Character type, not Return");
        check(lf[0].input == "\n", "LF Character carries the raw 0x0a byte");
    }
}

// --- Alt + multibyte UTF-8 (fix 5) ---
// The Alt branch used to always consume exactly 2 bytes; ESC + a UTF-8 lead
// byte must instead wait for the whole codepoint.

void test_alt_multibyte_utf8() {
    // Alt+ü: ESC + U+00FC 'ü' = 0xC3 0xBC.
    std::string alt_u_umlaut = "\x1b";
    alt_u_umlaut += static_cast<char>(0xC3);
    alt_u_umlaut += static_cast<char>(0xBC);
    auto events = feed_all(alt_u_umlaut);
    check(events.size() == 1, "alt-ü in one feed yields exactly one event");
    if (events.size() == 1) {
        check(events[0].type == KeyType::Character, "alt-ü is a Character event");
        check(events[0].input == alt_u_umlaut, "alt-ü input carries ESC + both UTF-8 bytes");
        check(events[0].codepoints.size() == 1 && events[0].codepoints[0] == 0x00FC,
              "alt-ü decodes to U+00FC");
    }
}

void test_alt_multibyte_utf8_split_across_feeds() {
    KeyParser p;
    std::string first_two = "\x1b";
    first_two += static_cast<char>(0xC3);
    auto e1 = p.feed(first_two);
    check(e1.empty(), "ESC + UTF-8 lead byte alone produces nothing yet");
    check(p.has_pending(), "partial alt-multibyte leaves pending state");

    auto e2 = p.feed(std::string(1, static_cast<char>(0xBC)));
    check(e2.size() == 1, "completing the UTF-8 continuation yields the event");
    if (e2.size() == 1) {
        check(e2[0].type == KeyType::Character, "completed alt-ü is a Character event");
        check(e2[0].codepoints.size() == 1 && e2[0].codepoints[0] == 0x00FC,
              "completed alt-ü decodes to U+00FC");
    }
    check(!p.has_pending(), "pending cleared after alt-multibyte completes");
}

// --- Double-ESC burst (fix 6) ---
// A pending "ESC ESC" used to be swallowed as one bogus alt-ESC character,
// losing both keypresses. Now it must resolve to one Escape, with the second
// ESC re-examined as its own event (so a trailing sequence after it, e.g. an
// arrow key, still decodes correctly).

void test_double_escape_burst() {
    auto events = feed_all("\x1b\x1b");
    check(events.size() == 1, "\"ESC ESC\" yields exactly one Escape event");
    if (events.size() == 1) {
        check(events[0].type == KeyType::Special, "double-ESC result is Special type");
        check(events[0].special == SpecialKey::Escape, "double-ESC result is Escape");
    }
}

void test_double_escape_then_arrow() {
    auto events = feed_all("\x1b\x1b[A");
    check(events.size() == 2, "\"ESC ESC [ A\" yields Escape then Up");
    if (events.size() == 2) {
        check(events[0].type == KeyType::Special && events[0].special == SpecialKey::Escape,
              "first event is Escape");
        check(events[1].type == KeyType::Special && events[1].special == SpecialKey::ArrowUp,
              "second event is ArrowUp");
    }
}

// --- ESC O force-resolve (fix 7) ---
// A pending "ESC O" with no third byte used to emit a spurious Escape on
// timeout, aborting the whole app on Alt+Shift+O. It must discard silently,
// consistent with the incomplete-CSI discard rule.

void test_esc_o_incomplete_discards_on_timeout() {
    KeyParser p;
    auto e1 = p.feed("\x1bO");
    check(e1.empty(), "\"ESC O\" alone produces nothing yet");
    check(p.has_pending(), "\"ESC O\" leaves pending state");

    auto timeout_events = p.timeout_tick(KeyParser::kEscapeTimeoutMs);
    check(timeout_events.empty(), "incomplete \"ESC O\" discards silently on timeout, no Escape");
    check(!p.has_pending(), "pending cleared after discarding incomplete SS3");
}

// --- CSI parameter overflow guard (fix 8) ---
// Accumulation must clamp so a hostile/absurdly long digit run can't trigger
// signed-overflow UB, and still yields a well-defined (clamped) param.

void test_csi_param_overflow_clamped() {
    // An absurdly long digit run as a CSI param, terminated by 'A'. This
    // doesn't match any recognized single-param table entry (params[0] clamps
    // to 65535, not a value any table entry expects), so it's dropped as an
    // unrecognized CSI. The point of the test is that accumulating 500 nines
    // doesn't crash (signed overflow UB) and the parser ends up fully drained
    // with no pending state, i.e. the terminator was found and consumed.
    std::string digits(500, '9');
    std::string seq = "\x1b[" + digits + "A";
    KeyParser p;
    auto events = p.feed(seq);
    check(events.empty(), "long digit run + unrecognized param combo drops silently");
    check(!p.has_pending(), "no pending state left after parsing an absurd digit run");

    // Follow it immediately with a real keypress in the same buffer to prove
    // the parser is still functional afterward (not wedged/corrupted).
    auto events2 = feed_all(seq + "x");
    check(events2.size() == 1, "a real key right after the overflow run still decodes");
    if (events2.size() == 1) {
        check(events2[0].input == "x", "the surviving event is the real 'x' keypress");
    }
}

void test_bare_escape_via_timeout() {
    KeyParser p;
    auto events = p.feed("\x1b");
    check(events.empty(), "bare ESC produces nothing before timeout");
    check(p.has_pending(), "bare ESC leaves pending state");

    // A bare lone ESC now gets ONE grace tick before resolving to Escape: its
    // continuation (a CSI/OSC/DCS introducer) may be arriving in the next read,
    // split across the escape timeout (e.g. chafa's terminal probes trickling
    // onto stdin). The first tick holds; the second resolves the still-bare ESC
    // as a genuine keypress. This adds at most one timeout window (~50 ms) of
    // latency to a real Esc press but stops a split control sequence's tail from
    // leaking into the query as literal text.
    auto tick1 = p.timeout_tick(KeyParser::kEscapeTimeoutMs);
    check(tick1.empty(), "bare ESC holds for one grace tick");
    check(p.has_pending(), "bare ESC still pending after first tick");

    auto timeout_events = p.timeout_tick(KeyParser::kEscapeTimeoutMs);
    check(timeout_events.size() == 1, "bare ESC resolves after second timeout tick");
    if (timeout_events.size() == 1) {
        check(timeout_events[0].type == KeyType::Special, "resolved ESC is Special type");
        check(timeout_events[0].special == SpecialKey::Escape, "resolved ESC key matches");
    }
    check(!p.has_pending(), "pending cleared after timeout resolution");
}

void test_split_across_feeds() {
    KeyParser p;
    auto e1 = p.feed("\x1b[1;5");
    check(e1.empty(), "partial CSI produces nothing yet");
    check(p.has_pending(), "partial CSI leaves pending state");

    auto e2 = p.feed("D");
    check(e2.size() == 1, "completing the CSI yields the event");
    if (e2.size() == 1) {
        check(e2[0].special == SpecialKey::CtrlArrowLeft, "split CSI decodes to CtrlArrowLeft");
    }
    check(!p.has_pending(), "pending cleared after completion");
}

void test_utf8_split_across_feeds() {
    // U+00E9 'é' = 0xC3 0xA9 in UTF-8.
    KeyParser p;
    auto e1 = p.feed(std::string(1, static_cast<char>(0xC3)));
    check(e1.empty(), "first byte of split UTF-8 produces nothing yet");
    check(p.has_pending(), "split UTF-8 leaves pending state");

    auto e2 = p.feed(std::string(1, static_cast<char>(0xA9)));
    check(e2.size() == 1, "second byte completes the UTF-8 sequence");
    if (e2.size() == 1) {
        check(e2[0].type == KeyType::Character, "completed UTF-8 char is Character type");
        check(e2[0].codepoints.size() == 1 && e2[0].codepoints[0] == 0x00E9,
              "completed UTF-8 char decodes to U+00E9");
    }
}

void test_utf8_three_byte() {
    // U+4E2D '中' = 0xE4 0xB8 0xAD in UTF-8.
    std::string bytes;
    bytes += static_cast<char>(0xE4);
    bytes += static_cast<char>(0xB8);
    bytes += static_cast<char>(0xAD);
    auto events = feed_all(bytes);
    check(events.size() == 1, "3-byte UTF-8 yields one event");
    if (events.size() == 1) {
        check(events[0].codepoints.size() == 1 && events[0].codepoints[0] == 0x4E2D,
              "3-byte UTF-8 decodes to U+4E2D");
    }
}

void test_multiple_events_in_one_feed() {
    auto events = feed_all("ab\x1b[A");
    check(events.size() == 3, "three keys in one feed produce three events");
    if (events.size() == 3) {
        check(events[0].input == "a", "first event is 'a'");
        check(events[1].input == "b", "second event is 'b'");
        check(events[2].special == SpecialKey::ArrowUp, "third event is ArrowUp");
    }
}

void test_sgr_mouse_left_click() {
    // \x1b[<0;10;5M -> button 0 (left), press, SGR reports 1-based col=10 row=5.
    // The parser converts to 0-based frame coordinates (col=9, row=4) so mouse
    // events share the renderer's 0-based origin used for click hit-testing.
    auto events = feed_all("\x1b[<0;10;5M");
    check(events.size() == 1, "SGR mouse left-click yields one event");
    if (events.size() == 1) {
        check(events[0].type == KeyType::Mouse, "mouse event type");
        check(events[0].mouse.button == MouseInfo::Button::Left, "left button decoded");
        check(events[0].mouse.motion == MouseInfo::Motion::Pressed, "press motion decoded");
        check(events[0].mouse.x == 9 && events[0].mouse.y == 4,
              "mouse coordinates decoded to 0-based");
        check(!events[0].mouse.shift && !events[0].mouse.alt && !events[0].mouse.ctrl,
              "no modifiers set");
    }
}

void test_sgr_mouse_release() {
    auto events = feed_all("\x1b[<0;10;5m");
    check(events.size() == 1, "SGR mouse release yields one event");
    if (events.size() == 1) {
        check(events[0].mouse.motion == MouseInfo::Motion::Released, "release motion decoded");
    }
}

void test_sgr_mouse_wheel() {
    // code 64 = wheel up (bit 6 set, low bits 0).
    auto up = feed_all("\x1b[<64;1;1M");
    check(up.size() == 1 && up[0].mouse.button == MouseInfo::Button::WheelUp, "wheel up decoded");

    // code 65 = wheel down (bit 6 set, low bits 1).
    auto down = feed_all("\x1b[<65;1;1M");
    check(down.size() == 1 && down[0].mouse.button == MouseInfo::Button::WheelDown,
          "wheel down decoded");
}

void test_sgr_mouse_modifiers_and_ctrl_bit() {
    // code = 0 (left) | 4 (shift) | 8 (alt) | 16 (ctrl) = 28.
    auto events = feed_all("\x1b[<28;3;4M");
    check(events.size() == 1, "SGR mouse with all modifiers yields one event");
    if (events.size() == 1) {
        check(events[0].mouse.shift, "shift bit decoded");
        check(events[0].mouse.alt, "alt bit decoded");
        check(events[0].mouse.ctrl, "ctrl bit decoded (new: previously dead in FTXUI port)");
    }
}

// --- SGR mouse motion bit (fix 1) ---
// Bit 32 of the button code marks a motion/drag report (?1002 mode). Without
// decoding it, a drag reads as repeated fresh Left presses and the
// double-click detector fires on a sloppy click + 1px wiggle.

void test_sgr_mouse_motion_bit_moved() {
    // code = 32 (motion bit) | 0 (left) = 32.
    auto events = feed_all("\x1b[<32;5;6M");
    check(events.size() == 1, "SGR mouse motion report yields one event");
    if (events.size() == 1) {
        check(events[0].mouse.button == MouseInfo::Button::Left,
              "motion report still decodes the held button (Left)");
        check(events[0].mouse.motion == MouseInfo::Motion::Moved,
              "motion bit (32) decodes to Motion::Moved");
    }
}

void test_sgr_mouse_press_not_moved() {
    auto events = feed_all("\x1b[<0;5;6M");
    check(events.size() == 1, "SGR plain press yields one event");
    if (events.size() == 1) {
        check(events[0].mouse.button == MouseInfo::Button::Left, "press button is Left");
        check(events[0].mouse.motion == MouseInfo::Motion::Pressed,
              "no motion bit -> Motion::Pressed");
    }
}

void test_sgr_mouse_release_not_moved() {
    auto events = feed_all("\x1b[<0;5;6m");
    check(events.size() == 1, "SGR release yields one event");
    if (events.size() == 1) {
        check(events[0].mouse.motion == MouseInfo::Motion::Released,
              "lowercase 'm' terminator is always Motion::Released");
    }
}

// --- X10 mouse fallback (fix 2) ---
// "ESC [ M" with no params and no '<' is the X10 encoding: three raw payload
// bytes (button+32, x+32, y+32) follow. Previously this was consumed as an
// "unrecognized CSI" and the 3 payload bytes leaked into the query as text.

void test_x10_mouse_click() {
    // Button 0 (left) + 32 = 0x20 (' '), x=1 -> 1+32=33 ('!'), y=1 -> 33 ('!').
    auto events = feed_all(std::string("\x1b[M", 3) + "\x20\x21\x21");
    check(events.size() == 1, "X10 mouse click yields one event");
    if (events.size() == 1) {
        check(events[0].type == KeyType::Mouse, "X10 click is a Mouse event");
        check(events[0].mouse.button == MouseInfo::Button::Left, "X10 click button is Left");
        check(events[0].mouse.motion == MouseInfo::Motion::Pressed, "X10 click is Pressed");
        check(events[0].mouse.x == 0 && events[0].mouse.y == 0,
              "X10 click at 1,1 decodes to 0-based 0,0");
    }
}

void test_x10_mouse_split_across_feeds() {
    KeyParser p;
    auto e1 = p.feed(std::string("\x1b[M", 3) + "\x20");
    check(e1.empty(), "X10 CSI + 1 payload byte produces nothing yet");
    check(p.has_pending(), "partial X10 mouse report leaves pending state");
    auto e2 = p.feed("\x21\x21");
    check(e2.size() == 1, "completing the X10 payload yields the event");
    if (e2.size() == 1) {
        check(e2[0].type == KeyType::Mouse, "completed X10 report is a Mouse event");
        check(e2[0].mouse.button == MouseInfo::Button::Left, "completed X10 button is Left");
    }
    check(!p.has_pending(), "pending cleared after X10 report completes");
}

void test_x10_mouse_wheel() {
    // Wheel up: btn_bits must decode to 4 -> b&3=0, b&64 set. b = 64, +32 offset = 96 ('`').
    auto events = feed_all(std::string("\x1b[M", 3) + "\x60\x21\x21");
    check(events.size() == 1, "X10 wheel report yields one event");
    if (events.size() == 1) {
        check(events[0].mouse.button == MouseInfo::Button::WheelUp, "X10 wheel-up decoded");
    }
}

// --- String-terminated sequences (OSC/DCS/APC/PM/SOS) ---
// Regression guard for the ytsurf "11;rgb;ffff/ffff/ffff" garbage: an
// unsolicited OSC background-color reply arriving on our input must be
// consumed whole, not mis-decoded as alt-] followed by literal characters.

void test_osc_bel_terminated_is_dropped() {
    // OSC 11 reply as a terminal would send it, BEL-terminated.
    auto events = feed_all("\x1b]11;rgb:ffff/ffff/ffff\x07");
    check(events.empty(), "BEL-terminated OSC reply produces no events");
}

void test_osc_st_terminated_is_dropped() {
    // Same reply, ST-terminated (ESC '\').
    auto events = feed_all("\x1b]11;rgb:1a1a/2b2b/3c3c\x1b\\");
    check(events.empty(), "ST-terminated OSC reply produces no events");
}

void test_osc_then_real_key() {
    // An OSC reply immediately followed by a genuine keypress: the reply is
    // swallowed and only the keypress surfaces.
    auto events = feed_all("\x1b]11;rgb:ffff/ffff/ffff\x07x");
    check(events.size() == 1, "OSC reply + 'x' yields exactly one event");
    if (events.size() == 1) {
        check(events[0].type == KeyType::Character && events[0].input == "x",
              "the surviving event is the real 'x' keypress");
    }
}

void test_osc_split_across_feeds() {
    // The reply can be delivered in pieces; nothing should leak mid-stream.
    KeyParser p;
    auto e1 = p.feed("\x1b]11;rgb:ffff");
    check(e1.empty(), "partial OSC produces nothing yet");
    check(p.has_pending(), "partial OSC leaves pending state");
    auto e2 = p.feed("/ffff/ffff\x07");
    check(e2.empty(), "completing the OSC still yields no events");
    check(!p.has_pending(), "pending cleared after OSC completes");
}

void test_dcs_is_dropped() {
    // DCS (ESC P ... ST), e.g. a DECRQSS / terminfo query reply.
    auto events = feed_all("\x1bP1$r0m\x1b\\");
    check(events.empty(), "DCS reply produces no events");
}

// --- pending_ growth cap (fix 10) ---
// An unterminated string sequence (OSC/DCS/APC/PM/SOS) with bytes arriving
// continuously (never triggering the force_resolve timeout path) must not
// let pending_ grow without bound. Past 64 KiB it should give up and drop the
// buffered sequence, keeping the parser functional for whatever comes next.
// Once the cap trips mid-feed, bytes still unconsumed in that same feed() are
// legitimately re-examined as fresh input (same discard-then-resume
// convention as the force_resolve path exercised by test_osc_then_real_key) —
// the invariant under test is bounded growth + no leak *before* the cap
// trips, not that every byte in an arbitrarily-long hostile burst is
// swallowed.

void test_unterminated_osc_growth_cap() {
    KeyParser p;
    // Feed well under 64 KiB of OSC payload bytes with no terminator, in
    // small increments, all via feed() (no timeout_tick) — simulating a
    // malformed source that keeps the stream continuously "ready" so
    // force_resolve never triggers. None of this may leak or grow past the
    // cap.
    std::string chunk(1024, 'x');
    std::string osc_intro = "\x1b]11;";
    auto e0 = p.feed(osc_intro);
    check(e0.empty(), "OSC introducer alone produces nothing yet");
    bool leaked_before_cap = false;
    for (int i = 0; i < 32; ++i) {  // 32 * 1024 = 32 KiB, comfortably under the cap
        auto ev = p.feed(chunk);
        if (!ev.empty()) leaked_before_cap = true;
    }
    check(!leaked_before_cap, "no characters leak while the unterminated OSC payload grows");
    check(p.has_pending(), "still buffering below the 64 KiB cap");

    // Push it well past the cap in one big feed to force the discard.
    std::string big_chunk(80 * 1024, 'y');
    p.feed(big_chunk);
    check(!p.has_pending(),
          "pending_ is dropped once the buffered sequence exceeds the 64 KiB cap "
          "(growth is bounded, not unlimited)");

    // Parser must still work normally afterward, on genuinely new input.
    auto events = p.feed("\x1b[A");
    check(events.size() == 1, "parser still decodes real input after the cap discards the OSC");
    if (events.size() == 1) {
        check(events[0].special == SpecialKey::ArrowUp, "post-cap decode is a real ArrowUp keypress");
    }
}

// The exact terminal-probe cluster chafa writes to the controlling tty when its
// stdout is a pipe (OSC fg/bg color queries + CSI window-size/DA1 capability
// probes). These land on fzf's stdin. Split across the escape timeout they used
// to leak their tail into the query as literal text (the "]10;?...[18t[0c"
// garbage on the prompt line during a preview load). None of these bytes may
// ever surface as a Character event, no matter how the cluster is fragmented.
const char* kProbeCluster =
    "\x1b]10;?\x1b\\\x1b]11;?\x1b\\\x1b[18t\x1b[14t\x1b[16t\x1b[0c";

int count_characters(const std::vector<KeyEvent>& evs) {
    int n = 0;
    for (const auto& e : evs) if (e.type == KeyType::Character) n++;
    return n;
}

void test_probe_cluster_one_feed() {
    auto events = feed_all(kProbeCluster);
    check(events.empty(), "chafa probe cluster in one feed produces no events");
}

void test_probe_cluster_split_across_feeds() {
    // A fragmented-but-continuous stream: the run-loop's select() reports input
    // ready, so each fragment arrives via feed() with NO timeout tick between
    // (a tick only fires on genuine silence). No Character may leak at any split
    // point.
    const std::string cluster(kProbeCluster);
    bool any_leak = false;
    for (size_t k = 1; k < cluster.size(); ++k) {
        KeyParser p;
        std::vector<KeyEvent> evs;
        auto a = p.feed(cluster.substr(0, k));
        auto b = p.feed(cluster.substr(k));
        evs.insert(evs.end(), a.begin(), a.end());
        evs.insert(evs.end(), b.begin(), b.end());
        if (count_characters(evs) != 0) any_leak = true;
    }
    check(!any_leak, "probe cluster never leaks a character at any feed split");
}

void test_probe_cluster_pause_between_groups() {
    // The realistic timeout case: chafa writes its OSC color queries, pauses
    // (fzf's select() times out → timeout_tick), then writes the CSI capability
    // queries. The pause straddles the escape timeout; still nothing may leak.
    const std::string cluster(kProbeCluster);
    size_t split = cluster.find("\x1b[18t");  // OSC group | CSI group boundary
    KeyParser p;
    std::vector<KeyEvent> evs;
    auto a = p.feed(cluster.substr(0, split));
    auto t = p.timeout_tick(KeyParser::kEscapeTimeoutMs);
    auto t2 = p.timeout_tick(KeyParser::kEscapeTimeoutMs);
    auto b = p.feed(cluster.substr(split));
    auto t3 = p.timeout_tick(KeyParser::kEscapeTimeoutMs);
    auto t4 = p.timeout_tick(KeyParser::kEscapeTimeoutMs);
    for (auto* v : {&a, &t, &t2, &b, &t3, &t4})
        evs.insert(evs.end(), v->begin(), v->end());
    check(count_characters(evs) == 0, "probe cluster with a pause between groups leaks nothing");
}

} // namespace

int main() {
    test_plain_ascii();
    test_ctrl_letters();
    test_ctrl_special_bytes();
    test_named_keys();
    test_function_and_misc_keys();
    test_alt_letter();
    test_cr_is_return_lf_is_character();
    test_alt_multibyte_utf8();
    test_alt_multibyte_utf8_split_across_feeds();
    test_double_escape_burst();
    test_double_escape_then_arrow();
    test_esc_o_incomplete_discards_on_timeout();
    test_csi_param_overflow_clamped();
    test_bare_escape_via_timeout();
    test_split_across_feeds();
    test_utf8_split_across_feeds();
    test_utf8_three_byte();
    test_multiple_events_in_one_feed();
    test_sgr_mouse_left_click();
    test_sgr_mouse_release();
    test_sgr_mouse_wheel();
    test_sgr_mouse_modifiers_and_ctrl_bit();
    test_sgr_mouse_motion_bit_moved();
    test_sgr_mouse_press_not_moved();
    test_sgr_mouse_release_not_moved();
    test_x10_mouse_click();
    test_x10_mouse_split_across_feeds();
    test_x10_mouse_wheel();
    test_osc_bel_terminated_is_dropped();
    test_osc_st_terminated_is_dropped();
    test_osc_then_real_key();
    test_osc_split_across_feeds();
    test_dcs_is_dropped();
    test_unterminated_osc_growth_cap();
    test_probe_cluster_one_feed();
    test_probe_cluster_split_across_feeds();
    test_probe_cluster_pause_between_groups();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
