// Pure CPU bracket contract and delivery policy, independent of networking and game memory.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <chrono>
#include <set>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include "../abi/mu_tourney.h"

namespace yougame::tourney {
using json = nlohmann::json;
struct Match {
  std::string tournament_id, deadline;
  int round = 0, slot = 0, stage = 0, character = 0, level = 0;
  std::chrono::system_clock::time_point deadline_utc;
};
inline bool integer(const json& j, int lo, int hi) {
  return j.is_number_integer() && j >= lo && j <= hi;
}
// Zero means a legacy direct assignment without a listing size. Queue assignments always
// supply the site's size; no fixed bracket size is assumed by the native client.
inline int bracket_rounds(int size) {
  if (size < 2 || size > 1024 || (size & (size - 1))) return 0;
  int rounds = 0;
  while (size > 1) { ++rounds; size >>= 1; }
  return rounds;
}
inline int listing_size(const json& hello) {
  if (!hello.is_object() || !hello.count("tournament")) return 0;
  const auto& t = hello["tournament"];
  return t.is_object() && t.count("size") && integer(t["size"], 2, 1024) &&
         bracket_rounds(t["size"]) ? t["size"].get<int>() : 0;
}
inline std::optional<Match> parse(const json& hello, std::string* diagnostic = nullptr, int size = 0) {
  if (!hello.is_object() || !hello.count("tourneyMatch")) return {};
  const auto& j = hello["tourneyMatch"];
  if (!j.is_object()) return {};
  for (const auto* field : {"tournament_id", "round", "slot", "stage", "cpu", "deadline"})
    if (!j.count(field)) return {};
  if (!j["tournament_id"].is_string() || !j["deadline"].is_string() || !j["cpu"].is_object()) return {};
  const auto& cpu = j["cpu"];
  if (!cpu.count("character") || !cpu.count("level") || !integer(j["round"], 1, size ? bracket_rounds(size) : 10) ||
      !integer(j["slot"], 0, 1023) || !integer(j["stage"], 0, 32) ||
      !integer(cpu["character"], 0, 25) || !integer(cpu["level"], 1, 9)) return {};
  Match m{j["tournament_id"], j["deadline"], j["round"], j["slot"], j["stage"], cpu["character"], cpu["level"]};
  static const std::regex uuid("^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$");
  static const std::regex iso("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,6})?(Z|[+-][0-9]{2}:[0-9]{2})$");
  if (m.tournament_id.size() != 36 || m.deadline.size() < 20 || m.deadline.size() > 32) return {};
  if (!std::regex_match(m.tournament_id, uuid) || !std::regex_match(m.deadline, iso) ||
      (size && m.slot >= (size >> m.round))) return {};
  // Site contract: tournament-cpu.ts excludes both transforming characters.
  if (m.character == 18 || m.character == 19) {
    if (diagnostic) *diagnostic = "tournament: CPU character " + std::to_string(m.character) + " not offered by YouGame";
    return {};
  }
  // ISO syntax alone accepts impossible dates/times. Supabase sends UTC offsets or Z.
  const auto number = [&](size_t at, size_t len) { return std::stoi(m.deadline.substr(at, len)); };
  const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
  const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  if (!year || month < 1 || month > 12 || day < 1 || day > days[month] + (month == 2 && leap) ||
      number(11, 2) > 23 || number(14, 2) > 59 || number(17, 2) > 59) return {};
  if (m.deadline.back() != 'Z') {
    const size_t zone = m.deadline.size() - 6;
    if (number(zone + 1, 2) > 23 || number(zone + 4, 2) > 59) return {};
  }
  // Gregorian civil date -> Unix days, independent of the machine's local timezone.
  int y = year - (month <= 2);
  const int era = y / 400, yoe = y - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t unix_days = int64_t(era) * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  int64_t seconds = unix_days * 86400 + number(11, 2) * 3600 + number(14, 2) * 60 + number(17, 2);
  if (m.deadline.back() != 'Z') {
    const size_t zone = m.deadline.size() - 6;
    const int offset = number(zone + 1, 2) * 3600 + number(zone + 4, 2) * 60;
    seconds -= m.deadline[zone] == '+' ? offset : -offset;
  }
  int micros = 0;
  if (m.deadline[19] == '.') {
    size_t i = 20;
    int scale = 100000;
    while (i < m.deadline.size() && m.deadline[i] >= '0' && m.deadline[i] <= '9') {
      micros += (m.deadline[i++] - '0') * scale; scale /= 10;
    }
  }
  // Limit to the system_clock range before converting (nanosecond clocks cannot hold year 9999).
  const auto duration = std::chrono::seconds(seconds) + std::chrono::microseconds(micros);
  const auto max = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::duration::max());
  const auto min = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::duration::min());
  if (duration > max || duration < min) return {};
  m.deadline_utc = std::chrono::system_clock::time_point(duration);
  switch (m.stage) { case 2: case 3: case 8: case 28: case 31: case 32: return m; default: return {}; }
}

// Game supplies [method, frames BE32, human stocks, CPU stocks, winner (1/2)].
// Its winner helper resolves every undecidable game to CPU side 2 before crossing the ABI.
// Quit and LRAS are losses even if a player quits with more stocks. Never invent frames or stocks.
inline json result(const Match& m, const uint8_t* p, size_t n) {
  if (!p || n != MU_TOURNEY_RESULT_SIZE || p[5] > 4 || p[6] > 4) return nullptr;
  const uint32_t frames = uint32_t(p[1]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 8 | p[4];
  if (frames > 72000) return nullptr;
  const bool quit = p[0] == 0 || p[0] == 7;
  if (!quit && p[0] != 1 && p[0] != 2) return nullptr;
  if (!quit && p[7] != 1 && p[7] != 2) return nullptr; // an unresolved tie is not a result
  const bool tie = p[0] == 2 && p[5] == 0 && p[6] == 0;
  const int winner = quit || tie ? 2 : p[7];
  const bool won = winner == 1;
  if (p[0] == 2 && !tie && (p[won ? 6 : 5] != 0 || p[won ? 5 : 6] == 0)) return nullptr;
  return {{"type", "tourney.result"}, {"tournament_id", m.tournament_id}, {"round", m.round},
          {"slot", m.slot}, {"won", won}, {"frames", frames},
          {"end", quit ? "quit" : p[0] == 1 ? "time" : tie ? "tie" : "stocks"},
          {"stocks", {p[5], p[6]}}, {"winner", winner}};
}

// An ordinary first hello seals ordinary boot. After a bracket scene has unloaded,
// fresh hello answers may offer a NEW match; previously consumed coordinates never replay.
class Session {
 public:
  using Clock = std::chrono::steady_clock;
  std::string hello(const json& j, std::chrono::system_clock::time_point wall = std::chrono::system_clock::now(),
                    Clock::time_point now = Clock::now()) {
    if (seen_ && !awaiting_) return {};
    std::string diagnostic;
    auto offered = parse(j, &diagnostic, listing_size(j));
    if (!offered) {
      seen_ = true;
      return diagnostic.empty() && j.is_object() && j.count("tourneyMatch") ? "tournament: malformed tourneyMatch" : diagnostic;
    }
    const auto key = offered->tournament_id + ":" + std::to_string(offered->round) + ":" + std::to_string(offered->slot);
    if (consumed_.count(key)) return {};
    seen_ = true;
    consumed_.insert(key);
    // One wall-clock sample at hello; all later checks use the monotonic clock.
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(offered->deadline_utc.time_since_epoch()) -
                           std::chrono::duration_cast<std::chrono::microseconds>(wall.time_since_epoch()) + std::chrono::seconds(60);
    if (remaining < std::chrono::microseconds::zero()) return "tournament: past deadline";
    match_ = std::move(offered);
    const auto capacity = std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - now);
    deadline_ = remaining >= capacity ? Clock::time_point::max() :
                now + std::chrono::duration_cast<Clock::duration>(remaining);
    launched_ = finished_ = in_flight_ = expired_ = awaiting_ = false;
    return {};
  }
  void assignment(const json& hello) {
    if (match_ || !listing_size(hello)) return;
    awaiting_ = true;
    this->hello(hello);
  }
  const std::optional<Match>& match() const { return match_; }
  bool expired(Clock::time_point now = Clock::now()) {
    if (match_ && now > deadline_) expired_ = true;
    return expired_;
  }
  bool pending(Clock::time_point now = Clock::now()) { return match_.has_value() && !launched_ && !expired(now); }
  bool awaiting() const { return awaiting_; }
  std::optional<Match> launch(Clock::time_point now = Clock::now()) {
    if (!pending(now)) return {};
    launched_ = true;
    return match_;
  }
  json finish(const uint8_t* p, size_t n, Clock::time_point now = Clock::now()) {
    if (!match_ || finished_ || in_flight_ || expired(now)) return nullptr;
    auto report = result(*match_, p, n);
    if (!report.is_null()) in_flight_ = true;
    return report;
  }
  void answered() { if (in_flight_) { in_flight_ = false; finished_ = true; } }
  bool in_flight() const { return in_flight_; }
  bool finished() const { return finished_; }
  void release() {
    match_.reset(); launched_ = finished_ = in_flight_ = expired_ = false; awaiting_ = true;
  }
 private:
  bool seen_ = false, launched_ = false, finished_ = false, in_flight_ = false, expired_ = false, awaiting_ = false;
  Clock::time_point deadline_{};
  std::optional<Match> match_;
  std::set<std::string> consumed_;
};

enum class Reply { Answer, NoAnswer };
// Reconnect means an answered hello over the same SDK channel. The exact report is retried
// once only after no answer; an explicit rejection (including too_early/past_deadline) is final.
template<class Request, class Reconnect>
int deliver(const json& report, Request request, Reconnect reconnect) {
  if (request(report) == Reply::Answer) return 1;
  if (!reconnect()) return 1;
  request(report);
  return 2;
}
} // namespace yougame::tourney
