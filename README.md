# fzf++

A portable C++20 implementation of [fzf](https://github.com/junegunn/fzf),
free from a dependency on Go. It is designed as a drop-in replacement for
scripts and shell functions that drive fzf: the same command-line options,
`--bind` actions, placeholders, output protocol and exit codes, with fzf's
matching algorithms and scoring. The compatibility reference is fzf 0.74.

The only dependency is the header-only [utfcpp](https://github.com/nemtrif/utfcpp).
It builds on Linux and macOS (tested down to 10.6 on PowerPC) with any C++20
compiler, uses `select()` only, and is meant to stay fast on slow, single-core
machines: items are stored once in a chunked arena (about 38 bytes per line
of overhead), matching is cached per chunk, and keystrokes never re-scan
what a previous keystroke already rejected.

## Building

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    build/fzf --version        # 0.74 (fzf++ 0.3.1)

`ctest --test-dir build` runs the unit tests; `python3 testing/compliance_check.py
build/fzf` drives the binary under a pty and checks layout, preview and the
output contract; `testing/bench.sh` measures memory and latency.

## What works

- Input: stdin, `$FZF_DEFAULT_COMMAND`, the built-in directory walker,
  `--read0`, `--header-lines`, `--sync`, `--ansi` (escapes stripped at
  read time), `reload`/`reload-sync` that never wait for a streaming
  producer to finish.
- Search: fzf's v1/v2 fuzzy algorithms, extended-search syntax, `--exact`,
  `--case`, `--literal`, `--nth`, `--with-nth`, `--accept-nth`, `-d`,
  `--tiebreak`, `--scheme`, `--tac`, `--no-sort`, `--disabled`,
  `--filter`.
- Interaction: the complete `--bind` grammar (every fzf 0.74 key name and
  action name parses), the default keymap, `--expect`, `--multi[=N]`,
  `--cycle`, `--scroll-off`, `--query`, `--prompt`, `--header`, `--preview`
  with the full placeholder grammar (`{}`, `{+}`, `{*}`, `{n}`, `{f}`,
  `{r}`, `{s..}`, `{1..3}`, `{q}`, `{q:2}`, `{fzf:*}`, `\{}`),
  `--preview-window` position/size/hidden/`~N`/`+{n}` offsets, and the
  `FZF_*` environment for child commands.
- Actions: query editing (readline-style word motions, yank/put),
  navigation, selection, `execute`, `execute-silent`, `become`, `reload`,
  `change-prompt`/`change-header`/`change-query`/`change-preview`/
  `change-preview-window`, `transform`/`transform-*`, `preview-*`
  scrolling, `toggle-preview`, `toggle-sort`, `enable-search`/
  `disable-search`, `unbind`/`rebind`/`toggle-bind`, `trigger`, `print`,
  `print-query`, `accept-or-print-query`, `pos`, `first`/`last`, `offset-*`,
  `ctrl-z` suspend, and the `start`, `load`, `change`, `result`,
  `result-final`, `zero`, `one`, `focus`, `multi`, `backward-eof`,
  `resize`, `click-header` events.
- Output: `--print-query`, `--print0`, selection order for multi-select,
  `--select-1`, `--exit-0`, exit codes 0/1/2/126/130.

## Compatibility policy

- An implemented option, action or key behaves as in fzf 0.74.
- A known fzf 0.74 name that is not implemented is parsed and validated
  but has no effect; set `FZFPP_WARN_UNSUPPORTED=1` to be told which ones a
  script relies on. This covers `--listen`, `--tmux`, `--history`,
  jump mode, `--track`, `--tail`, `--gap`, `--style`, footer/list/input
  borders and labels, `--walker*` beyond the defaults, and the shell
  integration scripts (`--bash`/`--zsh`/`--fish`).
- An unknown option, a bad value, or an unknown action or key name prints
  fzf's message and exits with status 2.
- `--version` prints `0.74 (fzf++ X.Y.Z)`: the first token is the fzf
  version whose command line is mirrored, so scripts that check the first
  field keep working.
- The screen layout is fzf-like but not pixel-identical yet: the list is
  drawn top-down, `--height` still uses the alternate screen, and `--ansi`
  colors are not rendered. These are the next milestone.

Disclaimer: the code was written with Claude Code's assistance. Tested with
bash and Python apps such as viu (fastanime), ytsurf and ani-cli on macOS
10.6 (PowerPC), macOS 10.15 (x86_64) and Linux.
