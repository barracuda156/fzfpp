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
        // ctrl-i (0x09) is Tab, ctrl-j/ctrl-m (0x0a/0x0d) are Return, ctrl-h
        // (0x08) is Backspace — these are intentionally special-cased, not
        // plain Character events, so skip them here (covered in their own
        // tests).
        if (ctrl_byte == 0x09 || ctrl_byte == 0x0a || ctrl_byte == 0x0d || ctrl_byte == 0x08) {
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
        {"\n", SpecialKey::Return, "LF is Return"},
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
        {"\x1b[Z", SpecialKey::Tab, "CSI Z is shift-Tab"},
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

void test_alt_letter() {
    std::string alt_a = "\x1b";
    alt_a += "a";
    auto events = feed_all(alt_a);
    check(events.size() == 1, "alt-a yields one event");
    if (events.size() == 1) {
        check(events[0].input == alt_a, "alt-a input is ESC + 'a'");
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
    test_alt_letter();
    test_bare_escape_via_timeout();
    test_split_across_feeds();
    test_utf8_split_across_feeds();
    test_utf8_three_byte();
    test_multiple_events_in_one_feed();
    test_sgr_mouse_left_click();
    test_sgr_mouse_release();
    test_sgr_mouse_wheel();
    test_sgr_mouse_modifiers_and_ctrl_bit();
    test_osc_bel_terminated_is_dropped();
    test_osc_st_terminated_is_dropped();
    test_osc_then_real_key();
    test_osc_split_across_feeds();
    test_dcs_is_dropped();
    test_probe_cluster_one_feed();
    test_probe_cluster_split_across_feeds();
    test_probe_cluster_pause_between_groups();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
