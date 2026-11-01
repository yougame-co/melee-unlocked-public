// Shared by the settings overlay and its window-free input regression.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"

namespace gx::settings_input {
inline void navigation(ImGuiIO& io, bool open) {
  constexpr int flags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  if (open) io.ConfigFlags |= flags;
  else {
    io.ConfigFlags &= ~flags;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
  }
}
// The only routed GameCube-to-ImGui feed. Callers supply the navigation snapshot,
// retaining the unfiltered snapshot for port following, arming and gameplay consumers.
template <class IO, class Pad> inline void gamepad(IO& io, const Pad& pad) {
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.AddKeyEvent(ImGuiKey_GamepadStart, (pad.button & 0x1000) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadBack, (pad.button & 0x10) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (pad.button & 0x100) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (pad.button & 0x200) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (pad.button & 8) || pad.stick_y > 40);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (pad.button & 4) || pad.stick_y < -40);
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (pad.button & 1) || pad.stick_x < -40);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (pad.button & 2) || pad.stick_x > 40);
}
inline void configure(ImGuiIO& io) {
  navigation(io, false);
  // Preserve press/release pairs between rendered frames, including the mouse click that
  // returns from pad navigation. Platform gamepad polling is disabled; only our routed pad
  // submits gamepad keys, so opposing producers cannot build up a trickled event backlog.
  io.ConfigInputTrickleEventQueue = true;
}
}  // namespace gx::settings_input
