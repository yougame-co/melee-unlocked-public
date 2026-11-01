// Selection policy v1, ported from project-slippi/slippi-ssbm-c (GPL-3.0).
// Reference paths below are relative to that repository at d7174ca.
#pragma once
#include "../abi/mu_ranked_wire.h"
#include <cstdint>
#include <algorithm>
#include <vector>

namespace yougame::ranked_policy {
inline MuRankedPolicy slippi_v1() {
  return {
    1, 1,
    // Scenes/Ranked/GameSetup.c:510-518 (FoD, DL, YS, FD, BF).
    {2, 28, 8, 32, 31},
    // Scenes/Ranked/GameSetup.c:521-530: BF precedes FD in the counterpick list.
    {2, 28, 8, 31, 32, 3},
    // Scenes/Ranked/GameSetup.c:253-279: P1 strikes 1, P2 strikes 2, P1 strikes 1.
    {0, 1, 0}, {1, 2, 1},
    // Scenes/Ranked/GameSetup.c:225-245: P1 color, P2 color; skipped unless mirror color.
    {0, 1},
    // Scenes/Ranked/GameSetup.c:314-357: winner ban, loser stage, winner char, loser char.
    {0, 1, 0, 1}, 1,
    1, // last-win DSR: Scenes/Ranked/GameSetup.c:1099-1105.
    1, // set-long color ban: Scenes/Ranked/GameSetup.c:217-222,1476-1502.
    {25, 25}, // Scenes/Ranked/GameSetup.c:231,245.
    {30, 30, 10}, // Scenes/Ranked/GameSetup.c:256,267,279.
    {30, 30, 45, 45}, // Scenes/Ranked/GameSetup.c:317,328,344,357.
    3, 15 // Scenes/Ranked/GameSetup.h:16-19 (grace, opponent wait).
  };
}
// v1 has no arbitrary-stage semantic mapping. Require exactly the six vanilla IDs;
// reject subsets, supersets and remix replacements before matchmaking, never randomize a substitute.
inline bool maps_stages(const std::vector<uint16_t>& stages, const MuRankedPolicy& policy) {
  std::vector<uint16_t> pool(policy.counterpicks, policy.counterpicks + 6), actual = stages;
  std::sort(pool.begin(), pool.end()); std::sort(actual.begin(), actual.end());
  return actual == pool;
}
inline std::vector<uint8_t> encode(const MuRankedPolicy& policy) {
  std::vector<uint8_t> out(MU_YG_POLICY_SIZE); mu_yg_policy_encode(out.data(), &policy); return out;
}
} // namespace yougame::ranked_policy
