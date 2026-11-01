// Shared, read-only account discovery for the launcher and both game engines.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace slippi::account {
struct Profile {
  std::filesystem::path file;
  nlohmann::json data;
  explicit operator bool() const { return !file.empty(); }
};

inline std::string text(const nlohmann::json& data, const char* key) {
  if (!data.is_object()) return {};
  const auto value = data.find(key);
  return value != data.end() && value->is_string() ? value->get<std::string>() : std::string();
}

inline Profile read(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) return {};
  try {
    auto data = nlohmann::json::parse(input, nullptr, false);
    if (text(data, "uid").empty() || text(data, "playKey").empty() ||
        text(data, "connectCode").empty()) return {};
    return {file, std::move(data)};
  } catch (const nlohmann::json::exception&) {
    return {};
  }
}

inline Profile search(const std::filesystem::path& root, int depth, size_t& remaining) {
  if (depth < 0 || remaining == 0) return {};
  --remaining;
  if (auto profile = read(root / "User" / "Slippi" / "user.json")) return profile;
  // On Linux and macOS the Slippi user folder is itself the data root.
  // Its account file has no additional User folder above it.
  if (auto profile = read(root / "Slippi" / "user.json")) return profile;
  std::error_code ec;
  std::vector<std::filesystem::path> directories;
  std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
  for (std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
    std::error_code kind;
    if (!it->is_symlink(kind) && !kind && it->is_directory(kind) && !kind)
      directories.push_back(it->path());
  }
  std::sort(directories.begin(), directories.end());
  for (const auto& directory : directories)
    if (auto profile = search(directory, depth - 1, remaining)) return profile;
  return {};
}

inline std::vector<std::filesystem::path> launcher_roots() {
  std::vector<std::filesystem::path> roots;
  for (const char* name : {"APPDATA", "LOCALAPPDATA"}) {
#ifdef _WIN32
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value && *value)
      roots.emplace_back(std::filesystem::path(value) / "Slippi Launcher");
    free(value);
#else
    if (const char* value = std::getenv(name))
      if (*value) roots.emplace_back(std::filesystem::path(value) / "Slippi Launcher");
#endif
  }
#ifndef _WIN32
  // Linux: the Slippi Launcher (an Electron app) keeps its folder under the XDG config home, and
  // the Flatpak build inside its own sandbox folder.
  if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
    roots.emplace_back(std::filesystem::path(config) / "Slippi Launcher");
  if (const char* home = std::getenv("HOME"); home && *home) {
    roots.emplace_back(std::filesystem::path(home) / ".config" / "Slippi Launcher");
    roots.emplace_back(std::filesystem::path(home) / ".var" / "app" / "com.project_slippi.Launcher" / "config" / "Slippi Launcher");
    // Slippi Dolphin on Linux keeps its user folder (Slippi/user.json) in its own XDG folders, not
    // under the launcher's: config and data homes, and the Flatpak's.
    roots.emplace_back(std::filesystem::path(home) / ".config" / "SlippiOnline");
    roots.emplace_back(std::filesystem::path(home) / ".local" / "share" / "SlippiOnline");
    roots.emplace_back(std::filesystem::path(home) / ".var" / "app" / "com.project_slippi.Launcher" / "data" / "SlippiOnline");
    // The newer Slippi Dolphin (the Launcher's current netplay build) names its folders SlippiDolphin.
    for (const char* kind : {".config", ".local/share"})
      roots.emplace_back(std::filesystem::path(home) / kind / "SlippiDolphin");
    roots.emplace_back(std::filesystem::path(home) / ".var" / "app" / "com.project_slippi.Launcher" / "config" / "SlippiDolphin");
    roots.emplace_back(std::filesystem::path(home) / ".var" / "app" / "com.project_slippi.Launcher" / "data" / "SlippiDolphin");
  }
  for (const char* name : {"XDG_DATA_HOME", "XDG_CONFIG_HOME"})
    if (const char* base = std::getenv(name); base && *base)
      for (const char* dolphin : {"SlippiOnline", "SlippiDolphin"}) roots.emplace_back(std::filesystem::path(base) / dolphin);
#endif
  return roots;
}

// The folders the Slippi Launcher's own settings file names for its Dolphin builds. They are the
// default netplay and playback folders unless the player moved them, and a moved one can be on
// another drive, outside anything searched from the launcher's folder.
inline std::vector<std::filesystem::path> settings_builds(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> builds;
  std::ifstream input(root / "Settings", std::ios::binary);
  if (!input) return builds;
  const auto data = nlohmann::json::parse(input, nullptr, false);
  if (!data.is_object()) return builds;
  const auto inner = data.find("settings");
  const nlohmann::json& settings = inner != data.end() && inner->is_object() ? *inner : data;
  for (const char* key : {"netplayDolphinPath", "playbackDolphinPath"})
    if (const auto value = text(settings, key); !value.empty()) builds.push_back(std::filesystem::u8path(value));
  return builds;
}

// The YouGame app names the Slippi sign-in file it found (YOUGAME_SLIPPI_USER_JSON): then exactly that
// file is read, never written, and no launcher folder is searched. Empty when unset.
inline std::filesystem::path app_sign_in() {
#ifdef _WIN32
  wchar_t* value = nullptr;
  size_t length = 0;
  std::filesystem::path file;
  if (_wdupenv_s(&value, &length, L"YOUGAME_SLIPPI_USER_JSON") == 0 && value && *value) file = value;
  free(value);
  return file;
#else
  const char* value = std::getenv("YOUGAME_SLIPPI_USER_JSON");
  return value && *value ? std::filesystem::path(value) : std::filesystem::path();
#endif
}

inline Profile resolve(const std::filesystem::path& local_user_dir,
                       const std::vector<std::filesystem::path>& roots = launcher_roots()) {
  if (const auto app = app_sign_in(); !app.empty()) return read(app);
  if (auto profile = read(local_user_dir / "user.json")) return profile;
  for (const auto& root : roots) {
    for (const char* build : {"netplay", "playback"})
      if (auto profile = read(root / build / "User" / "Slippi" / "user.json")) return profile;
    for (const auto& build : settings_builds(root))
      if (auto profile = read(build / "User" / "Slippi" / "user.json")) return profile;
    size_t remaining = 1024;
    if (auto profile = search(root, 4, remaining)) return profile;
  }
  return {};
}

// Why resolve() found nothing, so the launcher can say what to do instead of "log in" to a player
// who has: no Slippi Launcher folder at all, a launcher with no sign-in file, or a sign-in file
// without a connect code (an account that was created but never finished).
enum class Missing { Launcher, SignIn, ConnectCode };
// Where a sign-in file can be: the local folder, then each launcher folder that exists (`launcher`
// says whether one did).
inline std::vector<std::filesystem::path> sign_in_files(const std::filesystem::path& local_user_dir,
                                                        const std::vector<std::filesystem::path>& roots,
                                                        bool* launcher = nullptr) {
  std::error_code ec;
  std::vector<std::filesystem::path> files{local_user_dir / "user.json"};
  for (const auto& root : roots) {
    if (!std::filesystem::is_directory(root, ec)) continue;
    if (launcher) *launcher = true;
    for (const char* build : {"netplay", "netplay-beta", "playback"}) files.push_back(root / build / "User" / "Slippi" / "user.json");
    for (const auto& build : settings_builds(root)) files.push_back(build / "User" / "Slippi" / "user.json");
    files.push_back(root / "Slippi" / "user.json");
  }
  return files;
}
inline Missing why_missing(const std::filesystem::path& local_user_dir,
                           const std::vector<std::filesystem::path>& roots = launcher_roots()) {
  std::error_code ec;
  bool launcher = false;
  for (const auto& file : sign_in_files(local_user_dir, roots, &launcher))
    if (std::filesystem::is_regular_file(file, ec)) return Missing::ConnectCode;
  return launcher ? Missing::SignIn : Missing::Launcher;
}

// Why the YouGame app's Unranked cannot use a Slippi login, for a log line that names no path: no
// sign-in file, one that is not a whole account (unreadable, not JSON, no uid or play key), or an
// account that was never finished (no connect code). The first file found decides; the app's file
// (app_sign_in) is the only one when it names one.
enum class Unusable { FileMissing, Unreadable, NoConnectCode };
inline Unusable why_unusable_grant() {
  const auto file = app_sign_in();
  if (file.empty()) return Unusable::FileMissing;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return Unusable::FileMissing;
  std::ifstream input(file, std::ios::binary);
  if (!input) return Unusable::Unreadable;
  const auto data = nlohmann::json::parse(input, nullptr, false);
  if (text(data, "uid").empty() || text(data, "playKey").empty()) return Unusable::Unreadable;
  if (text(data, "connectCode").empty()) return Unusable::NoConnectCode;
  return Unusable::FileMissing; // As with why_unusable(), called only after login failed.
}
inline Unusable why_unusable(const std::filesystem::path& local_user_dir,
                             const std::vector<std::filesystem::path>& roots = launcher_roots()) {
  std::error_code ec;
  const auto app = app_sign_in();
  for (const auto& file : app.empty() ? sign_in_files(local_user_dir, roots) : std::vector<std::filesystem::path>{app}) {
    if (!std::filesystem::is_regular_file(file, ec)) continue;
    std::ifstream input(file, std::ios::binary);
    if (!input) return Unusable::Unreadable;
    const auto data = nlohmann::json::parse(input, nullptr, false);
    if (text(data, "uid").empty() || text(data, "playKey").empty()) return Unusable::Unreadable;
    if (text(data, "connectCode").empty()) return Unusable::NoConnectCode;
  }
  return Unusable::FileMissing;
}
inline const char* unusable_text(Unusable why) {
  return why == Unusable::FileMissing ? "file missing" : why == Unusable::Unreadable ? "unreadable" : "no connect code";
}
}  // namespace slippi::account
