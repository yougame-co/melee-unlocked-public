// The Doubles home's search (G2, rooms MU-DOUBLES-R2 "G2 bridge calls"): the pieces of find_match's
// Doubles flow that are plain data, so they are unit tested. The home itself is the Friends lobby's
// session on its 2-seat profile (yougame_lobby.h, Profile::Doubles); this adds what lies around it:
//  - the queue generation Rooms hands the home in `lobby.party.queueing` once Start went through: each
//    human opens one queue ticket naming it (friends::doubles_queue_ticket);
//  - what the queue socket says (searching, matched, search-error);
//  - the app's room reports (mpRoom with the Teams summary: the home as "party", the match as "match"),
//    each foreground change with a newer roomEpoch;
//  - the frames that end a placed match before it started (the match room never sends lobby state).
#pragma once
#include "yougame_lobby_wire.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace yougame::lobby {

inline constexpr const char* kDoublesEnded = "The Doubles match ended before it started. Press START to search again.";
inline constexpr const char* kDoublesNoSetup = "The Doubles match never started. Press START to search again.";
// Rooms gives a placed match 60 s for everyone to arrive, then holds it 90 s more for a late player (150 s
// in all): the match room waits past that for its setup, then gives up without a leave (a leave there is a
// placed player walking out; the room ends the match on its own and the home takes the select back).
inline constexpr uint64_t kDoublesSetupMs = 165000;

// lobby.party for the Doubles home (the `lobby` broadcast, and the home's welcome): the generation the
// home searches with, how many humans it has (n: a human and a CPU are 1), and whether it is placed.
struct DoublesParty {
  std::string generation;
  int humans = 0;
  bool playing = false;
  // True when the message carried the party (whatever changed).
  bool observe(const json& m) {
    if (!m.is_object() || !m.count("party") || !m["party"].is_object()) return false;
    const json& party = m["party"];
    playing = party.value("playing", json()) == true;
    const json q = party.value("queueing", json());
    generation.clear();
    humans = 0;
    if (q.is_object() && q.count("id") && q["id"].is_string() && digest_ok(q["id"].get<std::string>()) &&
        integer_at(q, "n", 1, 2)) {
      generation = q["id"].get<std::string>();
      humans = (int)q["n"].get<int64_t>();
    }
    return true;
  }
};

// What the queue socket said.
enum class QueueNews { None, Searching, Matched, Refused };
struct QueueMessage {
  QueueNews news = QueueNews::None;
  std::string room;     // Matched: the match room's code
  std::string code;     // Refused: search-error's code (digest_mismatch, teams_generation, ...) or "error"
  int waiting = -1;     // Searching: players waiting, when said
};
inline QueueMessage queue_message(const json& m) {
  QueueMessage out;
  if (!m.is_object() || !m.count("t") || !m["t"].is_string()) return out;
  const std::string t = m["t"].get<std::string>();
  if (t == "matched") {
    if (m.count("room") && m["room"].is_string()) {
      const std::string room = m["room"].get<std::string>();
      if (!room.empty() && room.size() <= 64) { out.news = QueueNews::Matched; out.room = room; }
    }
  } else if (t == "search-error" || t == "error") {
    out.news = QueueNews::Refused;
    out.code = m.count("code") && m["code"].is_string() ? m["code"].get<std::string>().substr(0, 32) : std::string("error");
  } else if (t == "searching") {
    out.news = QueueNews::Searching;
    if (integer_at(m, "waiting", 0, 1000000)) out.waiting = (int)m["waiting"].get<int64_t>();
  }
  return out;
}

// A placed match that ended before its setup (a player who never came, the home released): the
// search fails quietly and the home, still parked, takes the select back.
inline bool doubles_match_ended(const json& m) {
  if (!m.is_object() || !m.count("t") || !m["t"].is_string()) return false;
  const std::string t = m["t"].get<std::string>();
  return t == "teams-dissolved" || t == "ended" || t == "roomClosed" || t == "kicked";
}

// Process-wide: a later report always supersedes an earlier one, whichever room it names.
inline uint64_t next_room_epoch() {
  static std::atomic<uint64_t> epoch{0};
  return ++epoch;
}
// The app's room report. match: the placed match room (4 seats, queue casual, phase assembling until its
// setup is planned, then playing), else the home (2 seats, queue private, phase home). The page accepts
// exactly these (web/src/lib/native-teams.ts acceptNativeTeamsRoom: a party report is the home, private,
// size 2; a match report another room, casual, size 4).
inline json doubles_room_report(const std::string& code, const std::string& home, bool match, uint64_t epoch,
                                const json& participants, const std::string& me, int revision, bool playing = false) {
  return {{"type", "mpRoom"}, {"code", code}, {"roomEpoch", epoch}, {"mode", "teams"},
          {"queue", match ? "casual" : "private"}, {"size", match ? kSeats : kDoublesSeats},
          {"nativeTeams", {{"v", 1}, {"kind", match ? "match" : "party"}, {"home", home},
                           {"phase", match ? (playing ? "playing" : "assembling") : "home"}}},
          {"me", me}, {"players", participants}, {"participants", participants}, {"revision", revision}};
}
// A Doubles home's welcome with nobody in it but this player (welcome.players: Rooms lists the connected and the
// away there; `me` is this player's id). A guest's ticket for a friend's home that expired or emptied opens a
// fresh home under that code (rooms' freshMeta, or startOver after host_gone) with only the guest in it: false
// when the welcome has no player list, so an unknown shape never fails a join.
inline bool lone_welcome(const json& welcome) {
  if (!welcome.is_object() || !welcome.count("players") || !welcome["players"].is_array()) return false;
  const std::string me = welcome.value("me", json()).is_string() ? welcome["me"].get<std::string>() : std::string();
  for (const auto& p : welcome["players"])
    if (!p.is_object() || !p.count("id") || !p["id"].is_string() || p["id"].get<std::string>() != me) return false;
  return true;
}
// A joined room's lobby frame (welcome.muLobby, or a yg.mu.lobby state) is the Doubles home's: Rooms' muState
// has 2 seats there and a `searching` key only a Doubles lobby carries; the Friends lobby has 4 and none.
inline bool doubles_frame(const json& lobby) {
  if (!lobby.is_object()) return false;
  if (lobby.count("searching")) return true;
  return lobby.count("seats") && lobby["seats"].is_array() && lobby["seats"].size() == (size_t)kDoublesSeats;
}

}  // namespace yougame::lobby
