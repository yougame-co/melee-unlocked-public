// Mac full-host window adapter. No AppKit types leak into the shared host.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
struct SDL_Window;
namespace mac_embed {
// Before SDL video initialization. Enabled only for embedded mode and a valid CGWindowID.
bool configure(bool embedded);
SDL_Window* create(int w, int h, const char* title, bool allowed_visible);
bool active(); // false after parent-gone fallback; the SDL/native surface remains the same
// Startup-only port-settings switch. Environment opt-in is applied separately in create().
void set_retina(bool enabled);
bool retina_setting();
// Embedded picture snapshot for both 1x and Retina. SDL's logical window size still describes the clip panel.
bool drawable_size(SDL_Window* window, int* w, int* h);
// Picture origin in panel coordinates (top-left points), plus its full size; false for unrelated/fallback windows.
bool picture_input(SDL_Window* window, double* x, double* y, int* w, int* h);
bool picture_hit(SDL_Window* window, double x, double y);
void attach_metal_view(SDL_Window* window, void* metal_view); // reparents SDL's view on the main thread
// Thread-safe snapshot, published by pump(). false for OFF, unrelated windows, and fallback.
bool retina_size(SDL_Window* window, int* w, int* h, double* scale = nullptr);
bool gl_retina_size(SDL_Window* window, int* w, int* h); // actual current GL context view in backing pixels
bool pump();   // main thread only; returns true for "embed quit"
void frame_presented(SDL_Window* window); // flag only; all AppKit work stays in pump()
// Screenshot jobs own their GPU bytes until the background PNG/file worker finishes.
bool screenshot_take_requested(); // exactly one relaxed load on an idle present
uint64_t screenshot_take(); // claim only when requested; opaque ticket, never the protocol id
void screenshot_failed(uint64_t id);
void screenshot_submit(uint64_t id, int w, int h, size_t pitch, bool bgra, bool bottom_up,
                       std::shared_ptr<const void> owner, const uint8_t* pixels);
void destroy(); // after SDL_DestroyWindow, on the main thread
} // namespace mac_embed
