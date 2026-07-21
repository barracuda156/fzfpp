#!/bin/bash
# Run on the box where you see the preview-pane bleed. Captures raw bytes of
# an ytsurf session into ~/ytsurf_bleed_<tag>.typescript for byte-level
# inspection. macOS/BSD script syntax (not GNU) -- also works on Linux script.
#
# IMPORTANT (2026-07-21): a real bug was just found and fixed on the fzfpp
# side -- it always ran --preview/--execute commands through /bin/sh (dash on
# most Linux, and dash-like on some setups) instead of $SHELL, so any
# bash-specific syntax in a preview script (ytsurf-fix.sh's preview bodies use
# `[[ ]]` throughout) could silently error out under the OLD binary. Before
# capturing, confirm:
#   1. `echo $SHELL` prints a real bash (or your intended shell), not dash/sh.
#   2. the fzf binary you're running is freshly rebuilt from a tree that has
#      this fix (look for `shell_popen`/`getenv("SHELL")` in src/terminal.cpp)
#      -- rebuilding on the SAME box you test on removes any doubt about
#      whether the fix actually reached the binary.
# If the bleed is still there after that, it's something else and this
# capture will show us what.
#
# Usage:
#   ./capture_bleed.sh [tag]
#   tag - short label for this run, e.g. "block-appleterm" or "sixel-iterm2"
#         (defaults to "run"). Lets you capture several scenarios without
#         overwriting each other.
#
# Then, with the recording running:
#   1. type a search query and press enter to get the results list
#   2. move the cursor down a few times so the preview (with a thumbnail)
#      renders for at least 2-3 different items -- do this SLOWLY once and
#      FAST once (rapid up/down) since a couple of the historical bugs here
#      only showed up on a still-streaming image
#   3. press ctrl-c or q to quit ytsurf
#   4. the script exits and prints the capture file path
set -e
TAG="${1:-run}"
OUT="$HOME/ytsurf_bleed_${TAG}.typescript"
rm -f "$OUT"
echo "SHELL=$SHELL"
echo "ytsurf: $(command -v ytsurf || echo '/opt/local/bin/ytsurf (not on PATH, using default)')"
echo "Recording to $OUT -- interact with ytsurf normally, then quit it."
script -q "$OUT" "$(command -v ytsurf || echo /opt/local/bin/ytsurf)"
echo "Done. Capture saved to: $OUT"
echo "Send this file back for analysis (it contains raw terminal bytes, not a text log)."
