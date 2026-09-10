#!/bin/bash
# Performance bench for fzf++, per docs/DESIGN.md section 11 and
# docs/TASKS.md T1.11. Generates a deterministic 300k-line synthetic path
# list, drives the given fzf binary under a pty exactly like a shell
# pipeline would (`cat list | fzf --reverse`, 24x100, TERM=xterm-256color),
# and prints a markdown table with:
#   - RSS after all 300k items are loaded
#   - time from exec to first bytes on the pty ("first frame")
#   - ingest time (exec -> info line shows the full count) and lines/s
#   - keystroke latency: cold scan ('z'), narrowing ('zb'), zero-match ('q')
#   - `fzf -f 'modfoo' < list` filter-mode wall time and peak RSS
#
# The pty mechanics live in bench_pty.py (stdlib pty/select/termios only,
# no pip packages) -- this script just resolves paths, makes a scratch
# tmpdir, and cleans up.
#
# Usage: testing/bench.sh [path/to/fzf]   (default: build/fzf)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FZF="${1:-$REPO_ROOT/build/fzf}"
if [ ! -x "$FZF" ]; then
    # allow a bare name resolved via PATH too
    resolved="$(command -v "$FZF" 2>/dev/null)"
    if [ -n "$resolved" ]; then
        FZF="$resolved"
    fi
fi
if [ ! -x "$FZF" ]; then
    echo "bench.sh: fzf binary not found or not executable: $FZF" >&2
    exit 3
fi

PY="$(command -v python3)"
if [ -z "$PY" ]; then
    echo "bench.sh: python3 not found on PATH" >&2
    exit 3
fi

TMPDIR="$(mktemp -d "${TMPDIR:-/tmp}/fzfpp_bench.XXXXXX")"
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT INT TERM

"$PY" "$SCRIPT_DIR/bench_pty.py" "$FZF" "$TMPDIR"
status=$?
exit "$status"
