// Offline update policy: an app notice is advisory; only an explicit server code refuses a search.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <string>
#include <stdexcept>

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

namespace yougame::update {
inline constexpr const char* kMessage = "A new version is out. Restart to update";
class Required : public std::runtime_error {
 public:
  Required() : std::runtime_error(kMessage) {}
};
inline std::string version_line() { return "Melee Unlocked " MELEE_PORT_VERSION; }

// Compare arbitrarily large dotted numbers without overflow; missing components are zero.
inline bool dotted(const std::string& v) {
  if (v.empty() || v.front() == '.' || v.back() == '.') return false;
  char prev = 0;
  for (char c : v) {
    if ((c < '0' || c > '9') && c != '.') return false;
    if (c == '.' && prev == '.') return false;
    prev = c;
  }
  return true;
}
inline bool newer(const std::string& candidate, const std::string& current) {
  if (!dotted(candidate) || !dotted(current)) return false;
  size_t a = 0, b = 0;
  const auto part = [](const std::string& v, size_t& pos) {
    const auto end = v.find('.', pos);
    std::string n = pos < v.size() ? v.substr(pos, end == std::string::npos ? end : end - pos) : "";
    pos = end == std::string::npos ? v.size() : end + 1;
    n.erase(0, n.find_first_not_of('0'));
    return n;
  };
  while (a < candidate.size() || b < current.size()) {
    const auto x = part(candidate, a), y = part(current, b);
    if (x.size() != y.size()) return x.size() > y.size();
    if (x != y) return x > y;
  }
  return false;
}
inline bool required(const nlohmann::json& reply) {
  if (!reply.is_object()) return false;
  const auto code = reply.find("code");
  return code != reply.end() && code->is_string() && *code == "update_required";
}
inline const nlohmann::json& refusal(const nlohmann::json& reply) {
  if (required(reply)) return reply;
  const auto result = reply.is_object() ? reply.find("result") : reply.end();
  return result != reply.end() && required(*result) ? *result : reply;
}
inline std::string current(const nlohmann::json& reply) {
  const auto value = reply.is_object() ? reply.find("current") : reply.end();
  // No server-supplied control bytes or unbounded text in a log line. Preserve release suffixes.
  if (value == reply.end() || !value->is_string()) return "unknown";
  const auto v = value->get<std::string>();
  if (v.empty() || v.size() > 64) return "unknown";
  for (char c : v)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          c == '.' || c == '-' || c == '_')) return "unknown";
  return v;
}
// Only positively identified idle scenes may hand back. Unknown scenes stay conservative.
inline bool idle_scene(uint32_t major, uint32_t minor) {
  // GameModeKind (melee/gm/forward.h): 0x00 GM_TITLE, 0x01 GM_MENU (main and Online menus).
  return major == 0x00 || major == 0x01 ||
         (major == 0x08 && minor == 0) || // idle online CSS / Ranked result reveal
         ((major == 0x02 || major == 0x0F || (major >= 0x10 && major <= 0x13) ||
           major == 0x1B || major == 0x1C || (major >= 0x1D && major <= 0x26)) && minor == 0);
}
struct SafePoint {
  bool idle_scene = false;
  bool online_busy = true;
  bool room_or_link = true;
  bool ranked_set = true;
  bool friends_room = true;
  bool online_screen = false;  // not a safety condition: the accept line says where to resume
};
inline bool safe(const SafePoint& s) {
  return s.idle_scene && !s.online_busy && !s.room_or_link && !s.ranked_set && !s.friends_room;
}
inline bool build_id(const std::string& id) {
  if (id.size() != 36) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
    else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  }
  return true;
}
enum class Resume { Absent, Online, Unknown };
inline Resume resume(const char* value) {
  return !value ? Resume::Absent : std::string(value) == "online" ? Resume::Online : Resume::Unknown;
}
class Notice {
 public:
  bool command(const std::string& line, const std::string& build = MELEE_PORT_VERSION) {
    // Consume malformed ready commands too. Never let app protocol text reach the window adapter.
    constexpr const char* ready = "embed update_ready";
    if (line.rfind(ready, 0) == 0) {
      const auto id = line.substr(std::char_traits<char>::length(ready));
      if (id.size() == 37 && id.front() == ' ' && build_id(id.substr(1))) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepted_) pending_ = ready_log_ = id.substr(1);
      }
      return true;
    }
    constexpr const char* prefix = "embed update_available ";
    if (line == "embed update_available") return true;
    if (line.rfind(prefix, 0) != 0) return false;
    if (newer(line.substr(std::char_traits<char>::length(prefix)), build)) available_ = true;
    return true;  // Consume stale/malformed notices too; never forward them to the window adapter.
  }
  void refuse() { available_ = true; }
  bool available() const { return available_.load(); }
  std::string take_ready_log() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out; out.swap(ready_log_); return out;
  }
  std::string pending() const { std::lock_guard<std::mutex> lock(mutex_); return pending_; }
  bool begin(const SafePoint& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handing_back_ || accepted_ || pending_.empty() || !safe(state)) return false;
    handing_back_ = true;
    online_ = state.online_screen;
    return true;
  }
  bool visible() const { std::lock_guard<std::mutex> lock(mutex_); return handing_back_; }
  // Presentation time, not simulation/retrace time: keep the notice on screen for a real second.
  std::string presented(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!handing_back_ || accepted_) return {};
    if (!presenting_) { presenting_ = true; since_ = now; return {}; }
    if (now < since_ || now - since_ < 1000) return {};
    accepted_ = true;
    // " online": the app relaunches with --resume online (native-experience 11-01).
    const auto line = "yougame: update accept " + pending_ + (online_ ? " online" : "");
    pending_.clear();
    return line; // The caller writes one newline using the bridge stdout writer.
  }
 private:
  mutable std::mutex mutex_;
  std::string pending_, ready_log_;
  bool handing_back_ = false, presenting_ = false, accepted_ = false, online_ = false;
  uint64_t since_ = 0;
  std::atomic<bool> available_{false};
};
}  // namespace yougame::update
