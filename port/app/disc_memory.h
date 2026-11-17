// Where a double-clicked Melee Unlocked finds the disc an earlier run already used.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Every run started with --iso records that disc as "iso=<path>" in %LOCALAPPDATA%\MeleeUnlocked\
// launcher.ini (main.cpp remember_iso), and a double-click records its own pick in
// %LOCALAPPDATA%\MeleeUnlocked-YouGame\standalone.ini. A run the YouGame app starts on Windows is
// in an AppContainer, where Windows points LOCALAPPDATA at %LOCALAPPDATA%\Packages\<profile>\AC, so
// its launcher.ini lands there (the profiles are named YouGame.Game.<hash>, YouGame.Staging.Game.<hash>
// and so on: desktop/src/sandbox-policy.ts). A double-click outside the app could not see that copy,
// so it asked for a disc the player had already set up in the app (found in a test walk, 11-17).
// Only paths are read here, all written by Melee Unlocked itself. launcher.ini (and so each
// AppContainer copy) records the disc of every --iso run, mod runs included, so a disc found there
// is only taken when the whole file is the unmodified NTSC 1.02 disc (the caller's `unmodified`,
// mod_scan's whole-disc check): a 20XX or Hack Pack ISO has the same GALE01 rev 2 header. The started
// game would refuse such a disc (main.cpp, --vanilla-only), but a double-click has no window to say
// so, and the bare launch would then remember it. standalone.ini holds only discs that passed.
// Portable on purpose (no Windows calls), so the host tests run anywhere.
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace disc_memory {
namespace fs = std::filesystem;

// The last "iso=" line of an ini's text, as written (bytes, line ending removed); "" when none.
inline std::string iso_line(const std::string& text) {
  std::string found;
  size_t at = 0;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("iso=", 0) == 0) found = line.substr(4);
    at = end + 1;
  }
  return found;
}

inline std::string read_iso(const fs::path& ini) {
  std::ifstream f(ini, std::ios::binary);
  if (!f) return {};
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return iso_line(text);
}

// standalone.ini is written as UTF-8; launcher.ini in the system code page (GetFullPathNameA).
enum class Encoding { Utf8, Ansi };
// full_check: the disc may be from a mod run, so the header is not enough (see above).
struct Place { fs::path ini; Encoding encoding; bool full_check = true; };

inline bool yougame_profile(const std::string& name) {
  static const char prefix[] = "yougame.";
  if (name.size() <= sizeof prefix - 1) return false;
  for (size_t i = 0; i < sizeof prefix - 1; ++i)
    if (std::tolower((unsigned char)name[i]) != prefix[i]) return false;
  return true;
}

// The files to look in, best first: the double-click's own pick, then the last run outside the
// app, then the YouGame app's runs, the most recently written first.
inline std::vector<Place> places(const fs::path& local_app_data) {
  std::vector<Place> out;
  if (local_app_data.empty()) return out;
  out.push_back({local_app_data / "MeleeUnlocked-YouGame" / "standalone.ini", Encoding::Utf8, false});
  out.push_back({local_app_data / "MeleeUnlocked" / "launcher.ini", Encoding::Ansi, true});
  std::vector<std::pair<fs::file_time_type, fs::path>> app_runs;
  std::error_code ec;
  fs::directory_iterator it(local_app_data / "Packages", ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (!yougame_profile(it->path().filename().u8string())) continue;
    const fs::path ini = it->path() / "AC" / "MeleeUnlocked" / "launcher.ini";
    std::error_code fe;
    if (!fs::is_regular_file(ini, fe)) continue;
    const auto when = fs::last_write_time(ini, fe);
    if (!fe) app_runs.emplace_back(when, ini);
  }
  std::stable_sort(app_runs.begin(), app_runs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& run : app_runs) out.push_back({run.second, Encoding::Ansi, true});
  return out;
}

// Records the disc a double-click used in standalone.ini (UTF-8), through a temporary file.
inline void remember(const fs::path& ini, const fs::path& disc) {
  const fs::path tmp = fs::path(ini).concat(".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "iso=" << disc.u8string() << "\n";
    if (!f) return;
  }
  std::error_code ec;
  fs::rename(tmp, ini, ec);
}

// The first remembered disc that passes `header` (cheap: a Melee NTSC 1.02 header) and, from a
// place with full_check, `unmodified` (reads the whole disc), or an empty path. `decode` turns an
// ini's bytes into a path in that ini's encoding.
template <class Decode, class Header, class Unmodified>
fs::path first_verified(const std::vector<Place>& from, Decode decode, Header header, Unmodified unmodified) {
  for (const Place& place : from) {
    const std::string raw = read_iso(place.ini);
    if (raw.empty()) continue;
    const fs::path disc = decode(raw, place.encoding);
    if (disc.empty() || !header(disc)) continue;
    if (place.full_check && !unmodified(disc)) continue;
    return disc;
  }
  return {};
}
}  // namespace disc_memory
