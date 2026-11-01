// Console intake is independent of Friends' scene and matchmaking state.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_friends.h"
#include <optional>
#include <functional>

namespace yougame::console {
using json = nlohmann::json;
inline std::string session_id(const json& j) {
  const auto id = friends::text(j,"sessionId",64);
  return id.size() <= 64 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos ? id : std::string();
}
inline bool number(const json& j, const char* key, uint64_t lo, uint64_t hi) {
  const auto p = j.find(key);
  return p != j.end() && p->is_number_integer() &&
      (p->is_number_unsigned() || p->get<int64_t>() >= 0) &&
      p->get<uint64_t>() >= lo && p->get<uint64_t>() <= hi;
}
// Rooms runs in JavaScript: carry all 64 seed bits as 16 hex digits on that hop.
inline bool read_seed(const json& j, uint64_t& seed) {
  const auto p = j.find("seed");
  if (p == j.end()) return false;
  if (number(j,"seed",0,UINT64_MAX)) { seed = p->get<uint64_t>(); return true; }
  if (!p->is_string()) return false;
  const auto s = p->get<std::string>();
  if (s.size() != 16) return false;
  seed = 0;
  for (char c : s) {
    const int digit = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
    if (digit < 0) return false;
    seed = (seed << 4) | uint64_t(digit);
  }
  return true;
}
// App values are expectations, never authority over a room-assigned seat/start.
inline bool parse_request(const json& j, json& out) {
  if (!j.is_object() || !j.count("room") || !j["room"].is_string() ||
      !number(j, "players", 2, 4)) return false;
  const auto room = j["room"].get<std::string>();
  if (room != "new" && friends::room(j).empty()) return false;
  if (j.count("type") && j["type"] != "consoleSession") return false;
  if (j.count("sessionId") && session_id(j).empty()) return false;
  uint64_t seed = 0;
  if (j.count("seed") && !read_seed(j,seed)) return false;
  for (auto key : {"port", "epochMs", "inputDelay"}) {
    if (!j.count(key)) continue;
    const uint64_t hi = std::string(key) == "port" ? j["players"].get<int>() :
        std::string(key) == "inputDelay" ? 8 : std::string(key) == "epochMs" ? INT64_MAX : UINT64_MAX;
    if (!number(j, key, std::string(key) == "port" || std::string(key) == "inputDelay" ? 1 : 0, hi)) return false;
  }
  if (j.count("sessionId")) {
    if (room == "new") return false;
    for (auto key : {"port","seed","epochMs","inputDelay"}) if (!j.count(key)) return false;
  }
  if (j.count("cardDigest") && (!j["cardDigest"].is_string() ||
      j["cardDigest"].get<std::string>().size() != 64 ||
      j["cardDigest"].get<std::string>().find_first_not_of("0123456789abcdef") != std::string::npos)) return false;
  if (j.count("card")) return false; // Save choice is game-local, never an app launch field.
  out = {{"type", "consoleSession"}, {"room", room}, {"players", j["players"]}};
  for (auto key : {"sessionId", "port", "seed", "epochMs", "inputDelay", "cardDigest"})
    if (j.count(key)) out[key] = j[key];
  if (j.count("seed")) out["seed"] = seed;
  return true;
}
class Requests {
 public:
  bool push(const json& j, bool only_if_empty = false, const std::function<bool()>& before_publish = {}) {
    json parsed;
    if (!parse_request(j, parsed)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ || (only_if_empty && pending_)) return false;
    if (before_publish && !before_publish()) return false;
    pending_ = std::move(parsed); return true;
  }
  bool pending() { std::lock_guard<std::mutex> lock(mutex_); return bool(pending_); }
  std::optional<json> take() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(pending_, std::nullopt);
  }
  bool claim() { std::lock_guard<std::mutex> lock(mutex_); if (active_) return false; active_ = true; pending_.reset(); return true; }
  void clear() { std::lock_guard<std::mutex> lock(mutex_); pending_.reset(); }
  void release() { std::lock_guard<std::mutex> lock(mutex_); active_ = false; }
 private:
  std::mutex mutex_;
  bool active_ = false;
  std::optional<json> pending_;
};
inline Requests requests;
// Engine boot polls this after the bridge hello and agent defaults have been read.
inline std::optional<json> take_request() { return requests.take(); }
inline bool launch_request(const json& j) {
  return j.is_object() && j.value("type", json()) == "consoleSession" && requests.push(j);
}
inline void launch_hello(const json& j) {
  if (j.is_object() && j.count("launch") && j["launch"].is_object() && j["launch"].value("type",json()) == "consoleSession") requests.push(j["launch"],true);
}
} // namespace yougame::console
