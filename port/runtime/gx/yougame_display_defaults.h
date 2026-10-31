// Display defaults when YouGame launches the game (Luis, 10-30: V-Sync off, and "an fps counter on
// by default to make it easier for people to see when there are frame drops").
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace yougame::display_defaults {

// Inside YouGame these choices live in their own keys ("vsync_yougame", "showfps_yougame"), because
// every save writes "vsync 1" on a fresh install: the old key cannot tell a player's choice from the
// old default. `saved_yougame` is -1 when the key was never saved, else 0 or 1. A launch outside
// YouGame reads and writes only the old keys, exactly as before.

// V-Sync on Windows inside YouGame: off unless the player turned it on there. In the app's full
// screen V-Sync on measured 11.5 ms input to screen against 3.4 ms off (perf-lead, 10-30).
constexpr bool own_vsync_key(bool yougame, bool windows) { return yougame && windows; }
constexpr bool vsync(bool yougame, bool windows, bool saved, int saved_yougame) {
  return own_vsync_key(yougame, windows) ? saved_yougame == 1 : saved;
}

// FPS counter inside YouGame, every platform: on unless the player turned it off there.
constexpr bool show_fps(bool yougame, bool saved, int saved_yougame) {
  return yougame ? saved_yougame != 0 : saved;
}

}  // namespace yougame::display_defaults
