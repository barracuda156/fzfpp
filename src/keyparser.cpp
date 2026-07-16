#include "keyparser.hpp"

#include <utf8.h>

namespace fzf {

namespace {

// CSI/SS3 sequence -> (SpecialKey, canonical fzf key name for event_to_bind_key-
// style lookups). The canonical name isn't used here directly (KeyEvent::input
// carries the raw bytes, matching ftxui::Event::input() semantics), but the
// SpecialKey mapping is what run()'s dispatch chain pattern-matches on.
struct CsiEntry {
    const char* suffix;  // bytes after "\x1b[" (or "\x1bO" for SS3, marked below)
    bool ss3;
    SpecialKey key;
};

// clang-format off
const CsiEntry kCsiTable[] = {
    {"A", false, SpecialKey::ArrowUp},
    {"B", false, SpecialKey::ArrowDown},
    {"C", false, SpecialKey::ArrowRight},
    {"D", false, SpecialKey::ArrowLeft},
    {"H", false, SpecialKey::Home},
    {"F", false, SpecialKey::End},
    {"1~", false, SpecialKey::Home},
    {"4~", false, SpecialKey::End},
    {"3~", false, SpecialKey::Delete},
    {"5~", false, SpecialKey::PageUp},
    {"6~", false, SpecialKey::PageDown},
    {"Z", false, SpecialKey::Tab},        // shift-tab (CSI Z, no modifier param)
    {"A", true,  SpecialKey::ArrowUp},     // SS3 application-mode variants
    {"B", true,  SpecialKey::ArrowDown},
    {"C", true,  SpecialKey::ArrowRight},
    {"D", true,  SpecialKey::ArrowLeft},
    {"H", true,  SpecialKey::Home},
    {"F", true,  SpecialKey::End},
};
// clang-format on

// Modified-arrow table: CSI "1;{mod}{letter}" where mod=2 shift, 3 alt, 5 ctrl.
SpecialKey modified_arrow(int mod, char letter) {
    switch (letter) {
        case 'A':
            if (mod == 2) return SpecialKey::ShiftArrowUp;
            if (mod == 3) return SpecialKey::AltArrowUp;
            if (mod == 5) return SpecialKey::CtrlArrowUp;
            break;
        case 'B':
            if (mod == 2) return SpecialKey::ShiftArrowDown;
            if (mod == 3) return SpecialKey::AltArrowDown;
            if (mod == 5) return SpecialKey::CtrlArrowDown;
            break;
        case 'C':
            if (mod == 2) return SpecialKey::ShiftArrowRight;
            if (mod == 3) return SpecialKey::AltArrowRight;
            if (mod == 5) return SpecialKey::CtrlArrowRight;
            break;
        case 'D':
            if (mod == 2) return SpecialKey::ShiftArrowLeft;
            if (mod == 3) return SpecialKey::AltArrowLeft;
            if (mod == 5) return SpecialKey::CtrlArrowLeft;
            break;
        default:
            break;
    }
    return SpecialKey::None;
}

int utf8_sequence_length(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;  // invalid lead byte; treat as a single Latin-1 byte
}

KeyEvent decode_utf8_or_latin1(const std::string& bytes) {
    std::u32string codepoints;
    try {
        utf8::utf8to32(bytes.begin(), bytes.end(), std::back_inserter(codepoints));
    } catch (...) {
        codepoints.clear();
        for (unsigned char c : bytes) {
            codepoints.push_back(static_cast<char32_t>(c));
        }
    }
    return KeyEvent::make_character(bytes, codepoints);
}

} // namespace

bool KeyParser::try_decode_one(std::vector<KeyEvent>& out, bool force_resolve) {
    if (pending_.empty()) {
        return false;
    }

    unsigned char c0 = static_cast<unsigned char>(pending_[0]);

    // --- ESC-prefixed sequences ---
    if (c0 == 0x1b) {
        if (pending_.size() < 2) {
            if (force_resolve) {
                // A bare lone ESC times out. It's EITHER a real Esc keypress OR
                // the split head of a control sequence (an arrow key, or chafa's
                // terminal probes arriving on our stdin) whose introducer byte is
                // still in flight. The run-loop only force-resolves after select()
                // saw NO input for a whole timeout window, so a continuation that
                // was really coming arrives via feed() (select ready) rather than
                // triggering this tick. We still hedge ONE extra window: hold the
                // ESC on the first timeout; if the introducer lands during the
                // hold, feed() appends it to the still-pending ESC and the whole
                // sequence is parsed as a unit (a probe/reply is then dropped) —
                // so its tail can never leak into the query as literal text, and
                // crucially no spurious Escape is emitted. Only a SECOND timeout
                // with the ESC still bare (a full ~2× window of real silence)
                // resolves it to a genuine Esc keypress.
                if (!lone_esc_pending_) {
                    lone_esc_pending_ = true;
                    return false;  // hold one window; wait for introducer / more silence
                }
                lone_esc_pending_ = false;
                out.push_back(KeyEvent::make_special(SpecialKey::Escape, pending_));
                pending_.clear();
                return true;
            }
            return false;  // wait for more bytes (could be CSI/SS3/Alt+key)
        }
        lone_esc_pending_ = false;  // introducer present; no longer a bare ESC

        char c1 = pending_[1];

        // String-terminated sequences: OSC (ESC ]), DCS (ESC P), APC (ESC _),
        // PM (ESC ^) and SOS (ESC X). These carry a payload that runs until a
        // String Terminator — ST (ESC \) or, for OSC, a BEL (0x07). Terminals
        // emit them unsolicited (e.g. an OSC 11 "rgb:ffff/ffff/ffff" reply to a
        // background-color query issued by chafa/tmux/the shell), and the reply
        // lands on our input stream. Without this, ESC ] fell through to the
        // Alt+key branch below: it consumed only "ESC ]" as alt-], then the
        // "11;rgb:ffff/ffff/ffff" tail leaked through as literal characters and
        // corrupted the query line (the "11;rgb;ffff/ffff/ffff" garbage seen in
        // ytsurf). Consume the whole sequence and drop it.
        if (c1 == ']' || c1 == 'P' || c1 == '_' || c1 == '^' || c1 == 'X') {
            // Scan from byte 2 for the terminator.
            for (size_t j = 2; j < pending_.size(); ++j) {
                unsigned char b = static_cast<unsigned char>(pending_[j]);
                if (b == 0x07) {  // BEL terminates (common for OSC)
                    pending_.erase(0, j + 1);
                    return true;
                }
                if (b == 0x1b && j + 1 < pending_.size() && pending_[j + 1] == '\\') {
                    pending_.erase(0, j + 2);  // ST = ESC '\'
                    return true;
                }
                if (b == 0x9c) {  // single-byte ST (8-bit C1)
                    pending_.erase(0, j + 1);
                    return true;
                }
                // A bare ESC at the end could be the start of ST; wait for the
                // next byte unless we're forced to resolve now.
                if (b == 0x1b && j + 1 >= pending_.size() && !force_resolve) {
                    return false;
                }
            }
            // Terminator not seen yet. If more bytes may still come, wait. On a
            // forced resolve, discard the partial silently — its body must never
            // leak into the query. (A real OSC/DCS reply or chafa probe arrives
            // as one contiguous burst, so its ST lands in the same or the very
            // next read; a forced resolve here means the stream genuinely
            // stalled mid-sequence, and dropping it is correct.)
            if (force_resolve) {
                pending_.clear();
                return true;
            }
            return false;  // wait for more bytes to complete the sequence
        }

        // Alt+letter: ESC followed by a single printable byte, not '[' or 'O'.
        // fzf's convention (matching event_to_bind_key's "alt-<letter>" ==
        // "\x1b" + letter check) is to carry both bytes on one event rather
        // than splitting ESC and the following key into two events.
        if (c1 != '[' && c1 != 'O') {
            out.push_back(KeyEvent::make_character(pending_.substr(0, 2), U""));
            pending_.erase(0, 2);
            return true;
        }

        // SS3: ESC O <letter>
        if (c1 == 'O') {
            if (pending_.size() < 3) {
                if (force_resolve) {
                    out.push_back(KeyEvent::make_special(SpecialKey::Escape, pending_));
                    pending_.clear();
                    return true;
                }
                return false;
            }
            char c2 = pending_[2];
            for (const auto& entry : kCsiTable) {
                if (entry.ss3 && entry.suffix[0] == c2 && entry.suffix[1] == '\0') {
                    out.push_back(KeyEvent::make_special(entry.key, pending_.substr(0, 3)));
                    pending_.erase(0, 3);
                    return true;
                }
            }
            // Unrecognized SS3 sequence; drop it.
            pending_.erase(0, 3);
            return true;
        }

        // CSI: ESC [ ...
        // Scan for the terminator (first byte in 0x40-0x7E), tracking
        // whether we've seen enough bytes yet.
        size_t i = 2;
        bool sgr_mouse = false;
        if (i < pending_.size() && pending_[i] == '<') {
            sgr_mouse = true;
            i++;
        }
        size_t params_start = i;
        while (i < pending_.size()) {
            unsigned char b = static_cast<unsigned char>(pending_[i]);
            if (b >= 0x40 && b <= 0x7e) {
                break;
            }
            i++;
        }
        if (i >= pending_.size()) {
            if (force_resolve) {
                // Incomplete CSI (ESC [ … with no final byte yet). A human can't
                // type ESC[ as a keypress — this is a machine sequence (arrow
                // key, or a terminal capability probe/reply like ESC[18t / ESC[0c
                // from chafa) whose final byte hasn't arrived. Emitting Escape +
                // clearing orphans the tail so it leaks into the query as literal
                // text ("[18t" etc.). Discard the partial silently instead — its
                // bytes must never surface as characters. (A genuine arrow key
                // arrives as one read burst, well inside the timeout, so this
                // only ever drops a stalled machine sequence.)
                pending_.clear();
                return true;
            }
            return false;  // need more bytes
        }

        char terminator = pending_[i];
        std::string params_str = pending_.substr(params_start, i - params_start);
        std::string full_seq = pending_.substr(0, i + 1);

        // Parse ';'-separated integer params.
        std::vector<int> params;
        {
            int cur = 0;
            bool any_digit = false;
            for (char pc : params_str) {
                if (pc >= '0' && pc <= '9') {
                    cur = cur * 10 + (pc - '0');
                    any_digit = true;
                } else if (pc == ';') {
                    params.push_back(cur);
                    cur = 0;
                    any_digit = false;
                }
            }
            if (any_digit || !params_str.empty()) {
                params.push_back(cur);
            }
        }

        if (sgr_mouse && (terminator == 'M' || terminator == 'm')) {
            if (params.size() == 3) {
                int code = params[0];
                MouseInfo m;
                int btn_bits = (code & 3) + ((code & 64) >> 4);
                switch (btn_bits) {
                    case 0: m.button = MouseInfo::Button::Left; break;
                    case 1: m.button = MouseInfo::Button::Middle; break;
                    case 2: m.button = MouseInfo::Button::Right; break;
                    case 3: m.button = MouseInfo::Button::None; break;
                    case 4: m.button = MouseInfo::Button::WheelUp; break;
                    case 5: m.button = MouseInfo::Button::WheelDown; break;
                    default: m.button = MouseInfo::Button::None; break;
                }
                m.motion = (terminator == 'M') ? MouseInfo::Motion::Pressed
                                                : MouseInfo::Motion::Released;
                m.shift = (code & 4) != 0;
                m.alt = (code & 8) != 0;
                m.ctrl = (code & 16) != 0;
                // SGR mouse reports 1-based cell coordinates (top-left is 1;1),
                // but the renderer's frame coordinates are 0-based (move_to emits
                // row+1/col+1). Convert here so mouse.x/mouse.y share the frame's
                // 0-based origin that every consumer (click hit-testing against
                // content_top/results_start_y) assumes. Without this the click
                // landed one row too low — selecting the item below the click.
                m.x = params[1] > 0 ? params[1] - 1 : 0;
                m.y = params[2] > 0 ? params[2] - 1 : 0;
                out.push_back(KeyEvent::make_mouse(m));
            }
            pending_.erase(0, i + 1);
            return true;
        }

        // Modified arrows: params = [1, mod], terminator = A/B/C/D.
        if (params.size() == 2 && params[0] == 1 &&
            (terminator == 'A' || terminator == 'B' || terminator == 'C' || terminator == 'D')) {
            SpecialKey key = modified_arrow(params[1], terminator);
            if (key != SpecialKey::None) {
                out.push_back(KeyEvent::make_special(key, full_seq));
                pending_.erase(0, i + 1);
                return true;
            }
        }

        // Legacy shift-arrow variants without the "1;2" prefix: CSI 2 <letter>.
        if (params.size() == 1 && params[0] == 2 &&
            (terminator == 'A' || terminator == 'B' || terminator == 'C' || terminator == 'D')) {
            SpecialKey key = modified_arrow(2, terminator);
            out.push_back(KeyEvent::make_special(key, full_seq));
            pending_.erase(0, i + 1);
            return true;
        }

        // Plain CSI table lookup (arrows, Home/End, PageUp/Down, Delete, shift-tab).
        std::string suffix = params_str;
        suffix += terminator;
        for (const auto& entry : kCsiTable) {
            if (!entry.ss3 && suffix == entry.suffix) {
                out.push_back(KeyEvent::make_special(entry.key, full_seq));
                pending_.erase(0, i + 1);
                return true;
            }
        }

        // Unrecognized CSI sequence; drop it rather than misinterpreting it
        // as literal characters.
        pending_.erase(0, i + 1);
        return true;
    }

    // --- Bare control bytes and printable/UTF-8 characters ---

    // Backspace: terminals send either 0x7f (DEL) or 0x08; uniformize to a
    // single Special event.
    if (c0 == 0x7f || c0 == 0x08) {
        out.push_back(KeyEvent::make_special(SpecialKey::Backspace, pending_.substr(0, 1)));
        pending_.erase(0, 1);
        return true;
    }

    // Enter: \r (0x0d) is the common case; also accept \n (0x0a).
    if (c0 == 0x0d || c0 == 0x0a) {
        out.push_back(KeyEvent::make_special(SpecialKey::Return, pending_.substr(0, 1)));
        pending_.erase(0, 1);
        return true;
    }

    // Tab.
    if (c0 == 0x09) {
        out.push_back(KeyEvent::make_special(SpecialKey::Tab, pending_.substr(0, 1)));
        pending_.erase(0, 1);
        return true;
    }

    // Other C0 control bytes (ctrl-a..z, ctrl-space, ctrl-\, ctrl-], etc.)
    // are surfaced as Character events carrying the raw byte — matching
    // FTXUI's Event::Special-but-checked-via-input() behavior, so
    // event_to_bind_key's byte-range checks port unchanged.
    if (c0 <= 0x1f) {
        out.push_back(KeyEvent::make_character(pending_.substr(0, 1), U""));
        pending_.erase(0, 1);
        return true;
    }

    // Printable ASCII / UTF-8.
    int need = utf8_sequence_length(c0);
    if (static_cast<int>(pending_.size()) < need) {
        if (force_resolve) {
            // Incomplete multi-byte sequence with nothing more coming;
            // decode what we have (will fall back to Latin-1 per-byte).
            KeyEvent ev = decode_utf8_or_latin1(pending_);
            out.push_back(ev);
            pending_.clear();
            return true;
        }
        return false;
    }

    std::string bytes = pending_.substr(0, need);
    out.push_back(decode_utf8_or_latin1(bytes));
    pending_.erase(0, need);
    return true;
}

std::vector<KeyEvent> KeyParser::consume_available() {
    std::vector<KeyEvent> out;
    while (try_decode_one(out, /*force_resolve=*/false)) {
        // keep decoding as long as full sequences are available
    }
    return out;
}

std::vector<KeyEvent> KeyParser::feed(const std::string& bytes) {
    pending_ += bytes;
    return consume_available();
}

std::vector<KeyEvent> KeyParser::timeout_tick(int elapsed_ms) {
    std::vector<KeyEvent> out;
    if (pending_.empty() || elapsed_ms < kEscapeTimeoutMs) {
        return out;
    }
    try_decode_one(out, /*force_resolve=*/true);
    // A forced resolution only ever decides the first pending sequence;
    // anything left over is either empty or newly-available complete data,
    // so drain the rest normally.
    while (try_decode_one(out, /*force_resolve=*/false)) {
    }
    return out;
}

} // namespace fzf
