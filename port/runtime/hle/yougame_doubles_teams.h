// YouGame Doubles on the 4-peer rollback client (Slippi Teams). Pure rules shared by the
// room pump, the match setup, the Doubles home and the tests: no sockets, no globals.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <set>
#include <string>

namespace yougame::doubles_teams {
using json = nlohmann::json;

// The Doubles match plays on the 4-peer rollback client. 0: the console session, kept compiled (its
// retained-home tests build with 0).
#ifndef MU_DOUBLES_FOUR_PEER
#define MU_DOUBLES_FOUR_PEER 1
#endif
inline constexpr bool kFourPeer = MU_DOUBLES_FOUR_PEER != 0;
inline constexpr int kTeamsMode = 3;  // Slippi's Teams mode id
// A YouGame Teams search that names a room is the Doubles match the home found: the Teams row
// itself never carries a room (it searches by code, or is the locked Slippi row).
constexpr bool match_search(int mode, bool room) { return mode == kTeamsMode && room; }

// The frozen pairing when the room names no team: slots 0,1 team 0; slots 2,3 team 1.
constexpr int slot_team(int slot) { return slot >= 0 && slot < 4 ? slot / 2 : -1; }

// Doubles-legal stages, the same order as the console rules: Battlefield, Final Destination,
// Dream Land, Yoshi's Story, Pokemon Stadium. No Fountain of Dreams in doubles.
inline constexpr std::array<uint16_t, 5> kStages{{31, 32, 28, 8, 3}};

struct Seats {
  std::array<int, 4> team{{-1, -1, -1, -1}};  // by slot (Slippi player index)
  std::array<std::string, 4> connection;      // the peer each slot is
  std::string seed;                           // 16 lowercase hex; empty when the room sent none
  uint64_t epoch = 0;                         // selectionEpoch
  bool known = false;                         // a valid nativeTeams named all four seats
};

inline const json* native_teams(const json& m) {
  if (!m.is_object()) return nullptr;
  if (m.count("nativeTeams") && m["nativeTeams"].is_object()) return &m["nativeTeams"];
  if (m.count("room") && m["room"].is_object() && m["room"].count("nativeTeams") &&
      m["room"]["nativeTeams"].is_object()) return &m["room"]["nativeTeams"];
  return nullptr;
}
inline bool seed_valid(const std::string& s) {
  if (s.size() != 16) return false;
  for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
// A match room's lobby: nativeTeams {v:1, kind:"match", muDoubles:true, seats:[{id, connectionId,
// localIndex, slot, team} x4], seed, selectionEpoch}. Seats are matched by their own slot, never by
// array position. A seat without a team takes slot_team. Anything else leaves `out` unchanged.
inline bool read(const json& m, Seats& out) {
  const json* nt = native_teams(m);
  if (!nt || nt->value("muDoubles", json()) != true || nt->value("kind", json()) != "match") return false;
  if (!nt->count("seats") || !(*nt)["seats"].is_array() || (*nt)["seats"].size() != 4) return false;
  Seats s;
  std::set<std::string> connections;
  int per_team[2] = {0, 0};
  for (const auto& seat : (*nt)["seats"]) {
    if (!seat.is_object() || !seat.count("slot") || !seat["slot"].is_number_integer()) return false;
    const auto slot = seat["slot"].get<int64_t>();
    if (slot < 0 || slot > 3 || s.team[slot] >= 0) return false;
    int team = slot_team((int)slot);
    if (seat.count("team")) {
      if (!seat["team"].is_number_integer() || (seat["team"] != 0 && seat["team"] != 1)) return false;
      team = seat["team"].get<int>();
    }
    const auto connection = seat.value("connectionId", json());
    if (!connection.is_string() || connection.get<std::string>().empty() ||
        !connections.insert(connection.get<std::string>()).second) return false;
    s.team[slot] = team; s.connection[slot] = connection.get<std::string>(); ++per_team[team];
  }
  if (per_team[0] != 2 || per_team[1] != 2) return false;
  const auto seed = nt->value("seed", json());
  if (seed.is_string() && seed_valid(seed.get<std::string>())) s.seed = seed.get<std::string>();
  const auto epoch = nt->value("selectionEpoch", json());
  if (epoch.is_number_unsigned() || (epoch.is_number_integer() && epoch.get<int64_t>() >= 0)) s.epoch = epoch.get<uint64_t>();
  s.known = true;
  out = s;
  return true;
}
// Seats are matched to the roster by connectionId, never by array position: every participant the
// seats name must sit in that seat's slot (the Slippi player index is the participant's slot).
inline bool consistent(const Seats& s, const json& participants) {
  if (!s.known) return true;
  if (!participants.is_array()) return false;
  for (const auto& p : participants) {
    if (!p.is_object()) continue;
    const auto connection = p.value("connectionId", json());
    const auto slot = p.value("slot", json());
    if (!connection.is_string()) continue;
    for (int seat = 0; seat < 4; ++seat)
      if (s.connection[seat] == connection.get<std::string>() &&
          (!slot.is_number_integer() || slot.get<int64_t>() != seat)) return false;
  }
  return true;
}
// The local player's team from the queue's matched frame: its team, else its slot's, else -1.
inline int matched_team(const json& m) {
  if (!m.is_object()) return -1;
  const auto team = m.value("team", json());
  if (team.is_number_integer() && (team == 0 || team == 1)) return team.get<int>();
  const auto slot = m.value("slot", json());
  return slot.is_number_integer() ? slot_team((int)slot.get<int64_t>()) : -1;
}

inline uint64_t fnv64(const std::string& text) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
  return h;
}
// The room's seed; a room made before the seed existed derives one from its code (plan() allows that only
// when the room's seats are known).
inline uint64_t seed_value(const std::string& seed, const std::string& room_code) {
  if (seed_valid(seed)) return std::stoull(seed, nullptr, 16);
  return fnv64("mu-doubles:" + room_code);
}
inline uint64_t mix(uint64_t x) {  // splitmix64 finalizer
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
// The match's stage: game 0 takes the seed alone. A room plays one match, so a later game happens only if
// its result never came; it mixes in the game's index, which all four agree on (each game starts on all
// four at once). Nothing a peer chooses (its selection epoch included) goes in.
inline uint16_t stage(uint64_t seed, uint32_t game) {
  const uint64_t pick = game == 0 ? seed : mix(seed ^ mix(0x100000000ull + game));
  return kStages[pick % kStages.size()];
}

// The match Rooms froze for the four when the host's console begin went through: the session (the room's
// matchId), its seed, and each seat's port, team and ids. Read from the start frame.
struct Setup {
  std::string match, seed;                    // the session id; 16 lowercase hex
  std::array<int, 4> team{{-1, -1, -1, -1}};  // by slot (port - 1)
  std::array<std::string, 4> id, connection;
  bool known = false;
};
inline const json* start_console(const json& m) {
  if (!m.is_object()) return nullptr;
  if (m.count("console") && m["console"].is_object()) return &m["console"];
  if (m.count("room") && m["room"].is_object() && m["room"].count("console") && m["room"]["console"].is_object())
    return &m["room"]["console"];
  return nullptr;
}
inline std::string field(const json& j, const char* key) {
  const auto v = j.is_object() ? j.value(key, json()) : json();
  return v.is_string() && v.get_ref<const std::string&>().size() <= 128 ? v.get<std::string>() : std::string();
}
// A frame carrying the room's console session (start, or a later snapshot of the room). Returns why it is
// refused, or nullptr with `out` set. `digest` is the Doubles rules digest the host's begin sent.
inline const char* read_start(const json& m, const std::string& digest, Setup& out) {
  const json* c = start_console(m);
  if (!c) return "no console session";
  Setup s;
  s.match = field(*c, "sessionId"); s.seed = field(*c, "seed");
  if (s.match.empty()) return "the session has no id";
  if (m.count("matchId") && field(m, "matchId") != s.match) return "the session is not the room's match";
  if (!seed_valid(s.seed)) return "the session seed is not 16 lowercase hex";
  if (c->value("teamBattle", json()) != true) return "the session is not a Team Battle";
  if (field(*c, "settingsDigest") != digest) return "the session's rules are not these Doubles rules";
  if (!c->count("participants") || !(*c)["participants"].is_array() || (*c)["participants"].size() != 4)
    return "the session needs four participants";
  std::set<std::string> ids, connections;
  int per_team[2] = {0, 0};
  for (const auto& p : (*c)["participants"]) {
    const auto port = p.is_object() ? p.value("port", json()) : json();
    const auto slot = p.is_object() ? p.value("slot", json()) : json();
    const auto team = p.is_object() ? p.value("team", json()) : json();
    if (!port.is_number_integer() || port.get<int64_t>() < 1 || port.get<int64_t>() > 4 ||
        !slot.is_number_integer() || slot.get<int64_t>() != port.get<int64_t>() - 1 ||
        !team.is_number_integer() || (team != 0 && team != 1)) return "a seat has no port, slot or team";
    const int at = (int)slot.get<int64_t>();
    const auto id = field(p, "id"), connection = field(p, "connectionId");
    if (s.team[at] >= 0 || id.empty() || connection.empty() || !ids.insert(id).second ||
        !connections.insert(connection).second) return "the seats are not four computers";
    s.team[at] = team.get<int>(); s.id[at] = id; s.connection[at] = connection; ++per_team[s.team[at]];
  }
  if (per_team[0] != 2 || per_team[1] != 2) return "the teams are not two a side";
  s.known = true;
  out = s;
  return nullptr;
}
inline bool seated(const Setup& s, const std::string& connection) {
  if (!s.known || connection.empty()) return false;
  for (const auto& c : s.connection) if (c == connection) return true;
  return false;
}

inline constexpr const char* kSeatsUnknown =
    "The Doubles room didn't say who is on which team. Press B to leave";
// What a game in the match room plays: the start's teams and seed when Rooms sent one; else the room's
// seats and seed, where a room placed before the seed existed (05:11Z) takes its code, and only when its
// seats are known. Without either there is no match: refused on screen, never slot order or a code seed.
struct Plan {
  std::array<int, 4> team{{-1, -1, -1, -1}};
  uint64_t seed = 0;
  Seats seats;  // who sits where, for the roster check
  const char* refusal = nullptr;
};
inline Plan plan(const Setup& start, const Seats& room, const std::string& code) {
  Plan p;
  if (start.known) {
    p.team = start.team; p.seed = std::stoull(start.seed, nullptr, 16);
    p.seats.team = start.team; p.seats.connection = start.connection; p.seats.seed = start.seed; p.seats.known = true;
    return p;
  }
  if (!room.known) { p.refusal = kSeatsUnknown; return p; }
  p.team = room.team; p.seed = seed_value(room.seed, code); p.seats = room;
  return p;
}

inline constexpr const char* kTeamMismatch =
    "A player's team doesn't match the Doubles room. Press B to leave";
// Team colours (11-11 report: "would be nice to be able to change team color easily in doubles"). Melee's
// team byte is a colour: red 0, blue 1, green 2. The room owns the pairs (0 or 1 by slot); each machine's
// 0x82 team_id now carries the colour its player chose on the select's toggle, not the room team.
inline constexpr int kTeamColors = 3;
// The colour each room team wears: its lowest selected slot's colour (every machine holds the same 0x82s
// once all four are in, so all four agree), else the team's own index (red, blue). A clash (both teams
// chose one colour): team 0 keeps it, team 1 wears the first of red, blue, green that team 0 isn't.
inline std::array<int, 2> team_colors(const std::array<int, 4>& room, const std::array<int, 4>& sent, unsigned present) {
  std::array<int, 2> color{{0, 1}};
  for (int team = 0; team < 2; ++team)
    for (int slot = 0; slot < 4; ++slot)
      if ((present & (1u << slot)) && room[slot] == team && sent[slot] >= 0 && sent[slot] < kTeamColors) {
        color[team] = sent[slot];
        break;
      }
  if (color[1] == color[0])
    for (int c = 0; c < kTeamColors; ++c)
      if (c != color[0]) { color[1] = c; break; }
  return color;
}
// The pairs are the room's: every selected player has a room team, two a side, and sent a colour (any of
// the three; team_colors settles who wears which). A peer's colour never moves it to the other team.
// present: bit n set for slot n in the match. Returns the refusal, or nullptr to play.
inline const char* team_conflict(const std::array<int, 4>& room, const std::array<int, 4>& sent, unsigned present) {
  int per_team[2] = {0, 0};
  for (int slot = 0; slot < 4; ++slot) {
    if (!(present & (1u << slot))) continue;
    if (room[slot] != 0 && room[slot] != 1) return kTeamMismatch;
    if (sent[slot] < 0 || sent[slot] >= kTeamColors) return kTeamMismatch;
    ++per_team[room[slot]];
  }
  return per_team[0] == 2 && per_team[1] == 2 ? nullptr : kTeamMismatch;
}
}  // namespace yougame::doubles_teams
