// Edge detection and input suppression for the in-game settings shortcut.
#pragma once
#include <cstdint>

namespace host::settings_shortcut {

// The chord is Start + D-pad Down + Z: three buttons nobody presses together in play, so it never
// fires by accident (Z + Start alone did, and it also sat on the input overlay's Z + Start test).
inline bool poll_chord(bool connected, uint16_t& buttons, bool& was_down, uint16_t chord) {
  const bool down = connected && (buttons & chord) == chord;
  const bool pressed = down && !was_down;
  // Latched until every chord button is up: releasing Down and Z before Start used to hand the
  // UI a lone Start press, and Start closes the panel, so the menu shut the moment it opened.
  if (pressed) was_down = true;
  else if (was_down && (!connected || (buttons & chord) == 0)) was_down = false;
  if (connected && was_down) buttons &= static_cast<uint16_t>(~chord);
  return pressed;
}
inline bool poll_z_start(bool connected, uint16_t& buttons, bool& was_down) {
  return poll_chord(connected, buttons, was_down, 0x1000u | 0x0004u | 0x0010u);   // Start + D-pad Down + Z
}
// The Practice panel's chord, the settings one with D-pad Up: a controller alone opens and closes it.
inline bool poll_practice(bool connected, uint16_t& buttons, bool& was_down) {
  return poll_chord(connected, buttons, was_down, 0x1000u | 0x0008u | 0x0010u);   // Start + D-pad Up + Z
}

// Training reads a lone Start as its own pause menu. Hands rarely press three buttons on the same
// frame, so Start often landed a frame before D-pad Up + Z: the pause menu opened under the Practice
// panel with its cursor on Speed, and the first stick push after the panel closed set Speed to 2/3.
// In Training a lone Start now waits up to kStartWait polls for the rest of a chord. A Start that is
// still alone after that reaches the game, and a tap shorter than the wait is handed over on release.
// `taken`: a chord (or the overlay) has this Start, so it never goes to the game.
constexpr int kStartWait = 3;
inline void hold_lone_start(bool active, bool taken, uint16_t raw, uint16_t& buttons, int& waited) {
  const bool start = (raw & 0x1000u) != 0;
  if (!active || taken) {
    waited = start ? kStartWait + 1 : 0;
    return;
  }
  if (start) {
    if (waited < kStartWait) {
      buttons &= static_cast<uint16_t>(~0x1000u);
      ++waited;
    } else {
      waited = kStartWait + 1;   // waited long enough: it is the game's
    }
  } else {
    if (waited > 0 && waited <= kStartWait) buttons |= 0x1000u;   // a short tap still counts
    waited = 0;
  }
}

}  // namespace host::settings_shortcut
