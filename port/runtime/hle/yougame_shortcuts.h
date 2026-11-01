// App-owned keyboard shortcuts and window-free stdin hand-off.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_parity.h"
#include <array>
#include <string>
#include <string_view>

namespace yougame::shortcuts {
enum : unsigned { Shift = 1, Ctrl = 2, Alt = 4 };
// Virtual-key values are stable protocol-independent integers; no Windows headers here.
struct Combo {
  unsigned modifiers = 0;
  int key = 0;
  bool operator==(const Combo& other) const { return modifiers == other.modifiers && key == other.key; }
  bool held(unsigned mods) const { return key && (mods & modifiers) == modifiers; }
};
struct Config {
  Combo menu{Shift, 9}, chat{};
  bool operator==(const Config& other) const { return menu == other.menu && chat == other.chat; }
};
inline std::string lower(std::string_view input) {
  std::string result(input);
  for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return result;
}
inline bool parse_combo(std::string_view input, Combo& output) {
  const std::string value = lower(input);
  Combo next;
  size_t start = 0, plus;
  while ((plus = value.find('+', start)) != std::string::npos) {
    const auto part = value.substr(start, plus - start);
    const unsigned modifier = part == "shift" ? Shift : part == "ctrl" ? Ctrl : part == "alt" ? Alt : 0;
    if (!modifier || (next.modifiers & modifier)) return false;
    next.modifiers |= modifier;
    start = plus + 1;
  }
  if (!next.modifiers) return false;
  const auto key = value.substr(start);
  if (key == "tab") next.key = 9;
  else if (key == "enter") next.key = 13;
  else if (key == "backquote") next.key = 192;
  else if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') next.key = key[0] - 'a' + 'A';
  else {
    for (int f = 1; f <= 12; ++f) if (key == "f" + std::to_string(f)) next.key = 111 + f;
  }
  if (!next.key) return false;
  output = next;
  return true;
}
// A partial update keeps the omitted chord. Validate the entire line before publishing either.
inline bool parse(std::string_view command, const Config& previous, Config& output) {
  if (command.size() > 128 || command.substr(0, 10) != "shortcuts ") return false;
  Config next = previous;
  unsigned seen = 0;
  size_t start = 10;
  while (start < command.size()) {
    const size_t end = command.find(' ', start);
    const auto field = command.substr(start, end == std::string_view::npos ? end : end - start);
    const size_t equals = field.find('=');
    if (equals == std::string_view::npos) return false;
    const auto name = lower(field.substr(0, equals));
    const unsigned bit = name == "menu" ? 1 : name == "chat" ? 2 : 0;
    if (!bit || (seen & bit)) return false;
    seen |= bit;
    if (!parse_combo(field.substr(equals + 1), bit == 1 ? next.menu : next.chat)) return false;
    if (end == std::string_view::npos) break;
    start = end + 1;
    if (start == command.size()) return false;
  }
  if (!seen) return false;
  output = next;
  return true;
}

class Commands {
 public:
  // Called on the stdin reader, before the geometry backlog (including before a window exists).
  template<class Log> bool dispatch(const std::string& command, Log log) {
    if (command == "settings") { settings_.store(true); return true; }
    if (command.rfind("shortcuts", 0) != 0) return false;
    bool warn = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Config next;
      if (parse(command, config_, next)) config_ = next;
      else if (!warned_) { warned_ = true; warn = true; }
    }
    if (warn) log("embed: ignored malformed shortcuts command");
    return true;
  }
  Config snapshot() { std::lock_guard<std::mutex> lock(mutex_); return config_; }
  bool take_settings() { return settings_.exchange(false); }
 private:
  std::mutex mutex_;
  Config config_;
  bool warned_ = false;
  std::atomic<bool> settings_{false};
};
inline Commands commands;

inline constexpr const char* kChatDown = "yg {\"chat\":\"down\"}";
inline constexpr const char* kChatUp = "yg {\"chat\":\"up\"}";
inline constexpr const char* kChatCancel = "yg {\"chat\":\"cancel\"}";
inline const char* chat_line(const char* line) {
  return line == parity::kDown ? kChatDown : line == parity::kUp ? kChatUp : line ? kChatCancel : nullptr;
}
class Keys {
 public:
  struct Result { bool swallowed = false; std::array<const char*, 2> lines{}; };
  // The caller serializes configure/event/swallow with its keyboard mutex.
  Result configure(const Config& config) {
    Result result;
    if (!(config.menu == config_.menu)) { result.lines[0] = menu_.cancel(); menu_ = {}; }
    if (!(config.chat == config_.chat)) { result.lines[1] = chat_line(chat_.cancel()); chat_ = {}; }
    config_ = config;
    return result;
  }
  Result event(bool stdio, int vk, bool down, unsigned mods, bool repeat) {
    Result result;
    if (!stdio || vk < 0 || vk >= 256) return result;
    if (down && !repeat) reserved_[vk] = false; // A fresh edge recovers a key-up lost with focus.
    const Combo combos[] = {config_.menu, config_.chat};
    parity::OverlayKey* keys[] = {&menu_, &chat_};
    const unsigned released = down ? 0 : (vk == 16 || vk == 160 || vk == 161) ? Shift :
        (vk == 17 || vk == 162 || vk == 163) ? Ctrl : (vk == 18 || vk == 164 || vk == 165) ? Alt : 0;
    for (int i = 0; i < 2; ++i) {
      const auto combo = combos[i];
      const bool held = combo.held(mods);
      const char* line = held && !(released & combo.modifiers) ? nullptr : keys[i]->modifiers_released();
      if (combo.key && vk == combo.key) {
        const auto event = keys[i]->tab(true, down, held, repeat);
        if (event.line) line = event.line;
        result.swallowed |= event.swallowed;
      }
      result.lines[i] = i == 1 ? chat_line(line) : line;
    }
    result.swallowed |= reserved_[vk];
    reserved_[vk] = down && result.swallowed;
    return result;
  }
  // Before ImGui: releases always clear the window cache, including system releases.
  // Keep swallowed ordinary presses physically down; readers filter their reservation.
  // Unswallowed ordinary presses are still cached by the window after ImGui, and
  // system presses retain the window's existing Alt+Enter/default handling.
  Result window_event(bool (&physical)[256], bool stdio, int vk, bool down,
                      bool system, unsigned mods, bool repeat) {
    const auto result = event(stdio, vk, down, mods, repeat);
    if (vk >= 0 && vk < 256 && (!down || (result.swallowed && !system))) physical[vk] = down;
    return result;
  }
  bool swallows(bool stdio, int vk) const {
    return stdio && vk >= 0 && vk < 256 && reserved_[vk];
  }
  Result cancel() { return {false, {menu_.cancel(), chat_line(chat_.cancel())}}; }
  parity::OverlayKey& menu() { return menu_; }  // Existing scripted overlay taps share the real hold.
 private:
  Config config_;
  parity::OverlayKey menu_, chat_;
  bool reserved_[256]{};  // Survives modifier-first release, cancellation and remapping.
};
}  // namespace yougame::shortcuts
