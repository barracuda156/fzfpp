#pragma once

#include "keyevent.hpp"
#include <string>
#include <vector>

namespace fzf {

// Decodes a stream of raw terminal input bytes into KeyEvents. Bytes may
// arrive split across multiple feed() calls (e.g. a CSI sequence spanning
// two read()s, or a UTF-8 continuation byte in a separate read()) — the
// parser buffers incomplete sequences internally until they resolve or a
// timeout_tick() call forces a decision.
//
// Not thread-safe; intended to be owned and fed by a single reader loop.
class KeyParser {
public:
    // Feed newly-read bytes; returns zero or more fully-decoded KeyEvents.
    // Any trailing incomplete sequence is buffered for the next call.
    std::vector<KeyEvent> feed(const std::string& bytes);

    // Call when poll() times out waiting for more input. If a bare ESC (or
    // an incomplete UTF-8 sequence) has been buffered longer than the
    // disambiguation window, this resolves it (ESC alone, or Latin-1
    // fallback for invalid UTF-8) and returns it as a KeyEvent. Returns an
    // empty vector if nothing is pending or the window hasn't elapsed.
    // `elapsed_ms` is the time since the buffered byte(s) first arrived, as
    // tracked by the caller (the parser holds no clock itself).
    std::vector<KeyEvent> timeout_tick(int elapsed_ms);

    // True if there is a partially-decoded sequence buffered, i.e. the
    // caller should use a short poll() timeout rather than an infinite one
    // so timeout_tick() gets a chance to run and resolve it.
    bool has_pending() const { return !pending_.empty(); }

    // Milliseconds to wait for more bytes before a bare ESC / incomplete CSI
    // is resolved via timeout_tick(). Matches common terminal convention
    // (xterm/FTXUI use a similar small window).
    static constexpr int kEscapeTimeoutMs = 50;

private:
    std::vector<KeyEvent> consume_available();
    // Attempts to decode one event starting at pending_[0]. Returns true and
    // appends to out (advancing/clearing pending_ prefix) if a full sequence
    // was decoded; returns false (leaving pending_ untouched) if more bytes
    // are needed to decide.
    bool try_decode_one(std::vector<KeyEvent>& out, bool force_resolve);

    std::string pending_;
    // A bare lone ESC that hit the escape timeout once and is being held for one
    // extra window before it resolves to an Escape keypress — so a control
    // sequence whose introducer is split just after its ESC has time to land
    // (via the next feed(), appended to this still-pending ESC) and be parsed as
    // a sequence and dropped, rather than leaking its tail into the query as
    // literal text or emitting a spurious Escape. Reset the moment a second byte
    // joins the ESC or the ESC is finally resolved.
    bool lone_esc_pending_ = false;
};

} // namespace fzf
