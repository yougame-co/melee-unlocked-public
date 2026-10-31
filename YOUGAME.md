# Melee Unlocked: YouGame's version

This is the source of the Melee Unlocked build that YouGame ships (Windows build b002c, Melee Unlocked 0.9.82), released under the GNU GPL v3 like the original.

- Original project: https://github.com/Hero88go/melee-unlocked (GPL v3). This version adds YouGame's online play (Unranked, Friends, Ranked groundwork), its own direct connection and the in-app integration.
- You need your own copy of Super Smash Bros. Melee (NTSC 1.02). No game files are in this repository.
- The decomp (`sourceport/extern/melee`) and aurora (`sourceport/extern/aurora`) are git submodules. Clone with `--recursive`.
- Six Slippi menu files (`GameSetup_gui.dat` and `slpCSS.dat` under `port/slippi_sys*/GameFiles/GALE01/`) are not included. The build makes them from your own disc with the deltas in `port/sys_deltas/`.
- Build steps are in `README.md`, `docs/` and `tools/ci/`.

Changes you make can be tried on YouGame as a crowd test build of Melee Unlocked: https://yougame.co
