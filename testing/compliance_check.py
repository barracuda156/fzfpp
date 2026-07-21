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
import struct
import sys
import termios
import time

FAILURES = []


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


def visible_width(s):
    """Display-column width matching fzfpp's codepoint_width table (compact
    wcwidth): CJK/Hangul/fullwidth/emoji/PUA = 2, combining/controls = 0."""
    w = 0
    for ch in s:
        cp = ord(ch)
        if cp == 0 or cp < 0x20 or (0x7f <= cp < 0xa0):
            continue
        if (0x0300 <= cp <= 0x036f) or (0x200b <= cp <= 0x200f) or \
           (0xfe00 <= cp <= 0xfe0f) or cp == 0xfeff:
            continue
        if (0x1100 <= cp <= 0x115f) or (0x2e80 <= cp <= 0x303e) or \
           (0x3041 <= cp <= 0x33ff) or (0x3400 <= cp <= 0x4dbf) or \
           (0x4e00 <= cp <= 0x9fff) or (0xa000 <= cp <= 0xa4cf) or \
           (0xac00 <= cp <= 0xd7a3) or (0xf900 <= cp <= 0xfaff) or \
           (0xfe30 <= cp <= 0xfe4f) or (0xff00 <= cp <= 0xff60) or \
           (0xffe0 <= cp <= 0xffe6) or (0x1f300 <= cp <= 0x1faff) or \
           (0x20000 <= cp <= 0x3fffd):
            w += 2
            continue
        if (0xe000 <= cp <= 0xf8ff) or (0xf0000 <= cp <= 0xffffd) or \
           (0x100000 <= cp <= 0x10fffd):
            w += 2
            continue
        # Legacy symbol/dingbat codepoints with default emoji presentation
        # (Emoji_Presentation=Yes) -- see codepoint_width() in render.cpp for
        # the full citation. Keep this list in sync with that function.
        if (0x2614 <= cp <= 0x2615) or (0x2648 <= cp <= 0x2653) or \
           cp == 0x267f or cp == 0x2693 or cp == 0x26a1 or \
           (0x26aa <= cp <= 0x26ab) or (0x26bd <= cp <= 0x26be) or \
           (0x26c4 <= cp <= 0x26c5) or cp == 0x26ce or cp == 0x26d4 or \
           cp == 0x26ea or (0x26f2 <= cp <= 0x26f3) or cp == 0x26f5 or \
           cp == 0x26fa or cp == 0x26fd or cp == 0x2705 or \
           (0x270a <= cp <= 0x270b) or cp == 0x2728 or cp == 0x274c or \
           cp == 0x274e or (0x2753 <= cp <= 0x2755) or cp == 0x2757 or \
           (0x2795 <= cp <= 0x2797) or cp == 0x27b0 or cp == 0x27bf or \
           (0x2b1b <= cp <= 0x2b1c) or cp == 0x2b50 or cp == 0x2b55:
            w += 2
            continue
        w += 1
    return w


def strip_sgr(s):
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


def check(name, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}")
    if not condition:
        FAILURES.append(name)
        if detail:
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
    test_preview_line_overlong_clip(real)
    test_probe_and_cursor_home_stripped(real)
    test_preview_uses_dollar_shell(real)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} check(s) FAILED:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("All checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
