// Native header seams only: never inspect or launch an installed app.
// SPDX-License-Identifier: GPL-2.0-or-later
#define YOUGAME_START_TESTING
#define YOUGAME_START_IMPLEMENTATION
#include "yougame_start.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static int running, installed, launches, offers, signed_ok = 1;
static unsigned version[3] = {0, 13, 65535};
static unsigned clock_ms;
static int yg__start_running(int) { return running; }
static int yg__start_installed(int) { return installed; }
static int yg__start_app_find(int, yg__start_app*) { return installed; }
static int yg__start_app_named(yg__start_app*, int) { return 1; }
static int yg__start_app_version(yg__start_app*, unsigned have[3]) { std::memcpy(have, version, sizeof version); return 1; }
static int yg__start_app_signed(yg__start_app*) { return signed_ok; }
static int yg__start_app_launch(yg__start_app*, int) { ++launches; return 1; }
static void yg__start_app_close(yg__start_app*) {}
static int yg__start_shell_open(const char*) { CHECK(false); return 0; }
static int yg__start_offer(int) { ++offers; return 0; }
static int yg__start_quiet_env() { return 0; }
static void yg__start_sleep(unsigned ms) { clock_ms += ms; }
static unsigned long yg__start_ms() { return clock_ms; }
static int yg__start_installed_version(int, unsigned*) { return 0; }
static int yg__start_marker(int, yg__start_mark*, char*) { CHECK(false); return 0; }

int main() {
  yg_start_options options{};
  options.game = "melee-unlocked";
  options.tray_from = "0.13.1407";
  yg__start_test_macos = 1;
  installed = 1;
  options.quiet = 1;
  CHECK(yg_start(&options) == YG_START_INSTALLED && launches == 0 && offers == 0);
  options.quiet = 0;
  CHECK(yg_start(&options) == YG_START_OPENED && launches == 1 && offers == 0);
  signed_ok = 0;
  CHECK(yg_start(&options) == YG_START_DECLINED && launches == 1 && offers == 1);
  signed_ok = 1;
  version[2] = 1;
  CHECK(yg_start(&options) == YG_START_DECLINED && launches == 1 && offers == 2);
  running = 1;
  CHECK(yg_start(&options) == YG_START_RUNNING && launches == 1 && offers == 2);
  char path[] = "/Applications/YouGame.app", *argv[8];
  yg__start_mac_open_argv(path, argv);
  CHECK(!std::strcmp(argv[0], "/usr/bin/open"));
  CHECK(!std::strcmp(argv[1], "-g") && !std::strcmp(argv[2], "-j"));
  CHECK(!std::strcmp(argv[3], "-a") && argv[4] == path);
  CHECK(!std::strcmp(argv[5], "--args") && !std::strcmp(argv[6], "--yougame-tray") && !argv[7]);
  return 0;
}
