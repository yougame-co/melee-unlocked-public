// Window-free checks of the V-Sync and FPS counter defaults inside YouGame.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_display_defaults.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cstring>

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace dd = yougame::display_defaults;

int main() {
  // Outside YouGame: the saved "vsync" key, untouched, on every platform.
  for (bool windows : {false, true}) {
    CHECK(dd::vsync(false, windows, true, -1) && !dd::vsync(false, windows, false, -1));
    CHECK(dd::vsync(false, windows, true, 0) && !dd::vsync(false, windows, false, 1));
    CHECK(!dd::own_vsync_key(false, windows));
  }
  // YouGame on Windows: an install whose file says "vsync 1" (every old save) starts with V-Sync off;
  // only the player's own choice inside YouGame turns it on, and turning it back off sticks.
  CHECK(dd::own_vsync_key(true, true));
  CHECK(!dd::vsync(true, true, true, -1) && !dd::vsync(true, true, false, -1));
  CHECK(dd::vsync(true, true, false, 1) && dd::vsync(true, true, true, 1));
  CHECK(!dd::vsync(true, true, true, 0));
  // YouGame on Linux and Mac: the saved key as before.
  CHECK(!dd::own_vsync_key(true, false));
  CHECK(dd::vsync(true, false, true, -1) && !dd::vsync(true, false, false, 1));

  for (bool agent : {false, true}) for (bool saved : {false, true})
    for (int choice : {-1, 0, 1}) {
      CHECK(dd::show_fps(false, saved, choice, agent) == saved);
      CHECK(dd::show_fps(true, saved, choice, agent) == (choice < 0 ? agent : choice == 1));
      CHECK(std::strcmp(dd::fps_source(choice, agent), choice >= 0 ? "YouGame saved choice" :
          agent ? "YouGame agent test default" : "YouGame player default") == 0);
    }
  // Every staging zip carries the agent hook, so only a run that read an agent-test.json shows the
  // counter by default; a player on the same build starts with it off.
  dd::agent_test_run = false;
  CHECK(!dd::show_fps(true, true, -1));
  CHECK(std::strcmp(dd::fps_source(-1), "YouGame player default") == 0);
  dd::agent_test_run = true;
  CHECK(dd::show_fps(true, false, -1));
  CHECK(std::strcmp(dd::fps_source(-1), "YouGame agent test default") == 0);
  dd::agent_test_run = false;
  CHECK(dd::show_fps(true, false, 1)); // Player's existing enabled toggle survives.
  std::printf("yougame display defaults: %d checks\n", checks);
  return 0;
}
