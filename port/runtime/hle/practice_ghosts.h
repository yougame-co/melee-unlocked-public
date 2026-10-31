// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace practice_ghosts {

struct Ghost {
  const char* name;        // neutral placeholder
  const char* character;   // "Falco"
  int kind;                // the character's internal fighter kind
  int games;               // games the habits come from
  int sampled;             // games read frame by frame (specials, DI, out of shield, after hitstun)
  uint8_t move[10], attack[20], defend[6], oos[10], di[6], tech[4], getup[5], ledge[6], after[5];
  uint8_t l_cancel, aggression;   // a group all zero: no data, the default stays
};

inline const Ghost kGhosts[] = {
    {"Pro 1", "Falco", 22, 0, 0, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, 0, 0},
    {"Pro 2", "Marth", 18, 0, 0, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, 0, 0},
    {"Pro 3", "Jigglypuff", 15, 0, 0, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, 0, 0},
    {"Pro 4", "Fox", 1, 0, 0, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, 0, 0},
    {"Pro 5", "Yoshi", 14, 0, 0, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, 0, 0},
};

// Keep saved indices stable. No habit data or specialist is released.
inline constexpr bool kReleased[] = {false, false, false, false, false};
static_assert(sizeof kReleased / sizeof kReleased[0] == sizeof kGhosts / sizeof kGhosts[0], "one flag per ghost");

}  // namespace practice_ghosts
