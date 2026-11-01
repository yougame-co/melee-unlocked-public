# Melee Unlocked: YouGame's version

Source candidate for YouGame build 42.79, Melee Unlocked 0.9.89,
row `0.9.89-mac-online-experimental.79`. This is build 42 plus the macOS host line: the Mac build on the main listing. Original project:
https://github.com/Hero88go/melee-unlocked (GPL-3.0-or-later).
Retain LICENSE, NOTICE and third-party notices.

You need your own Melee NTSC 1.02 disc. Game-data containers and precompiled
game payloads are omitted under YouGame's distribution policy. Any VCDIFF
files present in an unstaged candidate require a separate provenance review
and block publication until approved. The pinned decomp and aurora dependencies
are gitlinks, without bundled submodule contents.

See BUILDING.md for build-26 prerequisites, platform commands and limitations.
Older CI documentation and patch-based recipes are historical and cannot be
used unchanged for this revision. Both game and host use
`YOUGAME_RANKED_BRANCH=ON` for the main staging build.
