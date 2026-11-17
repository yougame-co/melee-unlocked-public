// The settings search's matching (pc_settings_visuals.inl): forgiving, so a setting is found the way
// people type. Every word of the query must match some word of the setting's name (or its page), by
// any of: a part of the word ("hit" in "hitbox"); a typo of it (a letter wrong, missing, extra or two
// swapped: one for words up to six letters, two for longer); its letters in order inside one word
// ("hbx" in "hitbox", three letters or more); or another word for the same thing ("sound" for
// "audio", "colour" for "color", "lag" for "delay"). Pure: the unit test runs it without a window.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace settings_search {

inline std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> out;
  std::string w;
  for (size_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    if (c == '#' && i + 1 < text.size() && text[i + 1] == '#') break;   // an ImGui id suffix
    if (std::isalnum((unsigned char)c)) { w += (char)std::tolower((unsigned char)c); continue; }
    if (!w.empty()) { out.push_back(w); w.clear(); }
  }
  return out;
}

// Optimal string alignment distance (a swap of two neighbours counts once), stopping past `limit`.
inline int distance(const std::string& a, const std::string& b, int limit) {
  const int n = (int)a.size(), m = (int)b.size();
  if (std::abs(n - m) > limit) return limit + 1;
  std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1));
  for (int i = 0; i <= n; ++i) d[i][0] = i;
  for (int j = 0; j <= m; ++j) d[0][j] = j;
  for (int i = 1; i <= n; ++i) {
    int best = limit + 1;
    for (int j = 1; j <= m; ++j) {
      const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
      best = std::min(best, d[i][j]);
    }
    if (best > limit) return limit + 1;
  }
  return d[n][m];
}

inline bool in_order(const std::string& q, const std::string& w) {
  size_t k = 0;
  for (char c : w) if (k < q.size() && c == q[k]) ++k;
  return k == q.size();
}

// Other words people use for the same thing, each group matching any of its members.
inline const std::vector<std::vector<const char*>>& synonyms() {
  static const std::vector<std::vector<const char*>> s = {
      {"sound", "audio", "volume", "music", "sfx", "mute"},
      {"colour", "color", "colours", "colors", "tint"},
      {"lag", "delay", "latency", "input"},
      {"fps", "framerate", "frame", "frames", "cap", "vsync"},
      {"res", "resolution", "scale", "scaling", "upscale", "internal"},
      {"pad", "controller", "gamepad", "joystick", "adapter"},
      {"skin", "skins", "costume", "costumes", "outfit"},
      {"ui", "menu", "menus", "interface", "hud", "overlay", "overlays"},
      {"cam", "camera", "view"},
      {"hitbox", "hitboxes", "hurtbox", "hurtboxes", "boxes"},
      {"graphics", "video", "display", "picture", "visual", "visuals"},
      {"online", "netplay", "slippi", "internet", "multiplayer"},
      {"fullscreen", "window", "windowed", "borderless"},
      {"rtx", "raytracing", "ray", "tracing", "pathtracing"},
      {"code", "codes", "gecko", "cheat", "cheats"},
      {"mod", "mods", "pack", "packs", "disc", "iso"},
  };
  return s;
}

inline bool word_matches(const std::string& q, const std::string& w) {
  if (q.empty()) return true;
  if (w.find(q) != std::string::npos) return true;
  if (q.size() >= 3 && q[0] == w[0] && in_order(q, w)) return true;
  if (q.size() >= 4) {
    const int limit = q.size() <= 6 ? 1 : 2;
    if (distance(q, w, limit) <= limit) return true;
    // a typo in the first letters of a longer word ("hitbx" for "hitboxes")
    if (w.size() > q.size() && distance(q, w.substr(0, q.size()), limit) <= limit) return true;
  }
  return false;
}

inline bool matches(const std::string& query, const std::string& name) {
  const std::vector<std::string> q = words(query), n = words(name);
  if (q.empty()) return false;
  for (const std::string& qw : q) {
    bool found = std::any_of(n.begin(), n.end(), [&](const std::string& w) { return word_matches(qw, w); });
    if (!found)
      for (const auto& group : synonyms()) {
        const bool in_group = std::any_of(group.begin(), group.end(), [&](const char* s) { return qw == s || (qw.size() >= 4 && distance(qw, s, 1) <= 1); });
        if (!in_group) continue;
        for (const char* s : group)
          if (std::any_of(n.begin(), n.end(), [&](const std::string& w) { return w == s || (std::string(s).size() >= 4 && w.rfind(s, 0) == 0); })) found = true;
        if (found) break;
      }
    if (!found) return false;
  }
  return true;
}

}  // namespace settings_search
