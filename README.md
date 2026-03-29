# fzf++

This is a portable implementation of fzf, free from a dependency on Go.
It is designed to be an API-compatible drop-in for fzf. At the moment only a subset of features
is supported: do not expect feature parity with golang’s fzf, but it is sufficient to use
with bash and python scripts-based apps like viu (fastanime), ytsurf and ani-cli.

Disclaimer: the code was written with Claude Code’s assistance. Don’t use it in production.
I have tested the app with above-mentioned scripts on macOS 10.6 (powerpc) and 10.15 (x86_64).
It will probably work on other Unices and archs.
