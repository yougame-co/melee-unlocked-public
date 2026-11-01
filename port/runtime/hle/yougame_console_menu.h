// The presentation mirrors the engine's lockstep condition; no UI-owned network mode.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
namespace yougame::console {
inline constexpr const char* kMenuHint = "Menus are delay-based for now. Matches use rollback.";
inline bool menu_select(uint32_t major, uint32_t minor) {
  // Normal local VS menus; console sessions boot the retail game, not the Slippi CSS.
  return (major == 2 || (major >= 0x10 && major <= 0x13) || major == 0x1b || major == 0x1c) && minor <= 1;
}
inline bool honest_menu(bool lockstep, bool rollback, size_t players, uint32_t major, uint32_t minor) {
  return lockstep && rollback && players == 2 && menu_select(major,minor);
}
// UI-thread only. First CSS entry in this process fades in and pulses twice in 1.5 seconds.
class MenuHintAnimation {
 public:
  float alpha(bool visible, double now) {
    if (!visible) return 0;
    if (first_ < 0) first_ = now;
    const double elapsed = std::max(0.0,now-first_);
    if (elapsed >= 1.5) return 1;
    const double fade = std::min(1.0,elapsed/0.25);
    const double pulse = 0.75 + 0.25*std::cos(4*3.141592653589793*elapsed/1.5);
    return float(fade*pulse);
  }
 private:
  double first_ = -1;
};
bool console_menu_visible(); // atomic publication from the simulation thread
#ifdef YOUGAME_AGENT_TEST_HOOK
uint32_t console_css_frames(); // consecutive session frames on VS character select (dropRoomOnce)
#endif
} // namespace yougame::console
