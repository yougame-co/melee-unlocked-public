// Process-local policy for the clean engine launched by YouGame.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <string>

namespace yougame::mode_policy {

// Direct, Teams and Party in the Slippi submenu never use YouGame's queues. An explicit
// friend room is the separate Friends flow, which retains its existing routing.
enum class MatchmakingRoute { YouGame, Slippi, Blend, Locked };
constexpr bool slippi_menu_mode(int mode, bool friend_room) {
  return mode >= 2 && mode <= 4 && !friend_room;
}

// Slippi mode ids: 0 Ranked, 1 Unranked, 2 Direct, 3 Teams, 4 Party.
// Plain Unranked keeps its mixed search on the base listing. Direct, Teams and Party
// require the app's granted Slippi account, independent of connect-code syntax.
constexpr MatchmakingRoute matchmaking_route(int mode, bool yougame_mode, bool slippi_login,
                                             bool agent_script, bool friend_room, bool vanilla) {
  if (yougame_mode && slippi_menu_mode(mode, friend_room))
    return slippi_login && !agent_script ? MatchmakingRoute::Slippi : MatchmakingRoute::Locked;
  if (agent_script) return MatchmakingRoute::YouGame;
  if (!yougame_mode) return MatchmakingRoute::Slippi;
  return mode == 1 && slippi_login && !friend_room && vanilla ? MatchmakingRoute::Blend : MatchmakingRoute::YouGame;
}

// Whether the Online menu offers Ranked in YouGame mode. Off until Ranked plays like Slippi's
// (rank shown, best of 3, stage striking, rating change) (2026-10-29). The site's ladder
// switch is off for these listings too, and the page turns a ranked request into a casual one.
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
constexpr bool yougame_ranked_offered = true;
#else
constexpr bool yougame_ranked_offered = false;
#endif
// The Online menu offers Friends by default in YouGame launches.
#ifndef YOUGAME_FRIENDS_ROW
#define YOUGAME_FRIENDS_ROW 1
#endif
constexpr bool friends_offered(bool yougame_mode) { return YOUGAME_FRIENDS_ROW != 0 && yougame_mode; }

constexpr bool spectator_allowed(bool yougame_mode, bool setting_on) {
  return !yougame_mode && setting_on;
}

// The crash dialog (main.cpp: die_report, crash_filter) is a modal message box on the thread that
// stopped, and the process ends only once someone closes it. Launched by YouGame, the game draws
// inside the app's player box and the app tells the player that it stopped, from the exit code; the
// dialog there kept the process alive with its last picture on screen (the presenter runs on at the
// monitor's rate, sim 0 Hz), so the app never learned. Beyond Melee's boot check on 10-27 stopped
// that way. Hidden and headless test runs never show it either.
constexpr bool crash_dialog_allowed(bool automated, bool yougame_mode) {
  return !automated && !yougame_mode;
}

// The last stderr line of a run that stopped (exit code 3), which the app shows the player and puts in
// reports: "crash: <reason>". One line, the reason up to its first ';' (a guest fault's registers
// follow it), at most 160 characters, and never a path: a message naming a file could carry the
// player's user name, so it becomes a plain "fatal error".
inline std::string crash_line(const char* message) {
  std::string reason = message ? message : "";
  reason = reason.substr(0, reason.find_first_of(";\r\n"));
  while (!reason.empty() && reason.back() == ' ') reason.pop_back();
  if (reason.empty() || reason.find_first_of("\\/") != std::string::npos) reason = "fatal error";
  if (reason.size() > 160) reason.resize(160);
  return "crash: " + reason;
}

struct ModSources {
  bool explicit_args;
  bool saved_profile;
  bool folders;  // Discovery, saved choices, implicit overlays and auxiliary mod-file loaders.
  std::size_t ignored_choices;
};

// Explicit launch layers always survive. Standalone keeps its existing profile precedence and
// folder scanning/overlay behavior. Count only known choices: YouGame never scans to count packs.
// Listing profile/discovery precedence is enforced by upstream's vanilla-only, explicit layers
// and no-mod-autodetect paths; the saved-profile and implicit Mods/Files sites also check this
// policy in YouGame mode (defense in depth, loader finding #8). The folders gate still protects
// auxiliary loaders and card mounts, which no-mod-autodetect does not suspend for remix listings.
constexpr ModSources mod_sources(bool yougame_mode, bool explicit_args = false,
                                 bool saved_profile = false, std::size_t saved_discovered_choices = 0) {
  return {explicit_args, !yougame_mode && !explicit_args && saved_profile, !yougame_mode,
          yougame_mode ? saved_discovered_choices + (saved_profile ? 1 : 0) : 0};
}

// Ordinary progress saves still mount. A known mod save on a card needs an explicit GCI layer;
// selecting an ISO or a directory must not silently opt into an old mod save alongside it.
constexpr bool card_save_allowed(bool yougame_mode, bool mod_save, bool explicit_import = false) {
  return !mod_save || explicit_import || mod_sources(yougame_mode).folders;
}

}  // namespace yougame::mode_policy
