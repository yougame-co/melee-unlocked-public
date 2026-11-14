// Human messages for Rooms' console interruption/error codes.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
namespace yougame::console {
inline constexpr const char* kMixedBuilds = "Different Melee Unlocked versions: update from YouGame, or play someone on the same version.";
inline constexpr const char* kBootTimeout = "your friend's game didn't start";
// This seat pressed Leave: it ends quietly; only the friend sees "Your friend left."
inline constexpr const char* kSelfLeft = "you left";
inline constexpr const char* kWaitingForFriend = "Waiting for your friend...";
inline constexpr const char* kReconnecting = "Reconnecting to your friend...";
// Doubles has three other players, not one friend.
inline constexpr const char* kWaitingForPlayers = "Waiting for the other players...";
inline constexpr const char* kReconnectingPlayers = "Reconnecting to the other players...";
// The app could not start its link helper (no_direct), so this computer cannot join any online match.
// The game can't tell which cause it hit (on Linux, often a system package the app needs), so it names none.
inline constexpr const char* kHelperMissing = "Online matches can't connect on this computer. Check the game's YouGame page for what to install. You can still play offline.";
inline const char* session_notice(const std::string& reason) {
  if (reason == "left" || reason == "away" || reason == "kicked") return "Your friend left.";
  if (reason == "host_ended") return "The host ended the session.";
  if (reason == "setup_refused") return "Your friend's game couldn't load that match.";
  if (reason == kBootTimeout) return "Your friend's game didn't start.";
  if (reason == "Couldn't get your friend's save") return "Couldn't get your friend's save";
  if (reason == kMixedBuilds) return kMixedBuilds;
  if (reason == kHelperMissing) return kHelperMissing;
  return nullptr;
}
}
