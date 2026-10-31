// Window-free match diagnostics. Frame samples use the game's observed ping.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
namespace slippi::diagnostics {
struct Samples {
  std::array<uint64_t, 2049> bins{};
  uint64_t count = 0, maximum = 0;
  void add(uint64_t n) { ++bins[std::min<uint64_t>(n, bins.size() - 1)]; ++count; maximum = std::max(maximum, n); }
  uint64_t percentile(unsigned p) const {
    if (!count) return 0;
    const auto rank = (count * p + 99) / 100;
    uint64_t sum = 0;
    for (size_t i = 0; i < bins.size(); ++i) if ((sum += bins[i]) >= rank) return i;
    return bins.size() - 1;
  }
};
struct Summary {
  bool active = false;
  uint32_t frames = 0, max_rollback = 0;
  uint64_t held_up = 0, fell_behind = 0;
  Samples ping;
  std::string mode;
  void begin(int id, bool friends) {
    *this = {};
    active = true;
    mode = friends ? "friends" : id == 0 ? "ranked" : id == 1 ? "unranked" : id == 2 ? "direct" : id == 3 ? "teams" : "party";
  }
  std::string line(uint64_t rollbacks, uint64_t stalls, uint64_t desyncs, const char* result) const {
    char out[512];
    std::snprintf(out, sizeof out,
        "netplay summary: mode %s frames %u secs %.3f ping p50 %llu p95 %llu max %llu rollbacks %llu max-rollback %u stalls %llu held-up %llu fell-behind %llu desyncs %llu result %s",
        mode.c_str(), frames, frames / 60.0, (unsigned long long)ping.percentile(50), (unsigned long long)ping.percentile(95),
        (unsigned long long)ping.maximum, (unsigned long long)rollbacks, max_rollback, (unsigned long long)stalls,
        (unsigned long long)held_up, (unsigned long long)fell_behind, (unsigned long long)desyncs, result);
    return out;
  }
};
} // namespace slippi::diagnostics
