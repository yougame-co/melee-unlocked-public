// YouGame Ranked: one best-of-3 set per room, and the player's rank. Kept independent of WinHTTP
// for deterministic socket tests (port/tests/yougame_ranked_test.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include "../abi/mu_ranked_wire.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace yougame::ranked {
using json = nlohmann::json;

// A string field, or "" when it is missing or not a string (json::value throws on a wrong type).
inline std::string str(const json& m, const char* key) {
  if (!m.is_object()) return {};
  const auto it = m.find(key);
  return it != m.end() && it->is_string() ? it->get<std::string>() : std::string();
}

inline std::string log_text(const std::string& text, size_t cap) {
  std::string out;
  for (unsigned char c : text.substr(0, cap)) out += c >= 32 && c <= 126 ? char(c) : '?';
  return out;
}

inline std::string ticket_error(const json& answer) {
  const std::string code = str(answer, "code"), error = str(answer, "error");
  if (code == "ranked_signed_out") return "Sign in to YouGame to play Ranked.";
  if (code == "ranked_off") return "Ranked is off for this game.";
  return error.empty() ? "YouGame request failed" : error;
}

// Ranked v1 plays Melee Unlocked's Unranked screens (character select, a random legal stage each
// game) and is rated on YouGame's ladder as one best-of-3 set per room. The game sends no set
// result in that flow (there is no GameSetup scene, so no C2): the set is decided here, from the
// games the room settled, which both players receive alike.
constexpr int kSetWins = 2;
// Draws and games the room voids do not count. A set still undecided after this many settled
// games is void (it cannot produce a winner); it only bounds the reports.
constexpr int kMaxGames = 9;
// How many times (every kRetryMs) the host asks the room to start the set's match.
constexpr int kBeginTries = 15;
constexpr uint64_t kRetryMs = 2000;
// rooms/game-lobby.ts MATCH_RECONNECT_MS (used by leave-rule tests).
constexpr uint64_t kGameGraceMs = 15000;
// Local cleanup bound, NEVER a claim that a room's report deadline expired.
// An unconfirmed game ends the set; it never admits the next game.
constexpr uint64_t kRecoveryWaitMs = 30000;
constexpr uint64_t kPairCleanupMs = 30000;
// A lost link names no leaver: the room's leave rule forfeits whichever game left the room
// (after kGameGraceMs). If both games are still in the room this long after the link broke,
// neither left, and the set is called off as void. Under kDrainMs, so the call-off is sent.
constexpr uint64_t kLostCallOffMs = kGameGraceMs + 10000;
// A set whose room has not started its match this long after its first game began cannot be
// recorded: the pair breaks up instead of playing unrated games under Ranked.
constexpr uint64_t kStartWaitMs = kBeginTries * kRetryMs + 5000;

// One game's winner (0-based port, -1 for none) from the game's own end record. `end_method` is
// Slippi's Game End method: 1 TIME!, 2 GAME!, 7 a No Contest. Ranked has no LRA-Start (the game
// installs no pause in mode 0), so a No Contest there is the online code ending the game for a
// disconnect or a desync, naming the other player as `lras`. Ranked must wait for the finalized
// report to distinguish them before using this winner. `placements` are the record's four
// placement bytes (0 first, 0xFF no player); without them `reported` (Slippi's winnerIdx) is used.
inline int game_winner(int end_method, int lras, int reported, const int* placements) {
  if (end_method == 7) return lras == 0 || lras == 1 ? 1 - lras : -1;
  if (placements && (end_method == 1 || end_method == 2)) {
    int winner = -1;
    for (int port = 0; port < 4; ++port) {
      if (placements[port] != 0) continue;
      if (port > 1 || winner >= 0) return -1;   // a tie, or a placement outside the duel
      winner = port;
    }
    return winner;
  }
  return reported == 0 || reported == 1 ? reported : -1;
}

// Casual/Direct duels use the same placements, but a No Contest never awards a winner.
// Multiplayer uses team_winner below; this single-port result remains duel-only.
inline int casual_winner(int end_method, int lras, int reported, const int* placements, int players) {
  if (players != 2 || end_method == 7) return -1;
  return game_winner(end_method, lras, reported, placements);
}

// A duel's TIME! is Slippi's to decide (SinglesDetermineWinner: the ledge grab limit, then stocks,
// then damage; an exact tie has no winner), and only the game's report carries that: winnerIdx,
// sent once the game can no longer roll back (slippi_online.cpp handle_report_game). The Game End
// record ranks a time-out the way Melee's results screen does (stocks, kills, falls, then port),
// and the Source Port sends it before the last frames are confirmed: on 10-28 two tied ranked
// time-outs (4 stocks and 0% each) went to port 1 from the record while the report said no winner.
// So a duel's TIME! from the record (`from_record`: it has placements) waits for the report.
// Teams has no report (ReportGameQuery has no teams result) and keeps the record.
inline bool time_out_waits_for_report(int end_method, bool from_record, int players) {
  return end_method == 1 && from_record && players == 2;
}

// A Ranked No Contest's record cannot distinguish disconnect from connected desync.
// The finalized report carries winnerIdx -2 for desync, -3 for disconnect.
inline bool no_contest_waits_for_report(int end_method, bool from_record, bool ranked) {
  return ranked && end_method == 7 && from_record;
}

// Teams v1 requires exactly two known players per team. An absent/invalid seat cannot win.
inline bool valid_teams(const int* teams) {
  if (!teams) return false;
  int count[2] = {};
  for (int port = 0; port < 4; ++port) {
    if (teams[port] != 0 && teams[port] != 1) return false;
    ++count[teams[port]];
  }
  return count[0] == 2 && count[1] == 2;
}

// Game End placements are 0-based (0 first, 0xFF absent). Winners may share first place
// within one team, or a surviving player may win after their teammate was eliminated.
// Only TIME/GAME with first place belonging exclusively to one team is conclusive. LRAS
// never awards a casual win; a No Contest, missing placements or a cross-team tie is void.
inline int team_winner(int end_method, int lras, const int* placements, const int* teams) {
  if ((end_method != 1 && end_method != 2) || lras != -1 || !placements || !valid_teams(teams)) return -1;
  int winner = -1;
  for (int port = 0; port < 4; ++port) {
    if (placements[port] < 0 || placements[port] > 3) return -1;
    if (placements[port] != 0) continue;
    if (winner >= 0 && winner != teams[port]) return -1;
    winner = teams[port];
  }
  return winner;
}

// A game report's `detail`: what the room stores beside the outcome and never compares. The room
// takes at most 8 keys of 1-4 lowercase letters, each an integer in [-1, 2^31) or up to four of
// them (rooms/src/game-lobby.ts cleanDetail); anything outside that is sent as -1.
inline json game_detail(int end_method, int lras, int stage, const int characters[2], int64_t frames) {
  const auto fit = [](int64_t v) -> int64_t { return v >= -1 && v < (int64_t(1) << 31) ? v : -1; };
  json detail = {{"end", fit(end_method)}, {"lras", fit(lras)}};
  if (stage >= 0) detail["stg"] = fit(stage);
  if (characters) detail["char"] = {fit(characters[0]), fit(characters[1])};
  if (frames >= 0) detail["frm"] = fit(frames);
  return detail;
}

// A game's report id from what both players' games negotiated for it (the match's rng offset and
// stage), so a game one side failed to count cannot shift every later id. "" when unknown.
inline std::string game_key(int64_t rng_offset, int stage) {
  if (rng_offset < 0 || rng_offset > 0xFFFFFFFFll || stage < 0 || stage > 0xFFFF) return {};
  char key[32];
  std::snprintf(key, sizeof key, "r%llx-s%d", (unsigned long long)rng_offset, stage);
  return key;
}

struct Game {
  std::string key;          // end telemetry only; start_game owns the wire id
  uint32_t index = 0, tiebreak = 0;
  int winner = -1;        // 0-based port; -1 a draw or a game with no winner (reported void)
  bool forfeit = false;   // the loser quit (a No Contest naming them): the set goes to the winner
  json detail = json::object();
};

inline Game game_outcome(int end_method, int lras, int reported, const int* placements) {
  Game game;
  if (reported == -2) return game; // connected desync: void this game and replay
  game.winner = game_winner(end_method, lras, reported, placements);
  game.forfeit = end_method == 7 && game.winner >= 0;
  return game;
}

// One ranked room's set, as one client sees it. The rooms Worker (game-lobby.ts) settles a duel's
// game only when both players report the same outcome, and its final only when both send the
// same `finish`, or by its leave rule: once a player has been gone for its reconnect allowance
// (15 s), the player still there gets the set, unless a pending report is void or names someone
// else. So:
// - the set's winner is the first to kSetWins games the room settled (both clients see the same
//   game-result events, so both send the same finish), or the player a No Contest did not name;
// - a player who quits (leaves the online menus, holds Z, Slippi's status 40) concedes and closes
//   at once, so the leave rule can award the opponent even if the opponent's game says nothing;
// - a player whose connection to the opponent broke claims the set, but only once its own games
//   are settled: the pair may have broken because the opponent already saw the deciding result;
// - exactly one final per set: C2 (a future GameSetup port) never adds a second one.
class Set {
 public:
  void configure(int local, std::vector<std::string> participants) {
    local_ = local;
    participants_ = std::move(participants);
  }
  // A game of the set started (its first frame; the game repeats frame 1 while it waits for the
  // opponent, so only the first call of a game counts). The first game makes the host ask for the
  // match. Both players use the negotiated key for the start and its eventual report.
  void start_game(const std::string& key = {}) {
    if (final_requested_ || closed_) return;
    if (in_game_ && (key.empty() || key == active_key_)) return;
    started_ = in_game_ = true;
    ++games_started_;
    active_key_ = key;
    const std::string base = key.empty() ? "g" + std::to_string(games_started_) + "-t0" : key;
    active_id_ = base;
    // Negotiated RNG/stage pairs can repeat. Both clients know the game number in this set;
    // keep repeated frame 1 idempotent even when this game's wire id needed a suffix.
    for (unsigned attempt = 0; games_.count(active_id_); ++attempt) {
      const std::string suffix = "-g" + std::to_string(games_started_) +
          (attempt ? "-" + std::to_string(attempt) : "");
      active_id_ = base.substr(0, 64 - suffix.size()) + suffix;
    }
    if (!games_.count(active_id_)) {
      Pending p;
      p.start = {{"t", "game-start"}, {"id", active_id_}};
      games_.emplace(active_id_, std::move(p));
      order_.push_back(active_id_);
    }
  }
  int games_started() const { return games_started_; }
  // Unique room receipts for our reports (draws/voids count too).
  int games_received() const {
    return (int)std::count_if(order_.begin(), order_.end(), [&](const std::string& id) {
      return !games_.at(id).message.is_null() && !games_.at(id).refused &&
             results_.count(id) && games_.at(id).received;
    });
  }
  // GameSetup adds a presentation fence; the room-settled first-to-two authority stays on.
  void game_setup() { game_setup_ = true; }

  void report_game(const Game& game) {
    in_game_ = false;
    if (final_requested_ || !started_ || closed_) return;
    // Keep the first-frame id even if end telemetry is incomplete or changes.
    auto& report = games_.at(active_id_);
    if (!report.message.is_null() || report.refused) return;
    report.message = {{"t", "game-report"}, {"id", active_id_}};
    if (valid_port(game.winner)) report.message["winner"] = participants_[game.winner];
    else report.message["void"] = true;
    if (game.detail.is_object() && !game.detail.empty()) report.message["detail"] = game.detail;
    if (results_.count(active_id_)) report.received = true;
    if (game.forfeit && valid_port(game.winner) && forfeit_ < 0) forfeit_ = game.winner;
    decide();
  }
  // C2 (CMD_REPORT_SET_COMPLETE): the game's own verdict, once its games are settled.
  void complete(uint8_t end_mode) {
    if (!started_ || c2_ >= 0) return;
    c2_ = end_mode;
    decide();
  }
  // The local player left the set (Slippi's status 40, leaving the online menus, holding Z).
  void quit() {
    if (!started_ || quit_ || lost_) return;
    quit_ = true;
    decide();
  }
  // The connection to the opponent broke (their game left, or the link died). Both games see the
  // same break, so neither claims an undecided set: the room's leave rule decides it.
  // `now` starts the call-off with the drain (the pump may not run due() while the room reconnects).
  void lost(uint64_t now = 0) {
    if (!started_ || quit_ || lost_) return;
    lost_ = true;
    lost_at_ = now;
    decide();
  }
  // A quitter whose concession is already decided leaves at once (sends what it owes, then closes),
  // so the room's leave rule can run; anyone else stays until the room's result.
  bool leaves_at_once() const { return conceded_; }

  // What is due now: the host's begin, each first-frame start and end report in order
  // (the next game waits for its predecessor's receipt), then the final. Sent subgame
  // commands wait for an answer; only a successful rejoin permits their replay.
  std::vector<json> due(uint64_t now, bool host, int revision) {
    std::vector<json> out;
    now_ = now;
    if (lost_ && !lost_at_) lost_at_ = now;
    for (const auto& [id, p] : games_) {
      if (!settled_ && !closed_ && p.refused && !p.received && now >= p.recovery_until) {
        // No authoritative outcome/deadline is exposed by this room protocol.
        // End cleanly; do not manufacture a receipt or send a queued next start.
        recovery_failed_ = cleanup_ = true;
        if (!final_requested_) finish(-1);
      }
    }
    decide();
    // Run before the settled_ early return: missing C2 is bounded too.
    if (game_setup_ && (final_requested_ || settled_)) {
      if (!cleanup_at_) cleanup_at_ = now + kPairCleanupMs;
      if (now >= cleanup_at_) cleanup_ = true;
    }
    // A concession is flushed exactly once by close_room before closing the socket; the pump
    // must not send it concurrently with that flush.
    if (!started_ || settled_ || closed_ || conceded_) return out;
    if (match_id_.empty()) {
      if (!waiting_since_) waiting_since_ = now;
      if (now - waiting_since_ >= kStartWaitMs) {
        orphaned_ = true;
        return out;
      }
      if (host && !begin_dead_ && (begin_tries_ == 0 || now >= begin_at_)) {
        out.push_back({{"t", "begin"}, {"revision", revision}});
        begin_at_ = now + kRetryMs;
        if (++begin_tries_ >= kBeginTries) begin_dead_ = true;
      }
      return out;
    }
    if (!recovery_failed_ && next_game_message(out, now, false)) return out;
    if (final_requested_ && !final_dead_ && now >= final_at_) {
      final_["matchId"] = match_id_;
      out.push_back(final_);
      final_at_ = now + kRetryMs;
    }
    return out;
  }
  // A quitter's last words before it closes: the next owed report, then the final after receipts.
  std::vector<json> flush() {
    std::vector<json> out;
    if (match_id_.empty() || settled_ || closed_) return out;
    if (!recovery_failed_ && next_game_message(out, 0, true)) return out;
    if (final_requested_ && !final_dead_ && !final_sent_) {
      final_["matchId"] = match_id_;
      out.push_back(final_);
    }
    return out;
  }

  // Called after socket I/O, under the room mutex. A failed attempt keeps its retry spacing
  // from due(), but never becomes sent. Only a successful send may await an answer.
  void sent(const json& message) {
    if (str(message, "t") == "finish") { final_sent_ = true; return; }
    const auto it = games_.find(str(message, "id"));
    if (it == games_.end()) return;
    if (str(message, "t") == "game-start") it->second.start_sent = true;
    if (str(message, "t") == "game-report") it->second.sent = true;
  }
  // A successful rejoin permits one replay of the first game's unanswered commands.
  // An unanswered current start goes first (idempotent at the room): even a successful
  // socket send may have been lost in the outage before the room received either command.
  void rejoined() {
    for (const auto& id : order_) {
      auto& p = games_.at(id);
      if (p.received || p.refused) continue;
      // not_started spends its retry budget permanently, including after that retry
      // was sent. Recovery cannot reset either its send state or its backoff.
      if (p.start_retried || p.report_retried) return;
      if (p.start_sent && !p.start_answered) { p.start_sent = false; p.start_at = 0; }
      if (p.sent) { p.sent = false; p.report_at = 0; }
      return;
    }
  }

  // A room message. True only for this set's first final result.
  bool receive(const json& message) {
    const std::string type = str(message, "t");
    if (type == "start" && match_id_.empty()) {
      match_id_ = str(message, "matchId");
      if (!match_id_.empty()) orphaned_ = false;   // late, but before the pair broke up: play on
      return false;
    }
    if (match_id_.empty() || str(message, "matchId") != match_id_) return false;
    if (type == "game-result") {
      const std::string id = str(message, "id");
      if (id.empty()) return false;
      if (!results_.count(id)) results_[id] = settled_winner(message);   // a repeated receipt counts once
      const auto it = games_.find(id);
      if (it != games_.end()) {
        it->second.start_answered = true;
        it->second.received = true;
      }
      decide();
    } else if (type == "result" && !settled_) {
      settled_ = true;
      return true;
    }
    return false;
  }
  // Text is for logs only. Old rooms without a code refuse finally; not_started alone
  // permits one delayed retry. A final refusal holds the receipt fence until a room result;
  // unconfirmed recovery can only end the set, never advance to another game.
  void refused(const json& message, uint64_t now = 0) {
    now = std::max(now, now_);
    const std::string action = str(message, "action");
    if (action == "begin") return; // a roster may still be settling; the existing cap bounds retries
    if (action == "finish") final_dead_ = true;
    else if (action == "game-report" || action == "game-start") {
      const auto it = games_.find(str(message, "id"));
      if (it == games_.end() || it->second.refused || it->second.received) return;
      auto& p = it->second;
      const std::string code = str(message, "code");
      bool& retried = action == "game-start" ? p.start_retried : p.report_retried;
      if (code == "not_started" && !retried) {
        retried = true;
        if (action == "game-start") { p.start_sent = false; p.start_at = now + kRetryMs; }
        else { p.sent = false; p.report_at = now + kRetryMs; }
      } else {
        // already_recorded is NOT a receipt: the room supplies no winner in the
        // refusal and its welcome/lobby omits completed outcomes. Await the real
        // game-result, then abort on the cleanup bound if it cannot be recovered.
        p.refused = true; // never replay, including after rejoin
        p.recovery_until = now + kRecoveryWaitMs;
      }
      decide();
    }
  }
  void room_closed() { closed_ = true; }

  std::string score_text() const {
    const auto w = wins();
    if (orphaned_ || closed_ || recovery_failed_ || (conceded_ && results_.empty())) return "Set ended";
    const int local = local_ == 1 ? 1 : 0;
    const int game = in_game_ ? games_started_ : games_started_ + 1;
    const auto score = std::to_string(w[local]) + ":" + std::to_string(w[1 - local]);
    return over() ? "Final score - " + score : "Game " + std::to_string(std::max(1, game)) + " - " + score;
  }
  bool started() const { return started_; }
  bool decided() const { return final_requested_; }
  bool settled() const { return settled_; }
  bool closed() const { return closed_; }
  // The game thread breaks the pair: the set has a final, a result, no room any more, or a room
  // that never started its match.
  bool over() const { return final_requested_ || settled_ || closed_ || orphaned_; }
  // GameSetup may still need to send C2 after first-to-two or after a fast room result.
  // Explicit leave, lost link and dead rooms can always escape the presentation fence. A lost
  // link already broke the pair, decided or not (the room decides the set).
  bool release_pair() const {
    if (lost_) return true;
    if (cleanup_ || closed_ || orphaned_ || quit_) return over();
    return game_setup_ ? c2_ >= 0 && settled_ : over();
  }
  // A report still waits for its game-result: no next game may start yet.
  bool awaiting_receipt() const {
    if (settled_ || closed_ || match_id_.empty()) return false;
    return !fence_clear();
  }
  // The room started a match for this set and has not given its result: leaving now could cost it.
  bool needs_drain() const { return !match_id_.empty() && !settled_ && !closed_; }
  const json& final_message() const { return final_; }
  const std::string& local_participant() const {
    static const std::string empty;
    return valid_port(local_) ? participants_[local_] : empty;
  }

 private:
  bool valid_port(int port) const {
    return participants_.size() == 2 && port >= 0 && port < 2 && !participants_[port].empty();
  }
  int port_of(const std::string& participant) const {
    for (int port = 0; port < 2 && port < (int)participants_.size(); ++port)
      if (!participant.empty() && participants_[port] == participant) return port;
    return -1;
  }
  int settled_winner(const json& m) const {
    if (!m.is_object()) return -1;
    const auto flag = [&](const char* key) { const auto it = m.find(key); return it != m.end() && it->is_boolean() && it->get<bool>(); };
    if (flag("void") || flag("draw")) return -1;
    const auto ranking = m.find("ranking");
    if (ranking == m.end() || !ranking->is_array() || ranking->empty() || !(*ranking)[0].is_string()) return -1;
    return port_of((*ranking)[0].get<std::string>());
  }
  std::array<int, 2> wins() const {
    std::array<int, 2> out{};
    for (const auto& [id, winner] : results_) if (winner == 0 || winner == 1) ++out[winner];
    return out;
  }
  bool fence_clear() const {
    for (const auto& id : order_) {
      const auto& p = games_.at(id);
      if ((!p.message.is_null() || p.refused) && !p.received) return false;
    }
    return true;
  }
  void finish(int port) {
    final_requested_ = true;
    final_ = {{"t", "finish"}};
    if (valid_port(port)) final_["winner"] = participants_[port];
    else final_["void"] = true;
  }
  void decide() {
    if (final_requested_ || !started_) return;
    if (forfeit_ >= 0) return finish(forfeit_);
    const auto w = wins();
    for (int port = 0; port < 2; ++port) if (w[port] >= kSetWins) return finish(port);
    if ((int)results_.size() >= kMaxGames) return finish(-1);
    if (!fence_clear()) return;
    // A normal C2 cannot award an early 1-0 set. Abnormal completion voids an
    // undecided set; it cannot override an already requested authoritative final.
    if (c2_ > 0) return finish(-1);
    if (quit_) {
      conceded_ = true;
      return finish(valid_port(local_) ? 1 - local_ : -1);
    }
    if (lost_ && lost_at_ && now_ >= lost_at_ + kLostCallOffMs) return finish(-1);
  }
  struct Pending {
    json start, message;
    bool start_sent = false, start_answered = false;
    bool sent = false, received = false, refused = false;
    bool start_retried = false, report_retried = false;
    uint64_t start_at = 0, report_at = 0, recovery_until = 0;
  };
  // Return true while a game owns the receipt fence, even if no message is due.
  bool next_game_message(std::vector<json>& out, uint64_t now, bool flush) {
    for (const auto& id : order_) {
      auto& p = games_.at(id);
      if (p.received) continue;
      if (p.refused) return true;
      if (p.start_sent && p.message.is_null() && final_requested_) continue;
      if (!p.start_sent) {
        if (flush || now >= p.start_at) {
          p.start["matchId"] = match_id_;
          out.push_back(p.start);
          p.start_at = now + kRetryMs;
        }
      } else if (!p.message.is_null() && !p.sent && (flush || now >= p.report_at)) {
        p.message["matchId"] = match_id_;
        out.push_back(p.message);
        p.report_at = now + kRetryMs;
      }
      return true;
    }
    return false;
  }
  int local_ = -1;
  std::vector<std::string> participants_, order_;
  std::map<std::string, Pending> games_;
  std::map<std::string, int> results_;   // game id -> settled winner port, -1 none
  std::string match_id_, active_id_, active_key_;
  int games_started_ = 0, forfeit_ = -1, c2_ = -1, begin_tries_ = 0;
  bool started_ = false, in_game_ = false, game_setup_ = false, quit_ = false, lost_ = false, conceded_ = false;
  bool begin_dead_ = false, final_requested_ = false, final_sent_ = false, final_dead_ = false;
  bool settled_ = false, closed_ = false, orphaned_ = false, cleanup_ = false, recovery_failed_ = false;
  uint64_t begin_at_ = 0, final_at_ = 0, waiting_since_ = 0, now_ = 0, cleanup_at_ = 0, lost_at_ = 0;
  json final_;
};

// YouGame's authoritative tier/division compressed to Slippi's three divisions (E3/E5 and the
// match-state rank bytes; nothing in the native game draws them yet).
inline uint8_t rank_enum(const json& rank) {
  if (!rank.is_object() || !rank.count("placed") || !rank["placed"].is_boolean() || !rank["placed"].get<bool>()) return 0;
  const std::string tier = str(rank, "tier");
  if (tier == "challenger") return 19; // Grandmaster
  static const std::array<const char*, 5> tiers = {"bronze", "silver", "gold", "platinum", "diamond"};
  const auto it = std::find(tiers.begin(), tiers.end(), tier);
  if (it == tiers.end() || !rank.count("division") || !rank["division"].is_number_integer()) return 0;
  const int division = rank["division"].get<int>();
  if (division < 1 || division > 4) return 0;
  return (uint8_t)(1 + 3 * (it - tiers.begin()) + (division == 1 ? 2 : division == 2 ? 1 : 0));
}

// The rank's name as YouGame writes it (web/src/lib/ranks.ts rankFor's `label`: "Gold II",
// "Challenger", "Placements 2/5"), kept to plain ASCII the game's text can show. Built from the
// tier and division with ranks.ts's names when the label is missing or unusable.
inline std::string rank_label(const json& rank, bool placed, int64_t matches) {
  std::string label;
  for (char c : str(rank, "label"))
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '/') label += c;
  if (!label.empty() && label.size() <= 23) return label;
  if (!placed) {
    int64_t total = 5;   // ranks.ts PLACEMENT_MATCHES
    if (rank.count("placementTotal") && rank["placementTotal"].is_number_integer()) total = rank["placementTotal"].get<int64_t>();
    if (total < 1 || total > 99) total = 5;
    return "Placements " + std::to_string(std::min(matches, total)) + "/" + std::to_string(total);
  }
  const std::string tier = str(rank, "tier");
  if (tier == "challenger") return "Challenger";
  static const std::array<std::pair<const char*, const char*>, 5> names = {
      {{"bronze", "Bronze"}, {"silver", "Silver"}, {"gold", "Gold"}, {"platinum", "Platinum"}, {"diamond", "Diamond"}}};
  static const char* roman[] = {"", "I", "II", "III", "IV"};
  for (const auto& [id, name] : names) {
    if (tier != id) continue;
    if (!rank.count("division") || !rank["division"].is_number_integer()) return name;
    const int division = rank["division"].get<int>();
    return division >= 1 && division <= 4 ? std::string(name) + " " + roman[division] : std::string(name);
  }
  return {};
}

inline MuYgRank rank_record(const json& rank, double rating, int64_t matches) {
  MuYgRank out{};
  out.known = 1;
  out.placed = rank["placed"].get<bool>();
  static const std::array<const char*, 6> tiers = {"bronze", "silver", "gold", "platinum", "diamond", "challenger"};
  const auto tier = std::find(tiers.begin(), tiers.end(), str(rank, "tier"));
  out.tier = tier == tiers.end() ? 0 : uint8_t(1 + tier - tiers.begin());
  const auto bounded = [&](const char* key, unsigned maximum, unsigned fallback = 0) {
    if (!rank.count(key) || !rank[key].is_number_integer()) return fallback;
    const double value = rank[key].get<double>();
    return value >= 0 && value <= maximum ? unsigned(value) : fallback;
  };
  out.division = uint8_t(bounded("division", 4));
  out.order = uint16_t(bounded("order", 65535));
  out.rating = int32_t(std::lround(rating * 10));
  out.matches = uint16_t(std::min<int64_t>(matches, 65535));
  out.placement_total = uint8_t(bounded("placementTotal", 255, 5));
  const auto label = rank_label(rank, out.placed, matches);
  std::memcpy(out.label, label.data(), std::min(label.size(), sizeof out.label - 1));
  return out;
}
inline std::vector<uint8_t> rank_record_response(const MuYgRank& self, const MuYgRank& opponent,
                                                  const MuYgResult& result) {
  const MuYgRanks records{self, opponent, result};
  std::vector<uint8_t> out(MU_YG_RANK_RESPONSE_SIZE);
  mu_yg_ranks_encode(out.data(), &records); return out;
}

// The local player's ranked standing for this process: from a ranked ticket, the bridge's mpRank,
// or a set's final result, whichever is newest. Shown in the Ranked character select's header.
class RankCache {
 public:
  // A ranks.ts Rank object ({tier, division, label, placed, matches, rating, ...}) and the rating
  // beside it (the rank's own rating when `rating` is not a number). False when it is unusable, or
  // malformed. Zero matches is a real standing: Placements 0/5.
  bool standing(const json& rank, const json& rating, float change = 0) {
    if (!rank.is_object() || !rank.count("placed") || !rank["placed"].is_boolean()) return false;
    const json& r = rating.is_number() ? rating : rank.count("rating") ? rank["rating"] : json();
    if (!r.is_number() || !rank.count("matches") || !(rank["matches"].is_number_integer() || rank["matches"].is_number_unsigned()))
      return false;
    const double value = r.get<double>();
    const double count = rank["matches"].get<double>();
    if (count < 0 || count > INT32_MAX) return false;
    const int64_t matches = (int64_t)count;
    const bool placed = rank["placed"].get<bool>();
    if (!std::isfinite(value) || std::abs(value) > 1000000 || matches < 0 || matches > INT32_MAX) return false;
    known_ = true;
    placed_ = placed;
    rating_ = (float)value;
    change_ = change;
    matches_ = (uint32_t)matches;
    rank_ = rank_enum(rank);
    label_ = rank_label(rank, placed, matches);
    rank_object_ = rank;
    record_ = rank_record(rank, value, matches);
    rank_delta_ = 0;
    ++version_;
    return true;
  }
  // A set's final result: ratings[participant] = {before, after, matches, rank, rankBefore}.
  bool result(const json& event, const std::string& participant) {
    if (str(event, "t") != "result" || str(event, "queue") != "ranked" || !event.count("ratings") ||
        !event["ratings"].is_object()) return false;
    const auto it = event["ratings"].find(participant);
    if (it == event["ratings"].end() || !it->is_object()) return false;
    const auto& r = *it;
    if (!r.count("before") || !r["before"].is_number() || !r.count("after") || !r["after"].is_number() ||
        !r.count("rank") || !r["rank"].is_object()) return false;
    const double before = r["before"].get<double>(), after = r["after"].get<double>();
    if (!std::isfinite(before) || std::abs(before) > 1000000) return false;
    json rank = r["rank"];
    if (r.count("matches") && (r["matches"].is_number_integer() || r["matches"].is_number_unsigned())) rank["matches"] = r["matches"];
    if (!standing(rank, r["after"], (float)(after - before))) return false;
    // Preserve the complete objects, including rankBefore: order (not rating delta)
    // determines YouGame promotion/demotion, and IV/Challenger must not be compressed.
    rank_before_ = r.value("rankBefore", json());
    result_record_ = {};
    result_record_.status = MU_YG_RESULT_RECORDED;
    result_record_.before = int32_t(std::lround(before * 10));
    result_record_.after = record_.rating;
    result_record_.rank_after = record_;
    if (rank_before_.is_object() && rank_before_.count("placed") && rank_before_["placed"].is_boolean()) {
      json previous = rank_before_;
      if (!previous.count("matches")) previous["matches"] = matches_ ? matches_ - 1 : 0;
      RankCache old;
      if (old.standing(previous, r["before"])) result_record_.rank_before = old.record();
      rank_delta_ = int8_t(int(rank_) - int(rank_enum(rank_before_)));
    }
    return true;
  }
  void update(const RankCache& other) {
    if (!other.known()) return;
    const auto version = version_ + 1;
    *this = other; version_ = version;
  }
  void clear() { const auto version = version_ + 1; *this = RankCache(); version_ = version; }
  const MuYgRank& record() const { return record_; }
  const MuYgResult& result_record() const { return result_record_; }
  const json& rank_object() const { return rank_object_; }
  const json& rank_before() const { return rank_before_; }
  uint64_t version() const { return version_; }
  bool known() const { return known_; }
  // The Ranked character select's header: "Ranked Mode" before any result, then
  // "Ranked - Placements 2/5" while placing and "Ranked - Gold II - 1534" once placed.
  std::string header() const {
    if (!known_) return "Ranked Mode";
    const std::string rating = std::to_string((long long)std::lround(rating_));
    if (!placed_) return label_.empty() ? "Ranked Mode" : "Ranked - " + label_;
    return label_.empty() ? "Ranked - " + rating : "Ranked - " + label_ + " - " + rating;
  }
  uint8_t visibility() const { return known_ ? 1 : 0; } // only the local rank is known
  uint8_t rank() const { return known_ ? rank_ : 0; }
  std::vector<uint8_t> response() const {
    std::vector<uint8_t> out = {visibility(), 1, rank()};
    append_float(out, rating_);
    append_word(out, matches_);
    append_float(out, change_);
    // ExiSlippi.h:145-167: 1 = FETCHED. rankChange is a signed delta;
    // RankInfo.c:227 reconstructs the previous rank as rank - rankChange.
    out.push_back((uint8_t)rank_delta_);
    return out;
  }

 private:
  static void append_word(std::vector<uint8_t>& out, uint32_t word) {
    for (int i = 3; i >= 0; --i) out.push_back((uint8_t)(word >> (8 * i)));
  }
  static void append_float(std::vector<uint8_t>& out, float value) {
    uint32_t bits; std::memcpy(&bits, &value, sizeof bits); append_word(out, bits);
  }
  MuYgRank record_{};
  MuYgResult result_record_{};
  json rank_object_, rank_before_;
  int8_t rank_delta_ = 0;
  bool known_ = false, placed_ = false;
  uint8_t rank_ = 0;
  uint32_t matches_ = 0;
  float rating_ = 0, change_ = 0;
  std::string label_;
  uint64_t version_ = 0;
};

// Live bridge shape. A signed-out answer clears any previous account's standing.
inline bool apply_rank_answer(RankCache& cache, const json& result, uint64_t asked) {
  if (!result.is_object() || cache.version() != asked || str(result, "queue") != "ranked" ||
      str(result, "mode") != "singles") return false;
  if (result.count("player") && result["player"].is_null() &&
      result.count("rank") && result["rank"].is_null() && str(result, "reason") == "signed_out") {
    cache.clear();
    return true;
  }
  if (str(result, "player").empty() || !result.count("rank")) return false;
  return cache.standing(result["rank"], json());
}
inline bool unknown_request(const std::string& error) { return error.rfind("Unknown request", 0) == 0; }

// One read per visit to Ranked CSS, including failed/signed-out reads. The throttle survives a
// screen change; polling the UI cannot create a retry loop or erase a cached standing on 429.
struct RankRead {
  bool unsupported = false, in_flight = false, asked = false, enabled = false;
  uint64_t next_at = 0, screen = 0, recovery_at = 0;
  int recovery_tries = 0;
  void enter(bool same_set = false) {
    if (same_set) return;
    ++screen; asked = false; enabled = true;
  }
  // Only a closed room missing its final receipt schedules this bounded recovery read.
  void recover(uint64_t now) { recovery_at = now + 20000; recovery_tries = 0; }
  bool recovery_due(uint64_t now) const { return recovery_at && recovery_tries < 2 && now >= recovery_at; }
  bool begin(uint64_t now) {
    if (unsupported || in_flight || now < next_at) return false;
    const bool recovery = recovery_due(now);
    if (!recovery && (!enabled || asked)) return false;
    if (recovery) { ++recovery_tries; recovery_at = now + 20000; }
    asked = in_flight = true;
    return true;
  }
  void finish(uint64_t now, bool failed, bool unsupported_request = false) {
    in_flight = false;
    unsupported = unsupported_request;
    if (failed) next_at = now + 60000;
  }
  bool answer(RankCache& cache, const json& result, uint64_t version, uint64_t now,
              bool failed = false, bool unsupported_request = false) {
    failed = failed || (result.is_object() && result.count("status") && result["status"] == 429);
    finish(now, failed, unsupported_request);
    return !failed && apply_rank_answer(cache, result, version);
  }
};

inline void set_contract(json& request) {
  request["bestOf"] = 3;
  request["winsRequired"] = 2;
  request["rulesVersion"] = "mu-ranked-1";
}

inline bool set_contract_matches(const json& ticket) {
  return ticket.is_object() && ticket.count("bestOf") && ticket["bestOf"] == 3 &&
         ticket.count("winsRequired") && ticket["winsRequired"] == 2 &&
         str(ticket, "rulesVersion") == "mu-ranked-1";
}

inline RankCache roster_rank(const json& participants, int slot) {
  RankCache cache;
  if (!participants.is_array()) return cache;
  for (const auto& p : participants) {
    if (!p.is_object() || !p.count("slot") || !p["slot"].is_number_integer() || p["slot"] != slot ||
        !p.count("rank")) continue;
    cache.standing(p["rank"], p.count("rating") ? p["rating"] : json());
    break;
  }
  return cache;
}
inline uint8_t roster_rank_byte(const json& participants, int slot) {
  const auto cache = roster_rank(participants, slot);
  return cache.known() ? cache.rank() : 0xFF;
}

// Only the set's final receipt can show its rating change. If the retired room missed that
// receipt, a bounded mpRank recovery may show the current standing, explicitly without a delta.
class ResultsPanel {
 public:
  enum class State { Empty, Pending, Rated, Unrated, Standing, Error };
  void pending(const std::string& match) {
    if (match == match_ && state_ != State::Empty) return;
    match_ = match; state_ = State::Pending; rating_.clear(); change_.clear();
    record_ = {}; record_.status = MU_YG_RESULT_RECORDING;
  }
  bool receive(const json& event, const std::string& participant) {
    if ((state_ != State::Pending && state_ != State::Standing && state_ != State::Error) || match_.empty() || str(event, "matchId") != match_ ||
        str(event, "t") != "result" || str(event, "queue") != "ranked" ||
        !event.count("ratingDisposition") || !event["ratingDisposition"].is_object()) return false;
    const auto& disposition = event["ratingDisposition"];
    if (!disposition.count("rated") || !disposition["rated"].is_boolean()) return false;
    if (!disposition["rated"].get<bool>()) {
      const auto reason = str(disposition, "reason");
      rating_ = reason == "pack" ? "Unrated: custom content" :
                reason == "void" ? "Unrated: no winner" : "Unrated";
      record_ = {}; record_.status = MU_YG_RESULT_UNRATED;
      record_.reason = reason == "pack" ? MU_YG_REASON_PACK : reason == "void" ? MU_YG_REASON_VOID : MU_YG_REASON_OTHER;
      state_ = State::Unrated;
      return true;
    }
    if (!event.count("ratings") || !event["ratings"].is_object() || !event["ratings"].count(participant)) return false;
    const auto& r = event["ratings"][participant];
    RankCache after;
    if (!after.result(event, participant) || !r.count("rankBefore") || !r["rankBefore"].is_object()) return false;
    const auto& before_rank = r["rankBefore"];
    const auto& rank = r["rank"];
    const double before = r["before"].get<double>(), value = r["after"].get<double>();
    char line[96];
    std::snprintf(line, sizeof line, "%.0f -> %.0f (%+.0f)", before, value, value - before);
    rating_ = line;
    const auto placed = [](const json& j) { return j.count("placed") && j["placed"].is_boolean() && j["placed"].get<bool>(); };
    const auto order = [](const json& j) -> int64_t { return j.count("order") && j["order"].is_number_integer() ? j["order"].get<int64_t>() : 0; };
    const std::string label = rank_label(rank, placed(rank), rank.count("matches") && rank["matches"].is_number_integer() ? rank["matches"].get<int64_t>() : 0);
    if (!placed(rank)) change_ = label;
    else if (!placed(before_rank)) change_ = "Placed: " + label;
    else if (order(rank) > order(before_rank)) change_ = "Promotion: " + label;
    else if (order(rank) < order(before_rank)) change_ = "Demotion: " + label;
    else change_ = label;
    record_ = after.result_record();
    // A set the room gave by a leave: the winner's setup screen says so (CTO 11-01).
    if (str(event, "disposition") == "forfeit" && event.count("ranking") && event["ranking"].is_array() &&
        !event["ranking"].empty())
      record_.reason = event["ranking"][0] == participant ? MU_YG_REASON_FORFEIT_WON : MU_YG_REASON_FORFEIT_LOST;
    state_ = State::Rated;
    return true;
  }
  const MuYgResult& record() const { return record_; }
  // An accepted final lacking its required rating contract is a protocol error,
  // not an endless recording spinner. Late valid room receipts may still repair it.
  void result_error(uint8_t reason = MU_YG_REASON_MALFORMED) {
    record_ = {}; record_.status = MU_YG_RESULT_ERROR; record_.reason = reason;
    state_ = State::Error; rating_ = "Result unavailable"; change_.clear();
  }
  State state() const { return state_; }
  std::string rating_text() const { return state_ == State::Pending ? "Result being recorded" : rating_; }
  const std::string& change_text() const { return change_; }
  const std::string& match() const { return match_; }
  // mpRank cannot reconstruct a set result. Show its current standing explicitly, without
  // inventing a rating delta or claiming this set was rated.
  void recovered_standing(const std::string& match, const RankCache& cache, bool exhausted) {
    if (match != match_ || state_ != State::Pending) return;
    if (cache.known()) {
      result_error(MU_YG_REASON_UNAVAILABLE);
      state_ = State::Standing;
      rating_ = "Current standing";
      change_ = cache.header().substr(9);
    } else if (exhausted) {
      result_error(MU_YG_REASON_UNAVAILABLE);
      state_ = State::Standing; rating_ = "Result unavailable";
    }
  }
 private:
  MuYgResult record_{};
  State state_ = State::Empty;
  std::string match_, rating_, change_;
};
} // namespace yougame::ranked
