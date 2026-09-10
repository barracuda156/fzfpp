#!/usr/bin/env python3
"""Performance bench helper for fzf++ (invoked by testing/bench.sh).

Drives a real fzf binary under a pty exactly like a person at a shell
would (`cat list | fzf --reverse`, 24x100, TERM=xterm-256color) and
measures the metrics in docs/DESIGN.md section 11: RSS after loading a
300k-line synthetic path list, time to first frame, ingest rate, and
keystroke-to-frame latency for a cold scan, a narrowing follow-up
keystroke, and a zero-match query. Also times `fzf -f QUERY < list`
(filter mode) and its peak RSS.

Pure stdlib (pty, select, termios, subprocess) -- no pip packages, POSIX
only. RSS is read via `ps -o rss=` for macOS/BSD portability, with
/proc/<pid>/status as an optional (Linux-only) fast path tried first.

Usage:
    bench_pty.py <path-to-fzf> <tmpdir>

Prints a markdown table to stdout. Exit code is always 0 (this is a
measurement tool, not a pass/fail check) unless the fzf binary itself
cannot be executed at all.
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

# fzf: bench models docs/DESIGN.md section 11's "300k-line synthetic path
# list (17 MB)" -- keep this formula in sync with the DESIGN.md text and
# with docs/TASKS.md T1.11 so re-runs stay comparable across changes.
N = int(os.environ.get("FZFPP_BENCH_N", 300_000))
ROWS, COLS = 24, 100
FILTER_QUERY = "modfoo"


def gen_list(path, n=N):
    with open(path, "w") as f:
        for i in range(n):
            f.write(
                f"/home/user/project-{i % 97}/src/module_{i % 1013}/"
                f"file_{('foo', 'bar', 'baz')[i % 3]}_{i}.cpp\n"
            )


def rss_kb(pid):
    """Peak-ish current RSS of `pid` in KB. Tries /proc first (cheap, no
    fork, Linux-only) then falls back to `ps -o rss=` (POSIX, works on
    macOS/BSD too) so the script stays portable per CLAUDE.md."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS"):
                    return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError):
        pass
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)], stderr=subprocess.DEVNULL,
            text=True,
        )
        out = out.strip()
        return int(out) if out else -1
    except Exception:
        return -1


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


# The info line's item count is drawn right after the row-1/col-1 cursor
# move (ESC[1;1H), directly followed by the digits -- possibly through one
# or more SGR resets (\x1b[0m) -- and is NOT preceded by anything that
# would make a \b-word-boundary regex match (that was the bug in the
# scratchpad latency_probe.py's ingest wait: "\b300000\b" never matches
# because the digits immediately follow the "H" of the escape sequence,
# which is a word character, so \b never falls between them). Match the
# digits directly off the escape sequence instead.
COUNT_RE = re.compile(rb"\x1b\[1;1H(?:\x1b\[[0-9;]*m)*\s*(\d+)")


def last_count(buf):
    """Most recent info-line item count seen anywhere in `buf`, or None."""
    m = None
    for mm in COUNT_RE.finditer(buf):
        m = mm
    return int(m.group(1)) if m else None


class Session:
    """One fzf process running under a pty, with a helper to block until a
    predicate on the accumulated output becomes true."""

    def __init__(self, fzf, args, stdin_fd, env=None):
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.dup2(stdin_fd, 0)
            os.environ["TERM"] = "xterm-256color"
            if env:
                os.environ.update(env)
            os.execv(fzf, [fzf] + args)
            os._exit(127)
        set_winsize(self.master, ROWS, COLS)
        self.buf = b""
        self.first_byte_t = None

    def drain_until(self, pred, timeout):
        """Read from the pty until `pred(self.buf)` is true or `timeout`
        elapses. Returns True if the predicate was satisfied. Checks the
        already-buffered data FIRST -- a previous call's read may have
        already pulled in bytes that satisfy this call's predicate (fzf
        can coalesce a whole ingest into one pty chunk), and waiting for
        genuinely new bytes that never come would otherwise burn the
        entire timeout instead of returning immediately."""
        if pred(self.buf):
            return True
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if self.master in r:
                try:
                    d = os.read(self.master, 65536)
                except OSError:
                    break
                if not d:
                    break
                if self.first_byte_t is None:
                    self.first_byte_t = time.time()
                self.buf += d
                if pred(self.buf):
                    return True
        return pred(self.buf)

    def send(self, data):
        os.write(self.master, data)

    def kill(self):
        try:
            self.send(b"\x03")
        except OSError:
            pass
        end = time.time() + 1.0
        exited = False
        while time.time() < end:
            try:
                wpid, _ = os.waitpid(self.pid, os.WNOHANG)
                if wpid == self.pid:
                    exited = True
                    break
            except ChildProcessError:
                exited = True
                break
            time.sleep(0.05)
        if not exited:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except (ProcessLookupError, ChildProcessError):
                pass
        try:
            os.close(self.master)
        except OSError:
            pass


def bench_interactive(fzf, list_path, n=N):
    results = {}

    # Stream the list in through a real pipe (cat | fzf), matching how a
    # shell pipeline actually feeds fzf -- a redirected regular file would
    # exercise a different (readahead-friendly, seekable) I/O path than the
    # streaming-pipe reader real usage hits.
    pipe_r, pipe_w = os.pipe()
    t_exec = time.time()
    sess = Session(fzf, ["--reverse"], pipe_r)
    os.close(pipe_r)
    cat = subprocess.Popen(["cat", list_path], stdout=pipe_w)
    os.close(pipe_w)

    sess.drain_until(lambda b: len(b) > 0, 5.0)
    results["first_frame_s"] = (
        (sess.first_byte_t - t_exec) if sess.first_byte_t else None
    )

    ok = sess.drain_until(lambda b: last_count(b) == n, 90.0)
    t_ingest = time.time() - t_exec
    try:
        cat.wait(timeout=5)
    except subprocess.TimeoutExpired:
        cat.kill()

    results["ingest_ok"] = ok
    results["ingest_s"] = t_ingest
    results["ingest_lines_per_s"] = (n / t_ingest) if t_ingest > 0 else None

    # Let any post-ingest settling (final redraw, freed scratch buffers)
    # finish before sampling RSS.
    time.sleep(0.3)
    sess.drain_until(lambda b: False, 0.2)
    rss = rss_kb(sess.pid)
    results["rss_kb"] = rss if rss and rss > 0 else None

    def keystroke_step(key_bytes, prev_count, timeout=15.0):
        t0 = time.time()
        sess.send(key_bytes)
        found = sess.drain_until(
            lambda b, pc=prev_count: (lambda c: c is not None and c != pc)(
                last_count(b)
            ),
            timeout,
        )
        dt = time.time() - t0
        c = last_count(sess.buf)
        return (dt if found else None), c

    # Cold scan: type 'z' from an empty query. Only the "baz" third of the
    # synthetic corpus contains a 'z', so this matches ~n/3 items.
    cold_s, cold_count = keystroke_step(b"z", n)
    results["key_cold_scan_s"] = cold_s
    results["key_cold_scan_count"] = cold_count
    time.sleep(0.1)
    sess.drain_until(lambda b: False, 0.15)

    # Narrowing: extend the query to "zb" on top of the previous match set
    # (what a query cache narrows from). With this exact corpus this
    # mathematically lands on 0 matches -- every 'z' comes from "baz" and
    # no line has a 'b' anywhere after that 'z' -- but it is still a real
    # query-extension redraw (count goes from ~n/3 to 0) and is timed as
    # one.
    narrow_s, narrow_count = keystroke_step(
        b"b", cold_count if cold_count is not None else n
    )
    results["key_narrowing_s"] = narrow_s
    results["key_narrowing_count"] = narrow_count
    time.sleep(0.1)
    sess.drain_until(lambda b: False, 0.15)

    # Zero-match: reset the query (ctrl-u, default unix-line-discard bind)
    # back to the full n-item list, then type 'q' alone. The letter 'q'
    # never appears anywhere in the synthetic corpus, so this is
    # guaranteed to transition n -> 0 in one keystroke. Chaining straight
    # off "zb" (already at 0 matches) would have no transition to detect
    # and would hang until timeout, hence the reset first.
    sess.send(b"\x15")
    sess.drain_until(lambda b: last_count(b) == n, 5.0)
    zero_s, zero_count = keystroke_step(b"q", n)
    results["key_zero_match_s"] = zero_s
    results["key_zero_match_count"] = zero_count

    sess.kill()
    return results


def bench_filter(fzf, list_path):
    """`fzf -f QUERY < list` wall time, plus peak RSS via `/usr/bin/time -f
    %M` when that's available (Linux util-linux/GNU time; not present on
    macOS by default, in which case RSS is reported as unavailable)."""
    results = {}
    devnull = open(os.devnull, "wb")
    time_bin = "/usr/bin/time"

    with open(list_path, "rb") as inp:
        if os.path.exists(time_bin) and os.access(time_bin, os.X_OK):
            fd, time_out_path = tempfile.mkstemp(prefix="fzfpp_bench_time_")
            os.close(fd)
            t0 = time.time()
            timed_out = False
            try:
                subprocess.run(
                    [time_bin, "-f", "%M", fzf, "-f", FILTER_QUERY],
                    stdin=inp, stdout=devnull,
                    stderr=open(time_out_path, "w"), timeout=30,
                )
            except subprocess.TimeoutExpired:
                timed_out = True
            wall = time.time() - t0
            results["filter_wall_s"] = None if timed_out else wall
            rss_mb = None
            try:
                with open(time_out_path) as f:
                    lines = [ln.strip() for ln in f if ln.strip()]
                if lines and lines[-1].isdigit():
                    rss_mb = int(lines[-1]) / 1024.0
            except OSError:
                pass
            results["filter_rss_mb"] = rss_mb
            try:
                os.unlink(time_out_path)
            except OSError:
                pass
        else:
            t0 = time.time()
            timed_out = False
            try:
                subprocess.run(
                    [fzf, "-f", FILTER_QUERY], stdin=inp, stdout=devnull,
                    timeout=30,
                )
            except subprocess.TimeoutExpired:
                timed_out = True
            results["filter_wall_s"] = None if timed_out else (time.time() - t0)
            results["filter_rss_mb"] = None  # /usr/bin/time not available

    devnull.close()
    return results


def fmt_ms(seconds):
    return f"{seconds * 1000:.1f} ms" if seconds is not None else "N/A (timeout)"


def fmt_s(seconds):
    return f"{seconds:.2f} s" if seconds is not None else "N/A (timeout)"


def fmt_mb(kb):
    return f"{kb / 1024.0:.1f} MB" if kb is not None else "N/A"


def print_table(fzf, list_path, interactive, filt, input_bytes, n=N):
    rows = []

    rss_kb_v = interactive.get("rss_kb")
    rows.append((
        f"RSS after loading {n} lines",
        fmt_mb(rss_kb_v),
        "<= 40 MB",
    ))
    if rss_kb_v is not None:
        bytes_over_input = (rss_kb_v * 1024 - input_bytes) / n
        rows.append((
            "Bytes per item over raw input",
            f"{bytes_over_input:.0f} B",
            "<= 48 B",
        ))
    else:
        rows.append(("Bytes per item over raw input", "N/A", "<= 48 B"))

    rows.append((
        "First frame after exec",
        fmt_ms(interactive.get("first_frame_s")),
        "<= 20 ms",
    ))

    ingest_ok = interactive.get("ingest_ok")
    ingest_s = interactive.get("ingest_s")
    lps = interactive.get("ingest_lines_per_s")
    ingest_val = (
        f"{ingest_s:.2f} s ({lps:,.0f} lines/s)"
        if ingest_ok and lps is not None
        else f"N/A (did not reach {n} within timeout, {ingest_s:.1f}s elapsed)"
    )
    rows.append(("Ingest: pipe to full count " + str(n), ingest_val,
                  ">= 1M lines/s"))

    rows.append((
        f"Keystroke 'z' (cold scan, count -> {interactive.get('key_cold_scan_count')})",
        fmt_ms(interactive.get("key_cold_scan_s")),
        "<= 120 ms",
    ))
    rows.append((
        f"Keystroke 'zb' (narrowing from 'z', count -> {interactive.get('key_narrowing_count')})",
        fmt_ms(interactive.get("key_narrowing_s")),
        "<= 30 ms",
    ))
    rows.append((
        f"Keystroke 'q' (zero-match from full list, count -> {interactive.get('key_zero_match_count')})",
        fmt_ms(interactive.get("key_zero_match_s")),
        "n/a",
    ))

    rows.append((
        f"`-f '{FILTER_QUERY}'` filter-mode wall time ({n} lines)",
        fmt_s(filt.get("filter_wall_s")),
        "<= 1.5 s (DESIGN.md's 1.5s budget is for 1M lines)",
    ))
    rows.append((
        "`-f` filter-mode peak RSS",
        fmt_mb(filt.get("filter_rss_mb") * 1024) if filt.get("filter_rss_mb")
        is not None else "N/A (/usr/bin/time -f %M unavailable)",
        "n/a",
    ))

    print()
    print(f"Bench: {fzf}")
    print(f"Input: {n} lines, {input_bytes / (1024*1024):.1f} MB, "
          f"terminal {ROWS}x{COLS}")
    print()
    print("| Metric | Value | Target (DESIGN.md section 11) |")
    print("|---|---|---|")
    for metric, value, target in rows:
        print(f"| {metric} | {value} | {target} |")
    print()


def main():
    if len(sys.argv) != 3:
        print("usage: bench_pty.py <path-to-fzf> <tmpdir>", file=sys.stderr)
        return 2
    fzf = os.path.abspath(sys.argv[1])
    tmpdir = sys.argv[2]
    if not os.path.exists(fzf) or not os.access(fzf, os.X_OK):
        print(f"fzf binary not found or not executable: {fzf}", file=sys.stderr)
        return 3

    list_path = os.path.join(tmpdir, "bench_list.txt")
    t0 = time.time()
    gen_list(list_path, N)
    input_bytes = os.path.getsize(list_path)
    print(f"generated {N} lines ({input_bytes / (1024*1024):.1f} MB) "
          f"in {time.time() - t0:.2f}s -> {list_path}", file=sys.stderr)

    interactive = bench_interactive(fzf, list_path, N)
    filt = bench_filter(fzf, list_path)

    print_table(fzf, list_path, interactive, filt, input_bytes, N)
    return 0


if __name__ == "__main__":
    sys.exit(main())
