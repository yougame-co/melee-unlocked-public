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
inline void configure(ImGuiIO& io) {
  navigation(io, false);
  // Preserve press/release pairs between rendered frames, including the mouse click that
  // returns from pad navigation. Platform gamepad polling is disabled; only our routed pad
  // submits gamepad keys, so opposing producers cannot build up a trickled event backlog.
  io.ConfigInputTrickleEventQueue = true;
}
}  // namespace gx::settings_input
