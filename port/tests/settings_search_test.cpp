// The settings search's forgiving matching (settings_search.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings_search.h"

#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
using settings_search::matches;

int main() {
  check(matches("hitbox", "Hitbox colours by ID Game"), "a whole word");
  check(matches("hit", "Hitbox colours by ID Game"), "a part of a word");
  check(matches("hitbx", "Hitbox colours by ID Game"), "a letter missing");
  check(matches("hitbxo", "Hitbox colours by ID Game"), "two letters swapped");
  check(matches("shadwo", "Hide fighter shadows Game"), "a typo near the end");
  check(matches("volumne", "Volume Audio"), "an extra letter");
  check(matches("hbx", "Show attack hitboxes Overlays"), "letters in order (an abbreviation)");
  check(matches("sound", "Volume Audio"), "another word for the same thing");
  check(matches("colour", "Custom menu color Customize"), "British and American spelling");
  check(matches("lag", "Input delay frames Game"), "lag finds delay");
  check(matches("skin", "Random installed stage skin each match Mods"), "skin");
  check(matches("room look", "Room look Game"), "two words, both needed");
  check(!matches("room shadow", "Room look Game"), "every word must match");
  check(!matches("zzqx", "Volume Audio"), "nonsense matches nothing");
  check(!matches("hb", "Show attack hitboxes"), "two letters are too few for an abbreviation");
  check(!matches("vol", "Hide fighter shadows"), "a short word is not a typo of an unrelated one");
  check(!matches("##editing", "Editing"), "an empty query matches nothing");
  check(matches("fulscreen", "Fullscreen Video"), "a typo in a long word");
  std::printf(g_fail ? "settings search: %d failures\n" : "settings search: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
