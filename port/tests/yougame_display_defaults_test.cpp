// Window-free checks of the V-Sync and FPS counter defaults inside YouGame.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_display_defaults.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

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

  // FPS counter: outside YouGame the saved "showfps" key; inside, on unless turned off there.
  CHECK(!dd::show_fps(false, false, -1) && dd::show_fps(false, true, 0));
  CHECK(dd::show_fps(true, false, -1) && dd::show_fps(true, false, 1));
  CHECK(!dd::show_fps(true, true, 0));
  std::puts("yougame display defaults: ok");
  return 0;
}
