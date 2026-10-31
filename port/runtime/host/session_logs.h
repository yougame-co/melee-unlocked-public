// Session log retention, shared by both hosts and window-free tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <string>
namespace host {
inline std::string previous_log(const std::string& path, int age) {
  const bool log = path.size() > 4 && path.compare(path.size() - 4, 4, ".log") == 0;
  const auto stem = log ? path.substr(0, path.size() - 4) : path;
  return stem + ".prev" + (age == 1 ? "" : "." + std::to_string(age)) + (log ? ".log" : "");
}
// False means the current file could not be moved: callers must append to it.
inline bool rotate_session_logs(const std::string& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) return !error;
  std::remove(previous_log(path, 4).c_str());
  for (int age = 3; age >= 1; --age)
    std::rename(previous_log(path, age).c_str(), previous_log(path, age + 1).c_str());
  if (std::rename(path.c_str(), previous_log(path, 1).c_str()) == 0) return true;
  // A locked retention target must never cost us the just-ended session. Keep it
  // beside the current log under a unique name, outside the ordinary age chain.
  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto fallback = path + ".saved." + std::to_string(stamp) + "." + std::to_string(attempt);
    if (std::filesystem::exists(fallback, error) || error) continue;
    if (std::rename(path.c_str(), fallback.c_str()) == 0) return true;
  }
  return false;
}
} // namespace host
