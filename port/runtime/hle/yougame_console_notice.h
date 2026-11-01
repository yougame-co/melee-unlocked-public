// Human messages for Rooms' console interruption/error codes.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
namespace yougame::console {
inline constexpr const char* kMixedBuilds = "Your friend has a different version of this game.";
inline constexpr const char* kBootTimeout = "your friend's game didn't start";
inline const char* session_notice(const std::string& reason) {
  if (reason == "left" || reason == "away" || reason == "kicked") return "Your friend left.";
  if (reason == "host_ended") return "The host ended the session.";
  if (reason == kBootTimeout) return "Your friend's game didn't start.";
  if (reason == "Couldn't get your friend's save") return "Couldn't get your friend's save";
  if (reason == kMixedBuilds) return kMixedBuilds;
  return nullptr;
}
}
