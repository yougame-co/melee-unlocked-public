// Human messages for Rooms' console interruption/error codes.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
namespace yougame::console {
inline constexpr const char* kMixedBuilds = "Your friend is on a different version of Melee Unlocked. Both of you need the latest update to play together.";
inline constexpr const char* kBootTimeout = "your friend's game didn't start";
// This seat pressed Leave: it ends quietly; only the friend sees "Your friend left."
inline constexpr const char* kSelfLeft = "you left";
inline constexpr const char* kReconnecting = "Reconnecting to your friend...";
inline const char* session_notice(const std::string& reason) {
  if (reason == "left" || reason == "away" || reason == "kicked") return "Your friend left.";
  if (reason == "host_ended") return "The host ended the session.";
  if (reason == kBootTimeout) return "Your friend's game didn't start.";
  if (reason == "Couldn't get your friend's save") return "Couldn't get your friend's save";
  if (reason == kMixedBuilds) return kMixedBuilds;
  return nullptr;
}
}
