// The online random stage: a bag that empties before it refills, and the stage two games agree on
// when a selection names none. Doubles never plays Fountain of Dreams (its platforms and four
// players; the stage select already greys it out in Teams), so neither draw may hand it out there.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace slippi::stage_pool {

constexpr uint16_t kFountain = 0x2;

// The bag ready for a draw: stages a singles game left in it that doubles may not play are taken
// out, and an empty bag is refilled from the allowed list (without Fountain in doubles, unless the
// list has nothing else).
inline void prepare(std::vector<uint16_t>& bag, const std::vector<uint16_t>& allowed, bool doubles) {
  if (doubles) bag.erase(std::remove(bag.begin(), bag.end(), kFountain), bag.end());
  if (!bag.empty()) return;
  for (uint16_t s : allowed) if (!doubles || s != kFountain) bag.push_back(s);
  if (bag.empty()) bag = allowed;
}

// A selection arrived with no stage: the legal stage both games take, from the number both hold.
inline uint16_t agreed_stage(uint32_t agreed, bool doubles) {
  static const uint16_t singles[] = {2, 3, 8, 28, 31, 32};   // FoD, Stadium, Yoshi's, Dream Land, Battlefield, FD
  static const uint16_t teams[] = {3, 8, 28, 31, 32};
  return doubles ? teams[agreed % 5] : singles[agreed % 6];
}

} // namespace slippi::stage_pool
