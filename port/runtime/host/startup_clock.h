// Startup timing and the log's first line. Host-only text: nothing here reaches game state.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>

// The full YouGame build (e.g. 0.9.120-online-test.103), from the YOUGAME_BUILD file at the root
// (port/CMakeLists.txt). Builds that do not set it (Linux, Mac, bare test targets) log no build.
#ifndef MELEE_YOUGAME_BUILD
#define MELEE_YOUGAME_BUILD ""
#endif

namespace host::startup {
// " (+1234 ms)": milliseconds since this process was created, appended after a startup line's
// existing wording, so parsers that match a line's start or its words still match it. The YouGame
// app counts its caps deadline from launch; these say which phase a slow start spent its time in.
inline std::string suffix(uint64_t ms) { return " (+" + std::to_string(ms) + " ms)"; }

// The log's first line. Crash reports read "Melee Unlocked <version>," (crash_markdown.js), so the
// upstream version and the comma stay first; the YouGame build goes at the end when there is one.
inline std::string log_header(const char* version, const char* exe, const char* yougame_build) {
  std::string out = std::string("Melee Unlocked ") + (version ? version : "") + ", " + (exe ? exe : "");
  if (yougame_build && *yougame_build) out += std::string(", YouGame build ") + yougame_build;
  return out;
}
}  // namespace host::startup
