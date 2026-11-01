// Local Ranked UX fixture. Agent hook or explicit Ranked test branch only.
// Never starts matchmaking, contacts a ladder, or writes a rating.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
#include "yougame_ranked_policy.h"
#include "slippi_online.h"
#include <array>
#include <cstring>
namespace yougame::ranked_cpu {
inline bool enabled = false;
inline bool practice_allowed = false;
inline bool connected = true, ready = false;
inline std::array<uint8_t, 312> info{};
inline bool initialized = false;
inline bool practice = false;
inline MuYgResult practice_result{};
inline void init() {
  if (initialized) return;
  initialized = true;
  // Console GameInfoBlock, Slippi prepare_online_match_state's VS defaults.
  info[0] = 0x32; info[1] = 1; info[2] = 0x86; info[3] = 0x4C; info[4] = 0xC3;
  info[0xB] = info[0xC] = 0xFF; info[0xD] = 0x6E;
  mu_yg_put16(info.data()+0xE, 31); mu_yg_put32(info.data()+0x10, 480);
  for (unsigned i : {0x2Cu, 0x30u, 0x34u}) mu_yg_put32(info.data()+i, 0x3F800000);
  for (unsigned i = 0; i < 6; ++i) {
    auto* p = info.data()+0x60+0x24*i;
    p[0] = i == 0 ? 2 : 20; p[1] = i == 0 ? 0 : i == 1 ? 1 : 3;
    p[2] = 4; p[3] = 0; p[4] = (uint8_t)i; p[5] = 0xFF;
    p[8] = 9; p[0xA] = 0x78; p[0xC] = 0xC0; p[0xE] = 4; p[0xF] = 9;
    for (unsigned j : {0x18u, 0x1Cu, 0x20u}) mu_yg_put32(p+j, 0x3F800000);
  }
}
inline bool handle(uint8_t cmd, const uint8_t* p, unsigned n, std::vector<uint8_t>& out) {
  if (!enabled) return false;
  // Display/standing reads use the existing account path, never a fixture.
  if (practice && (cmd == 0xB9 || cmd == 0xE3)) return false;
  init(); out.clear();
  switch (cmd) {
    case 0xB9: // Signed-in display, synthetic identity, no personal account.
      out.resize(42); out[0] = 1;
      std::memcpy(out.data()+1, "Local test", 10); return true;
    case 0xFA:
      out.resize(1+6*128); out[0] = 4;
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
      out[0] |= 1;
#endif
      if (practice) {
        std::memcpy(out.data()+1, "Practice - not rated", 21);
        std::memcpy(out.data()+1+3*128, "Practice - not rated", 21);
      }
      return true;
    case MU_YG_COMMAND:
      if (n != 1) return false;
      if (p[0] == MU_YG_SUB_POLICY) { out = ranked_policy::encode(ranked_policy::slippi_v1()); return true; }
      if (p[0] != MU_YG_SUB_RANK) return false; // optional selector unknown here
      {
      MuYgRanks ranks{};
      ranks.self.known = ranks.self.placed = ranks.opponent.known = ranks.opponent.placed = 1;
      ranks.self.tier = 3; ranks.self.division = 2; ranks.self.rating = 15000;
      ranks.opponent.tier = 4; ranks.opponent.division = 1; ranks.opponent.rating = 18000;
      std::memcpy(ranks.self.label, "Gold II (test)", 15);
      std::memcpy(ranks.opponent.label, "CPU (test)", 11);
      if (practice) {
        std::vector<uint8_t> real;
        MuYgRanks actual{};
        ranks = {};
        if (slippi::online::handle(cmd, p, n, real) &&
            mu_yg_ranks_decode(&actual, real.data(), real.size()))
          ranks.self = actual.self;
        // The CPU has no ladder standing. Discard any prior room result.
        ranks.result = practice_result;
      }
      out.resize(MU_YG_RANK_RESPONSE_SIZE); mu_yg_ranks_encode(out.data(), &ranks); return true;
    }
    case 0xB3:
      out.resize(962); out[0] = connected ? 4 : 0; out[1] = out[2] = ready;
      out[4] = 1; out[9] = 2; out[13] = 7; out[14] = 10;
      if (practice) {
        // B3 is a local CPU match, but both local display fields come from the
        // real B9 identity response. Avoid B3's matchmaking lifecycle work.
        std::vector<uint8_t> identity;
        if (slippi::online::handle(0xB9, nullptr, 0, identity) && identity.size() >= 32) {
          std::memcpy(out.data()+15, identity.data()+1, 31);
          std::memcpy(out.data()+46, identity.data()+1, 31);
        }
      } else {
        std::memcpy(out.data()+15, "Local test", 10);
        std::memcpy(out.data()+46, "Local test", 10);
      }
      std::memcpy(out.data()+77, "CPU", 4); std::memcpy(out.data()+170, "CPU", 4);
      std::memcpy(out.data()+598, info.data(), info.size());
      std::memcpy(out.data()+910, "agent-ranked-cpu", 17); return true;
    case 0xB4: connected = true; return true;
    case 0xB5:
      if (n == 9) {
        if (p[3] != 2) { info[0x60] = p[1]; info[0x63] = p[2]; }
        ready = connected = true;
      }
      return true;
    case 0xBF:
      if (n == 14) {
        info[0xE] = p[0]; info[0xF] = p[1];
        for (unsigned i = 0; i < 2; ++i) if (p[2+3*i]) {
          info[0x60+0x24*i] = p[3+3*i]; info[0x63+0x24*i] = p[4+3*i];
        }
      }
      return true;
    case 0xC1: out.resize(6); return true; // game adapter's second model supplies the CPU reply
    case 0xE3: out.resize(16); out[1] = 1; return true; // ready, synthetic standing
    case 0xC0: case 0xC4: return true;
    case 0xC2: case 0xBA: connected = ready = false; return true;
    case 0xBC: out = {0,0,0,1}; return true;
    // No unhandled Slippi command may escape to the real online backend.
    default: return true;
  }
}

// This is the production host's interceptor as well as the portable test seam.
// When idle it declines every real Ranked search. While active it owns all
// match commands/reporting. Identity and rank reads retain the real account data.
inline bool intercept(uint8_t cmd, const uint8_t* p, unsigned n,
                      std::vector<uint8_t>& out, bool yougame_active) {
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
  if (yougame_active && cmd == MU_YG_COMMAND && p) {
    if (n == 1 && p[0] == MU_YG_SUB_PRACTICE_STATUS) {
      out = {static_cast<uint8_t>(practice)}; return true;
    }
    if (n == 1 && p[0] == MU_YG_SUB_PRACTICE_ALLOWED) {
      out = {static_cast<uint8_t>(practice_allowed)}; return true;
    }
    if (n == 2 && p[0] == MU_YG_SUB_PRACTICE) {
      out = {1};
      switch (p[1]) {
        case MU_YG_PRACTICE_START:
          if (!practice_allowed) { out[0] = 0; break; }
          initialized = false; init(); enabled = practice = connected = true;
          ready = false; practice_result = {}; break;
        case MU_YG_PRACTICE_STOP:
          if (practice) { enabled = practice = connected = ready = false; practice_result = {}; }
          break;
        case MU_YG_PRACTICE_WIN: case MU_YG_PRACTICE_LOSS:
          if (!practice || practice_result.status) { out[0] = 0; break; }
          // Practice is explicitly unrated: no fabricated rank or rating.
          practice_result.status = MU_YG_RESULT_UNRATED;
          break;
        default: out[0] = 0; break;
      }
      return true;
    }
  }
#else
  (void)yougame_active;
#endif
  if (!enabled) return false;
  if ((cmd >= 0xB0 && cmd <= 0xC4) || (cmd >= 0xE0 && cmd <= 0xE9) ||
      (cmd == 0xFA && n == 0) || cmd == MU_YG_COMMAND) {
    if (practice && (cmd == 0xB9 || cmd == 0xE3)) return false;
    handle(cmd, p, n, out); return true;
  }
  return false;
}
} // namespace yougame::ranked_cpu
#endif
