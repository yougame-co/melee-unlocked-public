// Agent copies never use Slippi accounts, services or matchmaking peers.
// This lock is independent of diagnostic overrides and build flags.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../host/test_env.h"
#include <atomic>
#include <cstddef>

namespace host { void log(const char* fmt, ...); }
namespace slippi::agent_lock {
enum class Operation { Login, Refresh, Version, Matchmaking, PeerConnect, Http,
                       Spectator, AccountLink, Reporting, Count };
inline constexpr const char* refusal = "agent test: slippi locked";
inline bool blocked(Operation operation) {
  if (!host::test_env::agent_copy()) return false;
  static std::atomic<bool> logged[static_cast<std::size_t>(Operation::Count)]{};
  static constexpr const char* names[] = {"login", "server refresh", "version check",
      "matchmaking", "matchmaking peers", "http", "spectator", "account linking", "game reporting"};
  const auto index = static_cast<std::size_t>(operation);
  if (!logged[index].exchange(true)) host::log("agent test: slippi locked (%s)", names[index]);
  return true;
}
} // namespace slippi::agent_lock
