// Offline update policy: an app notice is advisory; only an explicit server code refuses a search.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <atomic>
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
class Notice {
 public:
  bool command(const std::string& line, const std::string& build = MELEE_PORT_VERSION) {
    constexpr const char* prefix = "embed update_available ";
    if (line == "embed update_available") return true;
    if (line.rfind(prefix, 0) != 0) return false;
    if (newer(line.substr(std::char_traits<char>::length(prefix)), build)) available_ = true;
    return true;  // Consume stale/malformed notices too; never forward them to the window adapter.
  }
  void refuse() { available_ = true; }
  bool available() const { return available_.load(); }
 private:
  std::atomic<bool> available_{false};
};
}  // namespace yougame::update
