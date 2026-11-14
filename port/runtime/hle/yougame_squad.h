/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#pragma once
#if MU_SQUAD
#include "../abi/mu_squad_wire.h"
#include "yougame_compatibility.h"
#include "yougame_mode_policy.h"
#include <string>

namespace yougame::squad {
// Whether this search plays Squad Strike. Unranked (mode 1) searches its own squad pool. A friend
// room (Direct, mode 2, with a room) plays squads when the row is on: the room pairs the two players,
// so the lineup is never part of a queue key and goes as 0xE3 just before each locked 0x82. Every
// pool, ticket, caps and key helper below asks with two arguments, so friend rooms keep the plain
// Friends ticket and key: two friends with different rows still meet and see why on screen.
constexpr bool selected(int mode, bool row, bool friends = false) {
  return row && (friends ? mode == 2 : mode == 1);
}
// The ONLY pool policy: moving later to ticket mode "squad" is a one-line change
// to this constant. Lobby type, afterMatch and leave handling stay Unranked's.
inline constexpr bool kPoolUsesCompatibilityTag = true;
inline const char* ticket_mode(int mode, bool row, const char* ordinary) {
  return selected(mode, row) && !kPoolUsesCompatibilityTag ? "squad" : ordinary;
}
inline std::string pool_tag(int mode, bool row, const std::string& tag, const std::string& budget) {
  if (!selected(mode, row) || !kPoolUsesCompatibilityTag) return tag;
  const std::string identity = budget.empty() ? "" : "b" + budget.substr(0, 16) + "-";
  return (tag.empty() ? "" : tag + "-") + identity + "squad";
}
constexpr bool blend_allowed(int mode, bool row) { return !selected(mode, row); }
constexpr mode_policy::MatchmakingRoute matchmaking_route(int mode, bool yougame_mode,
    bool login, bool agent, bool friends, bool vanilla, bool row) {
  if (!friends && !blend_allowed(mode, row))
    return yougame_mode ? mode_policy::MatchmakingRoute::YouGame : mode_policy::MatchmakingRoute::Locked;
  // Only the Squad Strike search itself stays off Slippi (above). Every other search on this build
  // takes the ordinary route, Slippi included.
  return mode_policy::matchmaking_route(mode, yougame_mode, login, agent, friends, vanilla);
}
// Refuse rather than let the legacy 64-byte key builder shorten any version byte.
// Include every separator and the optional rules suffix, exactly as that builder does.
inline bool key_fits(const std::string& version, bool mod, bool engine,
                     const std::string& rules, const std::string& tag) {
  return 3 + version.size() + (engine ? 3 : 0) + (mod ? 21 : 0) +
      (rules.empty() ? 0 : 10) + (tag.empty() ? 0 : 1 + tag.size()) <= 64;
}
inline std::string build_tag(const std::string& tag) {
  return tag.empty() ? "sq" : tag + "-sq";
}
inline std::string compatibility(const std::string& version, bool mod,
    const std::string& fingerprint, bool engine, const std::string& rules,
    const std::string& tag, int mode, bool row, const std::string& budget) {
  // Every key from a squad build carries "sq", in every mode: its heaps differ from OFF builds,
  // so it may never share a queue with a non-squad seat, even under the same branch tag.
  const auto combined = pool_tag(mode, row, build_tag(tag), budget);
  if (selected(mode, row) && budget.size() != 64) return {};
  if (!key_fits(version, mod, engine, rules, combined)) return {};
  return compatibility_key(version, mod, fingerprint, engine, rules, combined);
}
constexpr unsigned char caps(unsigned char original, int mode, bool row) {
  return (unsigned char)(original | (selected(mode, row) ? MU_SQUAD_CAP : 0));
}
} // namespace yougame::squad
#endif
