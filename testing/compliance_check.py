#!/usr/bin/env python3
"""Standalone fzfpp layout/rendering compliance checker.

Drives the real fzf binary under a pty (no ytsurf, yt-x, chafa, or any other
consumer needed) and asserts on the raw bytes it writes to the terminal:
column layout consistency (results/separator/preview never gap or overlap,
with and without --border, preview-window left/right), preview-pane
column-clipping (a too-wide preview line must not spill past its pane),
border-vs-preview overshoot, and a couple of the ytsurf-specific
preview-sanitizer invariants (terminal probes / cursor-homing stripped,
CJK/wide-char width). Pure stdlib -- only needs `pty`, which is POSIX and
ships with the system Python on macOS (10.6 through current), no pip
packages required.

Usage:
    python3 compliance_check.py [/path/to/fzf]      (default: fzf on PATH)

Exit code 0 if all checks pass, 1 if any failed. Prints a PASS/FAIL line per
check plus a diagnostic on failure.
"""
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

FAILURES = []
# Names of checks that failed but were marked xfail=<task id> (expected --
# not counted as a failure) and ones that were marked xfail but passed
# anyway (XPASS -- also not a failure, just noted). See check().
XFAIL_NAMES = []
XPASS_NAMES = []
PASS_COUNT = 0


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def run_fzf(fzf_path, args, rows, cols, feed_lines, drain_s=2.0, nudge=None,
            nudge_drain_s=1.0):
    """Fork fzf under a pty, feed it `feed_lines` on stdin, capture what it
    writes to the terminal for `drain_s` seconds, optionally send `nudge`
    bytes and drain again, then kill it. Returns the captured bytes."""
    in_r, in_w = os.pipe()
    pid, master = pty.fork()
    if pid == 0:
        os.dup2(in_r, 0)
        os.close(in_r)
        os.close(in_w)
        os.environ["TERM"] = "xterm-256color"
        os.execv(fzf_path, [fzf_path] + args)
        os._exit(1)

    os.close(in_r)
    set_winsize(master, rows, cols)
    os.write(in_w, ("\n".join(feed_lines) + "\n").encode())
    os.close(in_w)

    buf = b""

    def drain(timeout):
        nonlocal buf
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.2)
            if master in r:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                buf += chunk

    drain(drain_s)
    if nudge is not None:
        try:
            os.write(master, nudge)
        except OSError:
            pass
        drain(nudge_drain_s)

    try:
        os.kill(pid, 15)
        os.waitpid(pid, 0)
    except (ProcessLookupError, ChildProcessError):
        pass
    try:
        os.close(master)
    except OSError:
        pass

    return buf.decode(errors="replace")


ENTER = b"\r"
TAB = b"\t"
UP = b"\x1b[A"
DOWN = b"\x1b[B"
CTRL_C = b"\x03"


def run_interactive(fzf, args, feed_bytes, keys, rows=20, cols=60,
                     settle=0.6, timeout=4.0, env=None):
    """Fork fzf under a pty with stdin AND stdout routed through separate
    pipes (stdin: a plain pipe fed with `feed_bytes`; stdout: piped so
    fzf's actual output-contract bytes -- the accepted item(s), --print0,
    --expect key, etc. -- can be told apart from what it drew to the
    terminal). `keys` is a list of (delay_s, bytes) pairs written to the
    pty (i.e. as if typed) with a `drain(delay_s)` after each. Returns
    (stdout_bytes, screen_bytes, exit_code); exit_code is an int for a
    normal exit, "sigN" for death by signal N, or the string
    "TIMEOUT(still running)" if it had to be SIGKILLed after `timeout`s of
    waiting for it to exit once the keys were all sent.

    fzf: src/tui/light.go et al. write the UI to /dev/tty (or whatever fd
    2/the controlling terminal is) while the OUTPUT PROTOCOL (accepted
    selection(s), --print-query, --expect key) goes to stdout -- keeping
    them on separate captured streams here mirrors that split instead of
    scraping the selection back out of the rendered screen."""
    in_r, in_w = os.pipe()
    out_r, out_w = os.pipe()
    pid, master = pty.fork()
    if pid == 0:
        os.dup2(in_r, 0)
        os.dup2(out_w, 1)
        for fd in (in_r, in_w, out_r, out_w):
            os.close(fd)
        os.environ["TERM"] = "xterm-256color"
        if env:
            os.environ.update(env)
        os.execv(fzf, [fzf] + args)
        os._exit(127)
    os.close(in_r)
    os.close(out_w)
    set_winsize(master, rows, cols)
    if feed_bytes is not None:
        try:
            os.write(in_w, feed_bytes)
        except OSError:
            pass
    os.close(in_w)

    screen = b""
    out = b""

    def drain(t):
        nonlocal screen, out
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([master, out_r], [], [], 0.05)
            if master in r:
                try:
                    c = os.read(master, 65536)
                except OSError:
                    c = b""
                if c:
                    screen += c
            if out_r in r:
                try:
                    c = os.read(out_r, 65536)
                except OSError:
                    c = b""
                if c:
                    out += c

    drain(settle)
    for delay, k in keys:
        try:
            os.write(master, k)
        except OSError:
            pass
        drain(delay)

    status = None
    end = time.time() + timeout
    while time.time() < end:
        try:
            wpid, st = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            wpid, st = pid, 0
        if wpid == pid:
            status = st
            break
        drain(0.1)
    if status is None:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        status = "TIMEOUT(still running)"
    drain(0.2)
    try:
        os.close(master)
    except OSError:
        pass
    try:
        os.close(out_r)
    except OSError:
        pass

    if isinstance(status, str):
        code = status
    elif os.WIFEXITED(status):
        code = os.WEXITSTATUS(status)
    else:
        code = f"sig{os.WTERMSIG(status)}"
    return out, screen, code


def last_frame_rows(screen, rows, cols):
    """Very rough screen model: apply CUP (cursor position) and text writes
    onto a `rows` x `cols` grid, honoring \\r/\\n and ED/EL erase sequences;
    ignores SGR/other attributes. Good enough to answer "what text is on
    row N" questions without a full terminal emulator. Returns a list of
    `rows` strings, right-trimmed of trailing spaces."""
    grid = [[" "] * cols for _ in range(rows)]
    r = c = 0
    i = 0
    s = screen.decode("utf-8", "replace")
    while i < len(s):
        ch = s[i]
        if ch == "\x1b":
            m = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", s[i:])
            if m:
                params, fin = m.group(1), m.group(2)
                if fin == "H":
                    p = [int(x) if x else 1 for x in params.split(";")] \
                        if params else [1, 1]
                    r = min(max(p[0] - 1, 0), rows - 1)
                    c = min(max((p[1] - 1 if len(p) > 1 else 0), 0), cols - 1)
                elif fin == "J":
                    if params in ("", "0", "2"):
                        grid = [[" "] * cols for _ in range(rows)]
                elif fin == "K":
                    for k in range(c, cols):
                        grid[r][k] = " "
                i += len(m.group(0))
                continue
            m = re.match(r"\x1b[()][A-Za-z0-9]|\x1b[78=>]", s[i:])
            if m:
                i += len(m.group(0))
                continue
            i += 1
            continue
        if ch == "\r":
            c = 0
        elif ch == "\n":
            r = min(r + 1, rows - 1)
        else:
            if 0 <= r < rows and 0 <= c < cols:
                grid[r][c] = ch
            c += 1
        i += 1
    return ["".join(row).rstrip() for row in grid]


def _is_extended_pictographic(cp):
    """Mirrors is_extended_pictographic() in render.cpp: the emoji-supplement
    block plus legacy dingbat/symbol codepoints with default emoji
    presentation. Keep in sync with that function."""
    return (0x1f300 <= cp <= 0x1faff) or \
        (0x2614 <= cp <= 0x2615) or (0x2648 <= cp <= 0x2653) or \
        cp == 0x267f or cp == 0x2693 or cp == 0x26a1 or \
        (0x26aa <= cp <= 0x26ab) or (0x26bd <= cp <= 0x26be) or \
        (0x26c4 <= cp <= 0x26c5) or cp == 0x26ce or cp == 0x26d4 or \
        cp == 0x26ea or (0x26f2 <= cp <= 0x26f3) or cp == 0x26f5 or \
        cp == 0x26fa or cp == 0x26fd or cp == 0x2705 or \
        (0x270a <= cp <= 0x270b) or cp == 0x2728 or cp == 0x274c or \
        cp == 0x274e or (0x2753 <= cp <= 0x2755) or cp == 0x2757 or \
        (0x2795 <= cp <= 0x2797) or cp == 0x27b0 or cp == 0x27bf or \
        (0x2b1b <= cp <= 0x2b1c) or cp == 0x2b50 or cp == 0x2b55


def _codepoint_width(cp):
    """Mirrors codepoint_width() in render.cpp. Keep in sync with that
    function."""
    if cp == 0 or cp < 0x20 or (0x7f <= cp < 0xa0):
        return 0
    if (0x0300 <= cp <= 0x036f) or (0x200b <= cp <= 0x200f) or \
       (0xfe00 <= cp <= 0xfe0f) or cp == 0xfeff or \
       (0x1f3fb <= cp <= 0x1f3ff):
        return 0
    if (0x1100 <= cp <= 0x115f) or (0x2e80 <= cp <= 0x303e) or \
       (0x3041 <= cp <= 0x33ff) or (0x3400 <= cp <= 0x4dbf) or \
       (0x4e00 <= cp <= 0x9fff) or (0xa000 <= cp <= 0xa4cf) or \
       (0xac00 <= cp <= 0xd7a3) or (0xf900 <= cp <= 0xfaff) or \
       (0xfe30 <= cp <= 0xfe4f) or (0xff00 <= cp <= 0xff60) or \
       (0xffe0 <= cp <= 0xffe6) or (0x1f1e6 <= cp <= 0x1f1ff) or \
       (0x20000 <= cp <= 0x3fffd):
        return 2
    if _is_extended_pictographic(cp):
        return 2
    if (0xe000 <= cp <= 0xf8ff) or (0xf0000 <= cp <= 0xffffd) or \
       (0x100000 <= cp <= 0x10fffd):
        return 2
    return 1


class _GraphemeWidthScanner:
    """Mirrors GraphemeWidthScanner in render.cpp: collapses ZWJ sequences
    and VS16-upgraded bases to their cluster width instead of summing each
    codepoint. Keep in sync with that class."""

    def __init__(self):
        self.prev_was_pictographic = False
        self.pending_zwj = False
        self.last_emitted_width = 0

    def consume(self, cp):
        if cp == 0x200d:  # ZWJ
            self.pending_zwj = True
            return 0
        if cp == 0xfe0f:  # VS16
            delta = 0
            if not self.prev_was_pictographic and self.last_emitted_width < 2:
                delta = 2 - self.last_emitted_width
                self.last_emitted_width = 2
            self.prev_was_pictographic = True
            return delta
        if 0xfe00 <= cp <= 0xfe0e:
            return 0

        is_pictographic = _is_extended_pictographic(cp)
        cw = _codepoint_width(cp)

        if self.pending_zwj and self.prev_was_pictographic and is_pictographic:
            self.pending_zwj = False
            return 0

        self.pending_zwj = False
        if cw != 0:
            self.prev_was_pictographic = is_pictographic
            self.last_emitted_width = cw
        return cw


def visible_width(s):
    """Display-column width matching fzfpp's codepoint_width/
    GraphemeWidthScanner (compact wcwidth + ZWJ/skin-tone-modifier
    collapsing): CJK/Hangul/fullwidth/emoji/PUA = 2, combining/controls = 0,
    ZWJ-joined emoji clusters and VS16-upgraded bases collapse to one
    cluster's width instead of summing each codepoint."""
    scanner = _GraphemeWidthScanner()
    return sum(scanner.consume(ord(ch)) for ch in s)


def strip_sgr(s):
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


def check(name, condition, detail="", xfail=None):
    """Record one check's result. `xfail`, when given, is a task id string
    (e.g. "T1.7") for a scenario that is known not to pass yet -- the task
    that is expected to make it pass. A failing xfail check prints
    `[XFAIL <id>] name` and does NOT count towards FAILURES/the exit code;
    a PASSING xfail check prints `[XPASS <id>] name` (the feature already
    works -- also not a failure, just noted so the tag can be dropped)."""
    global PASS_COUNT
    if condition:
        if xfail:
            status = f"XPASS {xfail}"
            XPASS_NAMES.append(name)
        else:
            status = "PASS"
            PASS_COUNT += 1
    else:
        if xfail:
            status = f"XFAIL {xfail}"
            XFAIL_NAMES.append(name)
        else:
            status = "FAIL"
            FAILURES.append(name)
    print(f"[{status}] {name}")
    if not condition and detail:
        print(f"       {detail}")


def test_column_layout(fzf, border, position, rows=24, cols=97, pct=35):
    """The core invariant this checker exists for: results end where the
    separator begins, and the separator sits immediately before/after the
    preview pane, for every combination of --border and preview-window
    left/right. No gap (dead, never-cleared columns) and no overshoot past
    the border. This is what caught the divergent-formula bug between
    calculate_preview_position() and repaint() (results/separator agreed,
    preview pane started up to 3 columns further right, and could overshoot
    past the border's right edge)."""
    args = ["--height=100%", f"--preview-window={position},{pct}%",
            "--preview=echo PREVIEWMARK; seq 1 5"]
    if border:
        args.insert(0, "--border")
    items = [f"item{i}" for i in range(40)]
    text = run_fzf(fzf, args, rows, cols, items,
                    drain_s=2.0, nudge=b"\x1bOB", nudge_drain_s=1.5)

    margin = 1 if border else 0
    content_cols = cols - 2 * margin
    preview_cols = (content_cols * pct) // 100
    results_width = content_cols - preview_cols - 1
    if results_width < 1:
        results_width = 1
    sep_col_rel = preview_cols if position == "left" else results_width
    sep_col_abs = margin + sep_col_rel + 1  # 1-indexed

    label = f"border={border} pos={position} {rows}x{cols}"

    # The outer --border box also uses '│' for its own left/right edges (col
    # 1 and `cols`) -- exclude those, we only care about the INTERNAL
    # results/preview separator.
    sep_positions = re.findall(r"\x1b\[(\d+);(\d+)H(?:\x1b\[0?m)?│", text)
    sep_cols_seen = sorted(set(int(c) for _, c in sep_positions
                               if int(c) not in (1, cols)))
    check(f"layout/separator-column [{label}]",
          sep_cols_seen == [sep_col_abs],
          f"expected separator only at col {sep_col_abs}, saw {sep_cols_seen}")

    idx = text.find("PREVIEWMARK")
    check(f"layout/preview-found [{label}]", idx >= 0,
          "preview text never appeared in captured output")
    if idx >= 0:
        pre = text[:idx]
        m = re.findall(r"\x1b\[(\d+);(\d+)H", pre)
        preview_col = int(m[-1][1]) if m else None
        expected_preview_col = margin + 1 if position == "left" \
            else sep_col_abs + 1
        check(f"layout/preview-start-col [{label}]",
              preview_col == expected_preview_col,
              f"expected preview text at col {expected_preview_col}, "
              f"got {preview_col}")

    # No overshoot: preview's right edge must not pass the border's right col.
    border_right_col = cols - margin
    preview_left_0idx = margin if position == "left" else margin + sep_col_rel + 1
    preview_right_edge = preview_left_0idx + preview_cols
    check(f"layout/no-overshoot [{label}]",
          preview_right_edge <= border_right_col,
          f"preview pane right edge at col {preview_right_edge} exceeds "
          f"border's right column {border_right_col}")

    # No dead gap: results_width + 1 (sep) + preview_cols must equal content_cols.
    check(f"layout/no-gap [{label}]",
          results_width + 1 + preview_cols == content_cols,
          f"results_width({results_width}) + 1 + preview_cols({preview_cols}) "
          f"!= content_cols({content_cols})")


def test_results_row_clip(fzf, rows=24, cols=80):
    """A results row's rendered width (measured in real display columns, not
    codepoints) must never exceed its budget -- this is the CJK/emoji
    "row wider than pane, terminal auto-wraps, list jumps" bug class. Uses a
    title that mixes Hangul + emoji, matching real ytsurf/yt-x content."""
    wide_title = "긴제목입니다" * 6 + "🎥" * 5  # forces overflow if mis-measured
    items = [wide_title] + [f"item{i}" for i in range(20)]
    text = run_fzf(fzf, ["--height=100%", "--preview-window=right,35%",
                          "--preview=echo x"], rows, cols, items)

    content_cols = cols
    preview_cols = (content_cols * 35) // 100
    results_width = content_cols - preview_cols - 1

    # Anchor on the wide title itself (its first few codepoints are enough to
    # find it uniquely), then walk BACK to the cursor move that started its
    # row and FORWARD to the next move that ends it -- searching from the
    # start of `text` would instead match an earlier unrelated row (e.g. the
    # info line) whose own "\x1b[...;1H" comes first in the stream.
    anchor = wide_title[:6]
    aidx = text.find(anchor)
    check("clip/wide-row-found", aidx >= 0,
          "couldn't find the wide title text in captured output at all")
    m = None
    if aidx >= 0:
        moves_before = list(re.finditer(r"\x1b\[(\d+);(\d+)H", text[:aidx]))
        check("clip/wide-row-parsed", bool(moves_before),
              "found the title but no preceding cursor move to anchor its row")
        if moves_before:
            row_start = moves_before[-1].start()
            row_end_match = re.search(r"\x1b\[\d+;\d+H", text[aidx:])
            row_end = aidx + row_end_match.start() if row_end_match else len(text)
            m = re.match(r"\x1b\[(\d+);(\d+)H(.*)", text[row_start:row_end], re.S)
    if m:
        row_content = strip_sgr(m.group(3))
        w = visible_width(row_content)
        check("clip/wide-row-width", w <= results_width,
              f"row rendered {w} display columns, budget is {results_width} "
              f"-- a wide-char row exceeding its pane can auto-wrap the "
              f"terminal and push the whole list down")


def test_legacy_emoji_width(fzf, rows=33, cols=97):
    """Regression test for a real user-reported bug (ytsurf7.png,
    2026-07-21): a title containing a legacy dingbat/symbol emoji with
    default emoji presentation (e.g. U+2B50 star) but no CJK/emoji-supplement
    codepoint measured one column narrower than it actually renders, so
    fzfpp clipped the row one character too late -- the separator (drawn
    independently at a fixed column) ended up with the row's last character
    landing ON or PAST it instead of stopping one column short. Confirms the
    separator column and the row's measured width both land where
    calculate_column_layout expects, for a title using ONLY a legacy-block
    emoji (no Hangul/CJK/emoji-supplement) so this can't be masked by
    already-covered wide ranges."""
    title = "#MANATO Got Your Back⭐ #BEFIRST @BEFIRSTOfficial"
    items = [title] + [f"item{i}" for i in range(20)]
    text = run_fzf(fzf, ["--height=100%", "--preview-window=right,50%",
                          "--preview=echo x"], rows, cols, items,
                    drain_s=1.2)

    content_cols = cols
    preview_cols = (content_cols * 50) // 100
    results_width = content_cols - preview_cols - 1
    sep_col_abs = results_width + 1

    anchor = "MANATO"
    aidx = text.find(anchor)
    check("clip/legacy-emoji-row-found", aidx >= 0,
          "couldn't find the star-emoji title in captured output")
    if aidx < 0:
        return

    moves_before = list(re.finditer(r"\x1b\[(\d+);(\d+)H", text[:aidx]))
    check("clip/legacy-emoji-row-parsed", bool(moves_before),
          "found the title but no preceding cursor move to anchor its row")
    if not moves_before:
        return

    row_start = moves_before[-1].start()
    row_end_match = re.search(r"\x1b\[\d+;\d+H", text[aidx:])
    row_end = aidx + row_end_match.start() if row_end_match else len(text)
    m = re.match(r"\x1b\[(\d+);(\d+)H(.*)", text[row_start:row_end], re.S)
    if not m:
        check("clip/legacy-emoji-row-content", False,
              "couldn't parse the row's cursor-move/content")
        return

    row_content = strip_sgr(m.group(3))
    w = visible_width(row_content)
    check("clip/legacy-emoji-row-width", w <= results_width,
          f"row measured {w} display columns, budget is {results_width} -- "
          f"a legacy-block emoji (e.g. star U+2B50) is under-measured if "
          f"this fails, letting the row spill past its pane")

    sep_positions = re.findall(r"\x1b\[(\d+);(\d+)H(?:\x1b\[0?m)?│", text)
    sep_cols_seen = sorted(set(int(c) for _, c in sep_positions
                               if int(c) not in (1, cols)))
    check("clip/legacy-emoji-separator-column", sep_cols_seen == [sep_col_abs],
          f"expected the separator only at col {sep_col_abs} even on the "
          f"star-emoji row, saw {sep_cols_seen}")


def test_zwj_and_skintone_emoji_width(fzf, rows=37, cols=106):
    """Regression test for a real user-reported bug (video WRpHQQdS_Qc's
    title, 2026-07-21): a title containing an emoji + Fitzpatrick skin-tone
    modifier (🤟🏻 = U+1F91F + U+1F3FB) and a ZWJ-joined sequence
    (❤️‍🔥 = U+2764 heart + VS16 + ZWJ + U+1F525 fire) measured WIDER than
    the terminal actually renders them, because summing each codepoint's
    width independently double-counts a skin-tone modifier (both codepoints
    individually fall in the wide 0x1f300-0x1faff range) and a ZWJ chain
    (each joined pictograph individually wide). Real terminals collapse each
    cluster to ONE glyph's width. Confirms GraphemeWidthScanner correctly
    collapses both cases so the row's measured width and the separator
    column agree with calculate_column_layout's expectation."""
    title = ("The Perfect Fukuoka Trip\U0001f91f\U0001f3fb Fukuoka Again "
              "with ILLIT❤️‍\U0001f525"
              "I #Where_is_SU_ILLIT #Odigassyu_ILLIT #EP1")
    items = [title] + [f"item{i}" for i in range(20)]
    text = run_fzf(fzf, ["--height=100%", "--preview-window=right,50%",
                          "--preview=echo x"], rows, cols, items,
                    drain_s=1.5)

    content_cols = cols
    preview_cols = (content_cols * 50) // 100
    results_width = content_cols - preview_cols - 1
    sep_col_abs = results_width + 1

    anchor = "Perfect"
    aidx = text.find(anchor)
    check("clip/zwj-row-found", aidx >= 0,
          "couldn't find the ZWJ/skin-tone-emoji title in captured output")
    if aidx < 0:
        return

    moves_before = list(re.finditer(r"\x1b\[(\d+);(\d+)H", text[:aidx]))
    check("clip/zwj-row-parsed", bool(moves_before),
          "found the title but no preceding cursor move to anchor its row")
    if not moves_before:
        return

    row_start = moves_before[-1].start()
    row_end_match = re.search(r"\x1b\[\d+;\d+H", text[aidx:])
    row_end = aidx + row_end_match.start() if row_end_match else len(text)
    m = re.match(r"\x1b\[(\d+);(\d+)H(.*)", text[row_start:row_end], re.S)
    if not m:
        check("clip/zwj-row-content", False,
              "couldn't parse the row's cursor-move/content")
        return

    row_content = strip_sgr(m.group(3))
    w = visible_width(row_content)
    # The title is long enough to always fill the results pane exactly (it
    # overflows even the widest of these test terminal sizes), so a
    # correctly-measuring build clips it to EXACTLY results_width -- not
    # just "no more than". An OVER-counting bug (skin-tone modifier or ZWJ
    # member summed instead of collapsed) clips too EARLY, under-filling the
    # row with wasted blank columns instead of overlapping the separator;
    # an UNDER-counting bug (the ⭐ class from test_legacy_emoji_width)
    # clips too LATE. Asserting equality catches both directions.
    check("clip/zwj-row-width", w == results_width,
          f"row measured {w} display columns, budget is {results_width} -- "
          f"a skin-tone modifier or ZWJ sequence is being summed instead of "
          f"collapsed to one cluster's width (row under-fills, wasting "
          f"columns) or a wide char is under-measured (row overflows)")

    sep_positions = re.findall(r"\x1b\[(\d+);(\d+)H(?:\x1b\[0?m)?│", text)
    sep_cols_seen = sorted(set(int(c) for _, c in sep_positions
                               if int(c) not in (1, cols)))
    check("clip/zwj-separator-column", sep_cols_seen == [sep_col_abs],
          f"expected the separator only at col {sep_col_abs} even on the "
          f"ZWJ/skin-tone-emoji row, saw {sep_cols_seen}")


def test_preview_line_overlong_clip(fzf, rows=24, cols=80):
    """A single preview line far longer than the pane's column budget must be
    clipped, not left to auto-wrap into (and push down) the results list.
    This is the plain-text analogue of the sixel-bleed report: if a preview
    tool (chafa block mode, or any text-based renderer) emits a row wider
    than FZF_PREVIEW_COLUMNS, fzfpp -- not the tool -- is responsible for
    clipping it before it reaches the terminal."""
    long_line = "X" * 500
    text = run_fzf(fzf, ["--height=100%", "--preview-window=right,35%",
                          f"--preview=printf '{long_line}\\n'"],
                    rows, cols, [f"item{i}" for i in range(10)],
                    drain_s=2.0, nudge=b"\x1bOB", nudge_drain_s=1.0)

    preview_cols = (cols * 35) // 100
    # Find the emitted preview row and measure it.
    idx = text.find("X" * 20)
    check("clip/overlong-preview-found", idx >= 0,
          "long preview line never appeared in output")
    if idx >= 0:
        # content runs from idx back to the preceding cursor move, forward to
        # the next escape or end of the X run.
        run_end = idx
        while run_end < len(text) and text[run_end] == "X":
            run_end += 1
        run_len = run_end - idx
        # also check nothing AFTER the X run before the next move landed on
        # the same physical row (i.e. it didn't wrap into results columns).
        check("clip/overlong-preview-clipped", run_len <= preview_cols,
              f"preview emitted {run_len} raw 'X' bytes on one row, "
              f"pane budget is {preview_cols} columns -- an unclipped "
              f"overlong line can auto-wrap the terminal and bleed into "
              f"the results pane")


def test_probe_and_cursor_home_stripped(fzf, rows=24, cols=80):
    """The ytsurf-class bugs: a preview tool's absolute cursor-home
    (ESC[H ESC[J, as ytsurf's own preview script emits) must never reach the
    terminal (it would wipe the results list), and a terminal-query OSC
    probe (as chafa emits when it detects capabilities) must never survive
    into the preview pane (forwarding it makes the terminal reply onto
    fzf's stdin, corrupting the query)."""
    preview_script = (
        r"printf '\033[H\033[J\033[1;36mTitle:\033[0m Demo"
        r"\033]10;?\033\\\033]11;?\033\\\033[18t\033[0c"
        r"\nVISIBLE_PREVIEW_TEXT\n'"
    )
    text = run_fzf(fzf, ["--height=100%", "--preview-window=right,55%",
                          f"--preview={preview_script}"],
                    rows, cols, [f"item{i}" for i in range(10)],
                    drain_s=2.0, nudge=b"\x1bOB", nudge_drain_s=1.0)

    check("sanitize/visible-text-survives", "VISIBLE_PREVIEW_TEXT" in text,
          "expected preview text never made it to the terminal at all")
    check("sanitize/cursor-home-stripped",
          "\x1b[H\x1b[J" not in text and "\x1b[1;1H\x1b[2J" not in text,
          "a raw home+clear escape reached the terminal -- would wipe the "
          "results list")
    check("sanitize/osc-query-stripped",
          "\x1b]10;?" not in text and "\x1b]11;?" not in text,
          "a terminal color-query OSC survived -- the terminal will reply "
          "onto fzf's stdin and corrupt the query/keys")
    check("sanitize/capability-query-stripped",
          "\x1b[18t" not in text and "\x1b[0c" not in text,
          "a terminal capability-query CSI survived into the preview pane")


def test_preview_uses_dollar_shell(fzf, rows=24, cols=80):
    """Preview (and execute/reload) commands must run under $SHELL, not a
    hardcoded /bin/sh -- popen(3) always execs the system shell, which on
    most Linux distros is dash, not bash. dash's printf builtin doesn't
    support bash's \\xHH hex escapes (among other differences), so a preview
    script that relies on bash-isms (chafa's own output, or hand-written
    preview scripts like ytsurf's) can silently misrender under fzfpp while
    working fine under real fzf, which always honors $SHELL. This test only
    means something when $SHELL is actually a bash (or anything with \\xHH
    printf support) different from the box's /bin/sh -- skips itself
    otherwise."""
    shell = os.environ.get("SHELL", "")
    if not shell or os.path.realpath(shell) == os.path.realpath("/bin/sh"):
        print("[SKIP] shell/respects-dollar-shell "
              "($SHELL is unset or same as /bin/sh on this system)")
        return

    preview_script = r"printf '\033[35mHEXCHECK:\033[0m'; printf '\xe2\x96\x80\n'"
    text = run_fzf(fzf, ["--height=100%", "--preview-window=right,50%",
                          f"--preview={preview_script}"],
                    rows, cols, [f"item{i}" for i in range(5)],
                    drain_s=1.5, nudge=b"\x1bOB", nudge_drain_s=1.0)

    idx = text.find("HEXCHECK:")
    check("shell/respects-dollar-shell", idx >= 0 and "▀" in text[idx:idx + 40],
          f"preview ran under a shell whose printf doesn't support \\xHH the "
          f"way $SHELL={shell!r} does -- fzfpp is likely still hardcoding "
          f"/bin/sh for preview/execute commands instead of $SHELL")


# --------------------------------------------------------------------------
# T1.11 scenarios: docs/DESIGN.md section 15 / docs/TASKS.md T1.11. Each is
# tagged with the task expected to turn it green (xfail=). A few may XPASS
# already if the underlying behavior happens to work in 0.2.1 -- that's
# fine, it just means the tag can be dropped once that task lands for real.
# --------------------------------------------------------------------------

def test_version_string(fzf):
    out, _screen, _code = run_interactive(fzf, ["--version"], b"", [],
                                           timeout=2.0)
    token = out.split()[0] if out.split() else b""
    check("version/first-token-is-fzf-version",
          bool(re.match(rb"^0\.\d+", token)),
          f"first token of `fzf --version` output was {token!r} "
          f"(fzf: a bare '0.<minor>.<patch>...' token, e.g. '0.55.0')")


def test_unknown_option_exits_2(fzf):
    _out, _screen, code = run_interactive(fzf, ["-f", "a", "--bogus"],
                                           b"a\n", [], timeout=2.0)
    check("options/unknown-option-exits-2", code == 2,
          f"exit code was {code!r}, expected 2")


def test_positional_arg_exits_2(fzf):
    _out, _screen, code = run_interactive(fzf, ["some-positional-arg"],
                                           b"a\n", [], timeout=2.0)
    check("options/positional-arg-exits-2", code == 2,
          f"exit code was {code!r}, expected 2 (fzf takes no positional "
          f"arguments)")


def test_accept_nth(fzf):
    out, _screen, _code = run_interactive(
        fzf, ["-f", "a", "--accept-nth", "2"], b"a b c\n", [], timeout=2.0)
    check("nth/accept-nth-without-delimiter", out.strip() == b"b",
          f"stdout was {out!r}, expected b'b\\n' (--accept-nth 2 with the "
          f"default AWK-style whitespace delimiter)",
          xfail="T1.3")


def test_tab_delimiter(fzf):
    # -d '\t' must parse the two-character escape as an actual tab byte,
    # not split on a literal backslash-t. Field 2 of "a\tb" is "b": a query
    # of 'b' restricted to field 2 must match, a query of 'a' restricted to
    # field 2 must not.
    out_match, _s1, _c1 = run_interactive(
        fzf, ["-f", "b", "-d", "\\t", "--nth", "2"], b"a\tb\n", [],
        timeout=2.0)
    out_nomatch, _s2, _c2 = run_interactive(
        fzf, ["-f", "a", "-d", "\\t", "--nth", "2"], b"a\tb\n", [],
        timeout=2.0)
    ok = out_match.strip() == b"a\tb" and out_nomatch.strip() == b""
    check("nth/tab-delimiter-regex", ok,
          f"query 'b' --nth 2 -d '\\t' on 'a<TAB>b' -> {out_match!r} "
          f"(expected the whole line); query 'a' --nth 2 -d '\\t' -> "
          f"{out_nomatch!r} (expected no match)",
          xfail="T1.3")


def test_nth_restricts_match(fzf):
    out, _screen, _code = run_interactive(
        fzf, ["-f", "^foo", "--nth", "2"], b"foo bar\nbar foo\n", [],
        timeout=2.0)
    check("nth/--nth-restricts-match", out.strip() == b"bar foo",
          f"stdout was {out!r}, expected b'bar foo\\n' -- field 2 of "
          f"'foo bar' is 'bar' (doesn't match ^foo), field 2 of 'bar foo' "
          f"is 'foo' (matches)",
          xfail="T1.5")


def test_tac(fzf):
    out, _screen, _code = run_interactive(fzf, ["-f", "", "--tac"],
                                           b"a\nb\nc\n", [], timeout=2.0)
    check("sort/--tac", out == b"c\nb\na\n",
          f"stdout was {out!r}, expected b'c\\nb\\na\\n' (--tac reverses "
          f"input order before matching)",
          xfail="T1.5")


def test_plus_s_keeps_input_order(fzf):
    out, _screen, _code = run_interactive(fzf, ["-f", "ab", "+s"],
                                           b"xxab\nab\n", [], timeout=2.0)
    first_line = out.split(b"\n")[0] if out else b""
    check("sort/+s-keeps-input-order", first_line == b"xxab",
          f"first line of stdout was {first_line!r}, expected b'xxab' -- "
          f"+s disables sorting, so input order (xxab before ab) is kept "
          f"even though 'ab' would score higher",
          xfail="T1.5")


def test_print0(fzf):
    out, _screen, _code = run_interactive(
        fzf, ["-f", "apple", "--print0"], b"apple\nbanana\n", [],
        timeout=2.0)
    check("output/--print0", out == b"apple\0",
          f"stdout was {out!r}, expected b'apple\\x00' (NUL-separated, "
          f"not newline-separated)",
          xfail="T1.8")


def test_selection_order(fzf):
    out, _screen, _code = run_interactive(
        fzf, ["-m", "--reverse"], b"one\ntwo\nthree\n",
        [(0.2, DOWN), (0.2, DOWN), (0.2, TAB), (0.2, UP), (0.2, UP),
         (0.2, UP), (0.2, TAB), (0.3, ENTER)])
    check("output/selection-order", out == b"three\none\n",
          f"stdout was {out!r}, expected b'three\\none\\n' (select 'three' "
          f"then 'one': fzf prints multi-selections in selection order, "
          f"not list order)",
          xfail="T1.8")


def test_header_lines_excluded(fzf):
    out, _screen, _code = run_interactive(
        fzf, ["--header-lines", "1", "--reverse"], b"HEADER\nbody\n",
        [(0.3, ENTER)])
    check("header/--header-lines-excluded-from-output", out.strip() == b"body",
          f"stdout was {out!r}, expected b'body\\n' -- the header line "
          f"must not be selectable/printable",
          xfail="T1.4")


def test_multiline_header(fzf):
    _out, screen, _code = run_interactive(
        fzf, ["--header", "line1\nline2", "--reverse"], b"one\n",
        [(0.3, ENTER)], rows=12, cols=40)
    rows_list = last_frame_rows(screen, 12, 40)
    row1 = next((i for i, r in enumerate(rows_list) if "line1" in r), None)
    row2 = next((i for i, r in enumerate(rows_list) if "line2" in r), None)
    ok = row1 is not None and row2 is not None and row1 != row2
    check("header/multiline-header-two-rows", ok,
          f"'line1' on row {row1}, 'line2' on row {row2} of the captured "
          f"frame -- expected both present on two distinct rows\n"
          + "\n".join(f"       {i:2d}|{r}" for i, r in enumerate(rows_list) if r),
          xfail="T2.2")


def test_become(fzf):
    out, _screen, code = run_interactive(
        fzf, ["--bind", "enter:become(echo BECAME {})"], b"one\ntwo\n",
        [(0.3, ENTER)], timeout=2.0)
    check("bind/become-replaces-process",
          out.strip() == b"BECAME one" and code == 0,
          f"stdout={out!r} exit={code!r}, expected stdout b'BECAME one\\n' "
          f"and exit 0 (become() execs and replaces the fzf process)",
          xfail="T1.7")


def test_execute_runs_on_tty(fzf):
    marker = os.path.join(
        tempfile.gettempdir(), f"fzfpp_compliance_execute_marker_{os.getpid()}")
    try:
        os.unlink(marker)
    except OSError:
        pass
    bind = (f"ctrl-r:execute(sh -c 'test -t 0 && test -t 1 && "
            f"touch {marker}')")
    try:
        run_interactive(fzf, ["--bind", bind], b"one\n",
                         [(0.5, b"\x12"), (0.3, ENTER)], timeout=2.0)
        ok = os.path.exists(marker)
        check("bind/execute-runs-on-tty", ok,
              f"marker file {marker} was not created -- expected "
              f"execute() to run its command with both stdin and stdout "
              f"attached to a tty (chafa/less/etc. -style previewers and "
              f"editors need this)",
              xfail="T1.7")
    finally:
        try:
            os.unlink(marker)
        except OSError:
            pass


def test_execute_silent(fzf):
    marker = os.path.join(
        tempfile.gettempdir(),
        f"fzfpp_compliance_execute_silent_marker_{os.getpid()}")
    try:
        os.unlink(marker)
    except OSError:
        pass
    bind = f"ctrl-r:execute-silent(touch {marker})"
    try:
        run_interactive(fzf, ["--bind", bind], b"one\n",
                         [(0.5, b"\x12"), (0.3, ENTER)], timeout=2.0)
        ok = os.path.exists(marker)
        check("bind/execute-silent-runs", ok,
              f"marker file {marker} was not created by "
              f"ctrl-r:execute-silent(touch {marker})",
              xfail="T1.7")
    finally:
        try:
            os.unlink(marker)
        except OSError:
            pass


def test_load_event(fzf):
    out, _screen, _code = run_interactive(
        fzf, ["--bind", "load:accept"], b"one\ntwo\n", [], timeout=1.5)
    check("bind/load-event", out.strip() == b"one",
          f"stdout was {out!r}, expected b'one\\n' -- load:accept should "
          f"fire once the initial item batch has loaded, with no keys sent",
          xfail="T1.7")


def test_space_key_name(fzf):
    out, _screen, _code = run_interactive(
        fzf, ["--bind", "space:accept"], b"one\n", [(0.3, b" ")],
        timeout=1.5)
    check("bind/space-key-name", out.strip() == b"one",
          f"stdout was {out!r}, expected b'one\\n' after pressing space "
          f"with --bind space:accept",
          xfail="T1.2")


def test_expect_f1(fzf):
    out, _screen, _code = run_interactive(
        fzf, ["--expect", "f1"], b"one\n", [(0.3, b"\x1bOP")], timeout=1.5)
    check("expect/f1-key", out == b"f1\none\n",
          f"stdout was {out!r}, expected b'f1\\none\\n' after pressing F1 "
          f"(ESC O P, the SS3 encoding xterm sends for F1) with "
          f"--expect f1",
          xfail="T1.8")


def test_placeholder_plus_and_f(fzf):
    _out, screen, _code = run_interactive(
        fzf, ["-m", "--preview", "echo PLUS[{+}] F[{f}]"], b"one\ntwo\n",
        [(0.8, ENTER)], rows=16, cols=60, timeout=2.0)
    has_plus = b"PLUS['one']" in screen
    has_f_path = bool(re.search(rb"F\[[^\]]*[/\\][^\]]*\]", screen))
    check("placeholder/plus-and-file", has_plus and has_f_path,
          f"expected the preview output to contain \"PLUS['one']\" "
          f"(has_plus={has_plus}) and \"F[<a path>]\" (has_f_path="
          f"{has_f_path})",
          xfail="T1.6")


def test_reload_does_not_block_on_streaming_stdin(fzf):
    # This one genuinely needs a shell pipeline (a slow producer feeding
    # fzf), not a single pipe write -- run_interactive always fully writes
    # feed_bytes up front, which wouldn't exercise "reader still blocked
    # mid-stream" at all.
    cmd = f"(echo a; sleep 3; echo b) | {fzf} --reverse " \
          f"--bind 'start:reload(echo RELOADED)'"
    pid, master = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.execv("/bin/sh", ["/bin/sh", "-c", cmd])
        os._exit(127)
    set_winsize(master, 20, 60)
    t0 = time.time()
    buf = b""
    seen = None
    while time.time() - t0 < 1.5:
        r, _, _ = select.select([master], [], [], 0.05)
        if master in r:
            try:
                d = os.read(master, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d
            if b"RELOADED" in buf and seen is None:
                seen = time.time() - t0
                break
    seen_desc = f"{seen:.2f}s" if seen is not None else "never (within 1.5s)"
    check("reload/does-not-block-on-streaming-stdin",
          seen is not None and seen < 1.0,
          f"RELOADED appeared after {seen_desc} (expected < 1.0s) -- "
          f"start:reload must not wait for the "
          f"still-streaming stdin producer to finish/EOF",
          xfail="T1.4")
    try:
        os.write(master, b"\x03")
    except OSError:
        pass
    time.sleep(0.2)
    try:
        wpid, _st = os.waitpid(pid, os.WNOHANG)
        if wpid == 0:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
    except (ChildProcessError, ProcessLookupError):
        pass
    try:
        os.close(master)
    except OSError:
        pass


def test_layout_reverse_prompt_on_top(fzf):
    _out, screen, _code = run_interactive(
        fzf, ["--reverse"], b"one\ntwo\nthree\n", [(0.3, ENTER)],
        rows=10, cols=40)
    rows_list = last_frame_rows(screen, 10, 40)
    # last_frame_rows() right-trims each row, so the prompt's trailing
    # space after "> " is gone by the time it gets here -- match on ">"
    # alone.
    ok = bool(rows_list) and rows_list[0].startswith(">")
    check("layout/reverse-prompt-on-top", ok,
          f"row 0 was {rows_list[0]!r} (expected it to start with '>') "
          f"-- with --reverse the prompt is the FIRST row",
          xfail="T2.2")


def test_layout_default_prompt_at_bottom(fzf):
    _out, screen, _code = run_interactive(
        fzf, [], b"one\ntwo\nthree\n", [(0.3, ENTER)], rows=10, cols=40)
    rows_list = last_frame_rows(screen, 10, 40)
    prompt_idx = next((i for i, r in enumerate(rows_list)
                        if r.startswith(">")), None)
    ok = prompt_idx is not None and prompt_idx > 0 and \
        "one" in rows_list[prompt_idx - 1]
    row_above = rows_list[prompt_idx - 1] if prompt_idx else None
    check("layout/default-prompt-at-bottom-first-item-above", ok,
          f"prompt row index={prompt_idx}, row above it={row_above!r} -- "
          f"expected the default (non-reverse) layout to put the prompt "
          f"at the bottom with the first item directly above it",
          xfail="T2.2")


def test_height_inline_no_alt_screen(fzf):
    _out, screen, _code = run_interactive(
        fzf, ["--height", "40%"], b"one\ntwo\n", [(0.3, ENTER)],
        rows=20, cols=60)
    check("height/inline-no-alt-screen", b"\x1b[?1049h" not in screen,
          f"the alternate-screen sequence ESC[?1049h appeared in the "
          f"captured output -- --height N%% (without a leading ~) must "
          f"draw inline instead",
          xfail="T2.3")


def test_ansi_colors_rendered(fzf):
    _out, screen, _code = run_interactive(
        fzf, ["--ansi"], b"\x1b[31mred\x1b[0m\nplain\n", [(0.3, ENTER)])
    check("ansi/colors-rendered", b"\x1b[31m" in screen,
          f"SGR 31 (red) from the --ansi-colored input line never "
          f"appeared in the captured output",
          xfail="T2.4")


def test_hscroll_match_kept_visible(fzf):
    long_line = "x" * 100 + "NEEDLE"
    _out, screen, _code = run_interactive(
        fzf, ["--reverse"], (long_line + "\nother\n").encode(),
        [(0.3, b"NEEDLE"), (0.5, ENTER)], cols=40)
    check("hscroll/match-kept-visible", b"NEEDLE" in screen,
          f"query 'NEEDLE' against a 100-'x'-then-NEEDLE line in a "
          f"40-column terminal never showed NEEDLE on screen -- the "
          f"matched region must stay horizontally scrolled into view",
          xfail="T2.5")


def test_preview_window_up_is_horizontal(fzf):
    _out, screen, _code = run_interactive(
        fzf, ["--preview", "echo PREVIEW-{}", "--preview-window", "up:40%"],
        b"one\ntwo\n", [(0.8, ENTER)], rows=16, cols=50)
    rows_list = last_frame_rows(screen, 16, 50)
    preview_idx = next((i for i, r in enumerate(rows_list)
                         if "PREVIEW-" in r), None)
    # "one" is also a substring of the preview's own rendered text
    # ("PREVIEW-one"), so exclude preview rows when looking for the list
    # row -- otherwise a left/right split (where both land on the same
    # physical row) would look like a false match instead of a real split.
    item_idx = next((i for i, r in enumerate(rows_list)
                      if "one" in r and "PREVIEW-" not in r), None)
    ok = preview_idx is not None and item_idx is not None and \
        preview_idx < item_idx
    check("preview/window-up-is-horizontal-split", ok,
          f"preview row index={preview_idx}, list-item row index="
          f"{item_idx} -- expected the preview pane ABOVE the list (a "
          f"horizontal split), not beside it",
          xfail="T1.10")


def test_stdin_tty_runs_default_command(fzf):
    # Deliberately does NOT go through run_interactive: that helper always
    # pipes stdin, but this scenario needs fzf's stdin left as the pty
    # itself (pty.fork()'s child inherits the pty slave as fd 0/1/2 by
    # default) so it takes the "no pipe, tty stdin" code path and falls
    # back to FZF_DEFAULT_COMMAND.
    pid, master = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["FZF_DEFAULT_COMMAND"] = "echo from-default"
        os.execv(fzf, [fzf])
        os._exit(127)
    set_winsize(master, 20, 60)
    buf = b""
    end = time.time() + 1.5
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.1)
        if master in r:
            try:
                d = os.read(master, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d
    check("stdin/tty-runs-default-command", b"from-default" in buf,
          f"expected 'from-default' (FZF_DEFAULT_COMMAND='echo "
          f"from-default') to appear on screen when fzf's stdin is a tty "
          f"(no pipe, no positional command)",
          xfail="T1.9")
    try:
        os.write(master, b"\x03")
    except OSError:
        pass
    time.sleep(0.2)
    try:
        wpid, _st = os.waitpid(pid, os.WNOHANG)
        if wpid == 0:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
    except (ChildProcessError, ProcessLookupError):
        pass
    try:
        os.close(master)
    except OSError:
        pass


def run_t1_11_scenarios(fzf):
    test_version_string(fzf)
    test_unknown_option_exits_2(fzf)
    test_positional_arg_exits_2(fzf)
    test_accept_nth(fzf)
    test_tab_delimiter(fzf)
    test_nth_restricts_match(fzf)
    test_tac(fzf)
    test_plus_s_keeps_input_order(fzf)
    test_print0(fzf)
    test_selection_order(fzf)
    test_header_lines_excluded(fzf)
    test_multiline_header(fzf)
    test_become(fzf)
    test_execute_runs_on_tty(fzf)
    test_execute_silent(fzf)
    test_load_event(fzf)
    test_space_key_name(fzf)
    test_expect_f1(fzf)
    test_placeholder_plus_and_f(fzf)
    test_reload_does_not_block_on_streaming_stdin(fzf)
    test_layout_reverse_prompt_on_top(fzf)
    test_layout_default_prompt_at_bottom(fzf)
    test_height_inline_no_alt_screen(fzf)
    test_ansi_colors_rendered(fzf)
    test_hscroll_match_kept_visible(fzf)
    test_preview_window_up_is_horizontal(fzf)
    test_stdin_tty_runs_default_command(fzf)


def main():
    fzf = sys.argv[1] if len(sys.argv) > 1 else shutil.which("fzf")
    if not fzf or not os.path.exists(fzf):
        print(f"fzf binary not found: {fzf!r}", file=sys.stderr)
        return 3

    real = shutil.which(fzf) or fzf
    print(f"testing: {real}")
    version_text = run_fzf(real, ["--version"], 24, 80, [], drain_s=0.5)
    print(f"reported version: {version_text.strip()[:80]}")
    print()

    for border in (False, True):
        for position in ("right", "left"):
            test_column_layout(real, border, position)
    test_column_layout(real, True, "right", rows=33, cols=97)  # odd size

    test_results_row_clip(real)
    test_legacy_emoji_width(real)
    test_zwj_and_skintone_emoji_width(real)
    test_preview_line_overlong_clip(real)
    test_probe_and_cursor_home_stripped(real)
    test_preview_uses_dollar_shell(real)

    print()
    run_t1_11_scenarios(real)

    print()
    print(f"{PASS_COUNT} passed, {len(XPASS_NAMES)} xpassed "
          f"(marked xfail but currently working), {len(XFAIL_NAMES)} "
          f"xfailed (expected -- see docs/TASKS.md), "
          f"{len(FAILURES)} FAILED")
    if FAILURES:
        print(f"{len(FAILURES)} check(s) FAILED:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("All checks passed (xfail/xpass do not count as failures).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
