// Pickup tournament SDK state. Pure policy, shared by the host and offline tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_tourney.h"

namespace yougame::tourney {
class Queue {
 public:
  void hello(const json& j) {
    const int size = listing_size(j);
    available_ = size != 0;
    if (size && !joined_) size_ = size;
  }
  bool available() const { return available_; }
  bool visible() const { return visible_; }
  bool joined() const { return joined_; }
  // One SDK request at a time. A leave waits for an in-flight join, so an old
  // join answer can never put the player back into a queue they already left.
  void join() {
    if (!available() || visible_ || busy_) return;
    visible_ = joined_ = true; join_pending_ = true;
    phase_ = "queue"; error_ = false; count_ = 0; fills_.clear(); terminal_until_ = 0;
  }
  void leave() {
    if (!visible_) return;
    visible_ = joined_ = false;
    leave_pending_ = true; join_pending_ = false;
  }
  std::string request(uint64_t now) {
    if (busy_) return {};
    std::string kind;
    if (leave_pending_) { kind = "tourney.leave"; leave_pending_ = false; }
    else if (join_pending_) { kind = "tourney.join"; join_pending_ = false; }
    else if (joined_ && now >= next_poll_) { kind = "tourney.state"; next_poll_ = now + 1000; }
    if (!kind.empty()) busy_ = true;
    return kind;
  }
  void answer(const std::string& kind, const json& state, uint64_t now, bool failed = false) {
    busy_ = false;
    if (kind == "tourney.leave" || !joined_) return;
    if (failed || !state.is_object() || state.count("error")) {
      error_ = true;
      if (kind == "tourney.join") joined_ = false;
      next_poll_ = now + 1000;
      return;
    }
    if (kind == "tourney.join") return; // join returns AutoQueue, not TournamentGameState
    for (const char* field : {"phase", "size", "count", "rounds", "round"})
      if (!state.count(field)) return;
    if (!state["phase"].is_string() || !integer(state["size"], 2, 1024) ||
        !bracket_rounds(state["size"]) || !integer(state["count"], 0, state["size"]) ||
        !integer(state["rounds"], 1, 10) || state["rounds"] != bracket_rounds(state["size"]) ||
        !integer(state["round"], 0, state["rounds"])) return;
    const std::string phase = state["phase"];
    if (phase != "none" && phase != "queue" && phase != "waiting" && phase != "ready" &&
        phase != "playing" && phase != "out" && phase != "champion") return;
    state_ = state; phase_ = phase; size_ = state["size"]; count_ = state["count"]; error_ = false;
    fills_ = state.count("fillsAt") && state["fillsAt"].is_string() ? state["fillsAt"].get<std::string>() : "";
    if (phase == "out" || phase == "champion") { joined_ = false; terminal_until_ = now + 5000; }
  }
  void tick(uint64_t now) {
    if (terminal_until_ && now >= terminal_until_) { visible_ = false; terminal_until_ = 0; }
  }
  std::string text() const {
    if (error_) return "Tournament unavailable. Hold B to leave";
    if (phase_ == "out" || phase_ == "champion") {
      const std::string place = phase_ == "champion" ? "1st" :
        state_.count("place") && state_["place"].is_string() ? state_["place"].get<std::string>() : "Out";
      return "Tournament place: " + place.substr(0, 20);
    }
    std::string fill = fills_.size() >= 19 ? fills_.substr(11, 8) + (fills_.back() == 'Z' ? " UTC" : fills_.substr(fills_.size() - 6)) : "soon";
    return "Tournament: " + std::to_string(count_) + " of " + std::to_string(size_) +
      " players | CPUs fill " + fill + " | Hold B to leave";
  }
  // Only the identity-projected, undecided pairing involving YOU can offer a CPU
  // game. Human rooms continue through the page's existing onReady path.
  json assignment(const json& launch) const {
    if (!joined_ || (phase_ != "ready" && phase_ != "playing") || !state_.count("bracket") ||
        !state_["bracket"].is_array() || !state_.count("tournament_id") || !state_.count("deadline")) return nullptr;
    const auto offered = parse(launch, nullptr, size_);
    if (!offered || offered->tournament_id != state_["tournament_id"] ||
        offered->deadline != state_["deadline"]) return nullptr;
    for (const auto& m : state_["bracket"]) {
      if (!m.is_object() || !m.count("round") || m["round"] != state_["round"] ||
          !m.count("slot") || !m.count("stage") || !m.count("winner") || !m["winner"].is_null() ||
          !m.count("p1") || !m.count("p2") || !m["p1"].is_object() || !m["p2"].is_object()) continue;
      const auto& a = m["p1"]; const auto& b = m["p2"];
      const auto is_you = [](const json& p) { return p.count("you") && p["you"].is_boolean() && p["you"].get<bool>(); };
      if (is_you(a) == is_you(b)) continue;
      const auto& cpu = is_you(a) ? b : a;
      if (!cpu.count("cpu") || !cpu["cpu"].is_object() || !cpu["cpu"].count("character")) continue;
      if (offered->round != m["round"] || offered->slot != m["slot"] || offered->stage != m["stage"] ||
          offered->character != cpu["cpu"]["character"]) continue;
      // Compact state omits CPU level. Keep every gameplay field from the site's
      // authoritative tourneyMatch; state only authorizes which pairing may start.
      json hello = launch;
      hello["tournament"] = {{"size", size_}};
      return hello;
    }
    return nullptr;
  }
 private:
  int size_ = 0, count_ = 0;
  bool available_ = false, visible_ = false, joined_ = false, busy_ = false, join_pending_ = false, leave_pending_ = false, error_ = false;
  uint64_t next_poll_ = 0, terminal_until_ = 0;
  std::string phase_, fills_;
  json state_;
};
} // namespace yougame::tourney
