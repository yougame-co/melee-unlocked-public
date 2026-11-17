// Our embedded-panel adapter around the unchanged vendored SDL backend.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef __APPLE__
#include "embed_mac.h"
#include "embed_mac_pixels.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl2.h"
#include <SDL.h>
#include <cfloat>

namespace mac_embed {
inline ImVec2 imgui_retina_input_scale(SDL_Window* window, float ui) {
  int w = 0, h = 0, pw = 0, ph = 0;
  if (!drawable_size(window, &w, &h)) return ImVec2(0, 0);
  SDL_GetWindowSize(window, &pw, &ph);
  double x = 0, y = 0;
  picture_input(window, &x, &y, &pw, &ph);
  const auto logical = ui_size(w, h, ui);
  return ImVec2(float(point_to_ui(1, w, pw, logical.framebuffer_x)),
                float(point_to_ui(1, h, ph, logical.framebuffer_y)));
}
// Only transform newly queued positions, before ImGui::NewFrame consumes them.
// This covers motion/touch, leave sentinels, and the backend's global-state fallback
// without relying on private SDL backend data or rounding positions to integer UI units.
inline void imgui_retina_map_events(int first, ImVec2 scale, SDL_Window* window, ImVec2 origin) {
  auto& events = ImGui::GetCurrentContext()->InputEventsQueue;
  for (int i = first; i < events.Size; ++i) {
    auto& e = events[i];
    if (e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX > -FLT_MAX && e.MousePos.PosY > -FLT_MAX) {
      if (!picture_hit(window, e.MousePos.PosX, e.MousePos.PosY)) {
        e.MousePos.PosX = e.MousePos.PosY = -FLT_MAX;
      } else {
        e.MousePos.PosX = (e.MousePos.PosX - origin.x) * scale.x;
        e.MousePos.PosY = (e.MousePos.PosY - origin.y) * scale.y;
      }
    }
  }
}
// The stock backend deduplicates against the last queued position (or io.MousePos).
// Temporarily expose that reference in points too, otherwise e.g. logical x=100
// could suppress a new point x=100 even though its mapped position is x=200.
struct ImGuiRetinaInput {
  ImVec2 scale, mouse, queued, origin;
  SDL_Window* window;
  int first, previous = -1;
  ImGuiRetinaInput(ImVec2 factor, SDL_Window* target) : scale(factor), mouse(ImGui::GetIO().MousePos), window(target),
      first(ImGui::GetCurrentContext()->InputEventsQueue.Size) {
    double x = 0, y = 0; int w = 0, h = 0;
    picture_input(window, &x, &y, &w, &h);
    origin = ImVec2(float(x), float(y));
    auto& events = ImGui::GetCurrentContext()->InputEventsQueue;
    for (int i = first - 1; i >= 0; --i) if (events[i].Type == ImGuiInputEventType_MousePos) {
      previous = i;
      auto& pos = events[i].MousePos;
      queued = ImVec2(pos.PosX, pos.PosY);
      if (pos.PosX > -FLT_MAX) pos.PosX = pos.PosX / scale.x + origin.x;
      if (pos.PosY > -FLT_MAX) pos.PosY = pos.PosY / scale.y + origin.y;
      break;
    }
    auto& pos = ImGui::GetIO().MousePos;
    if (pos.x > -FLT_MAX) pos.x = pos.x / scale.x + origin.x;
    if (pos.y > -FLT_MAX) pos.y = pos.y / scale.y + origin.y;
  }
  ~ImGuiRetinaInput() {
    if (previous >= 0) {
      auto& pos = ImGui::GetCurrentContext()->InputEventsQueue[previous].MousePos;
      pos.PosX = queued.x; pos.PosY = queued.y;
    }
    ImGui::GetIO().MousePos = mouse;
    imgui_retina_map_events(first, scale, window, origin);
  }
};
// Returns false for standalone windows. Embedded 1x needs the same clip/origin mapping as Retina.
inline bool imgui_retina_process_event(SDL_Window* window, const SDL_Event& event, float ui) {
  const auto scale = imgui_retina_input_scale(window, ui);
  if (scale.x <= 0) return false;
  const ImGuiRetinaInput input(scale, window);
  // Captured drags can leave the panel. Preserve button-up to release held state, but reject new clicks outside it.
  if (event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == SDL_GetWindowID(window) &&
      !picture_hit(window, event.button.x, event.button.y)) return true;
  if (event.type == SDL_MOUSEWHEEL && event.wheel.windowID == SDL_GetWindowID(window) &&
      !picture_hit(window, event.wheel.mouseX, event.wheel.mouseY)) return true;
  if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) &&
      event.button.windowID == SDL_GetWindowID(window)) {
    auto& io = ImGui::GetIO();
    io.AddMouseSourceEvent(event.button.which == SDL_TOUCH_MOUSEID ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
    io.AddMousePosEvent(float(event.button.x), float(event.button.y));
  }
  ImGui_ImplSDL2_ProcessEvent(&event);
  return true;
}
inline void imgui_retina_new_frame(SDL_Window* window, float ui) {
  const auto scale = imgui_retina_input_scale(window, ui);
  if (scale.x <= 0) { ImGui_ImplSDL2_NewFrame(); return; }
  // The scope also supplies point coordinates for the stock pointer warp.
  { const ImGuiRetinaInput input(scale, window); ImGui_ImplSDL2_NewFrame(); }
  int w = 0, h = 0;
  if (drawable_size(window, &w, &h)) ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));
}
inline void imgui_retina_display_size(SDL_Window* window, float ui) {
  auto& io = ImGui::GetIO();
  int w = int(io.DisplaySize.x), h = int(io.DisplaySize.y);
  const bool retina = drawable_size(window, &w, &h);
  const auto logical = display_size(retina, w, h, ui);
  io.DisplaySize = ImVec2(float(logical.w), float(logical.h));
  io.DisplayFramebufferScale = ImVec2(logical.framebuffer_x, logical.framebuffer_y);
}
} // namespace mac_embed
#endif
