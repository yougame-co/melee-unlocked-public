// YouGame display defaults: V-Sync off on Windows; FPS hidden for players, visible by default
// Applied when a game test runs. Every staging package carries the configuration.
// agent hook, so the build flag cannot tell an agent run from a player: main() sets
// agent_test_run when it read an agent-test.json from the game's data folder.
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

inline bool agent_test_run = false;

// The existing YouGame toggle's saved choice wins over either default.
constexpr bool show_fps(bool yougame, bool saved, int saved_yougame, bool agent_test = agent_test_run) {
  return yougame ? (saved_yougame < 0 ? agent_test : saved_yougame == 1) : saved;
}
constexpr const char* fps_source(int saved_yougame, bool agent_test = agent_test_run) {
  return saved_yougame >= 0 ? "YouGame saved choice" :
         agent_test ? "YouGame agent test default" : "YouGame player default";
}

}  // namespace yougame::display_defaults
