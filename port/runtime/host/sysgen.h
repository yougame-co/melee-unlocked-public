// Rebuild disc-derived Slippi assets in writable user data, before either engine boots.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace host::sysgen {
struct Source { std::string disc, sha1; size_t size = 0; };
struct File {
  std::string target, sha1, delta;
  size_t size = 0;
  std::vector<std::string> sys;
  std::vector<Source> sources;
};
bool parse_manifest(const std::string& json, std::vector<File>& files, std::string& error);
std::string sha1(const std::vector<uint8_t>& bytes);
struct Config {
  std::filesystem::path build_dir, sys_dir, data_dir;
};
// The player's own folder for generated Sys files, used when the command line names none outside the
// game folder. Windows: %LOCALAPPDATA%\MeleeUnlocked-YouGame\User (the folder a double-click and the
// launcher use). Linux: $XDG_DATA_HOME/MeleeUnlocked-YouGame/User, else ~/.local/share/... Mac: the
// bundle's own profile folder, $XDG_DATA_HOME/melee-unlocked/User, else ~/.local/share/... (full_launch.h).
// Empty when the system names no such folder.
std::filesystem::path user_data_dir();
// Config::data_dir from the command line. An explicit --sysgen-data-dir is used as given, so prepare still
// refuses one inside build_dir. Otherwise --user-dir's parent, unless there is no --user-dir or that parent
// is inside build_dir (a zip that ships User/Slippi beside the program): then `fallback`, with one log
// line in `note`. An empty fallback keeps the old answer, and prepare reports it.
std::filesystem::path choose_data_dir(const std::filesystem::path& build_dir, const std::string& user_dir,
                                      const std::string& data_dir, const std::filesystem::path& fallback,
                                      std::string& note);
// Boot supplies either the active disc reader or an independent retail base-image reader.
// Size is checked before allocation/read.
struct Disc {
  std::function<bool()> verify_retail;
  std::function<bool(const std::string&, size_t, std::vector<uint8_t>&)> read;
};
class Assets {
 public:
  bool prepare(const Config& config, const Disc& disc, std::string& error);
  const std::string& status_message() const { return status_message_; }
  std::filesystem::path lookup(const std::filesystem::path& sys_dir, const std::string& target) const;
  std::vector<std::string> generated_names(const std::filesystem::path& sys_dir) const;
 private:
  std::string status_message_;
  std::map<std::string, std::filesystem::path> redirects_;
};
}  // namespace host::sysgen
