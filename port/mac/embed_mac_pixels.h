// GPU/WindowServer-free pixel policy for the opt-in embedded Retina surface.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mac_embed {
struct PixelSize { int w = 0, h = 0; };
inline bool retina_enabled(bool setting, const char* environment) {
  return setting || (environment && std::strcmp(environment, "1") == 0);
}
// The launch setting gates snapshot access itself, including its mutex, on the OFF path.
template<class Snapshot> inline bool query_retina_size(bool enabled, Snapshot snapshot) {
  return enabled && snapshot();
}
struct EmbedScaleState {
  bool active = false;
  // After losing the embedded snapshot, consume SDL points once, then leave ordinary windows alone.
  template<class SdlSize> bool update(bool retina, int& w, int& h, SdlSize sdl_size) {
    if (!retina) {
      if (!active) return false;
      sdl_size(w, h);
    }
    if (h <= 0) return false; // Retry the transition when SDL has a usable size.
    active = retina;
    return true;
  }
};
// A disabled override must not even rewrite the caller's legacy SDL dimensions.
inline bool override_size(bool enabled, PixelSize pixels, int* w, int* h) {
  if (!enabled) return false;
  *w = pixels.w; *h = pixels.h;
  return true;
}
inline PixelSize backing_size(double w, double h, double scale) {
  if (!(scale > 0) || !std::isfinite(scale)) scale = 1;
  return {int(std::lround(w * scale)), int(std::lround(h * scale))};
}
struct PixelBox { int x, y, w, h; }; // bottom-left origin, GL centering (Metal flips Y)
inline PixelBox letterbox(int w, int h, double aspect) {
  int pw = w, ph = int(std::lround(w / aspect));
  if (ph > h) { ph = h; pw = int(std::lround(h * aspect)); }
  return {(w - pw) / 2, (h - ph) / 2, pw, ph};
}
inline int efb_scale(int pinned, int pixel_height, int ssaa = 1) {
  return std::clamp((pinned > 0 ? pinned : (pixel_height + 527) / 528) * std::clamp(ssaa, 1, 2), 1, 6);
}
// ImGui lays out in logical units, renders those units in pixels, and receives SDL points.
inline double point_to_ui(double point, int pixels, int points, double ui_scale) {
  return point * (points > 0 ? double(pixels) / points : 1) / ui_scale;
}
// ImGui's GL backend truncates DisplaySize * FramebufferScale to int. Round the float ratio upward if necessary
// so an odd drawable size never loses its last row/column through float multiplication.
inline float framebuffer_ratio(int pixels, int logical) {
  const float ratio = float(pixels) / logical;
  return int(logical * ratio) < pixels ? std::nextafter(ratio, INFINITY) : ratio;
}
struct UiSize { int w, h; float framebuffer_x, framebuffer_y; };
inline UiSize ui_size(int w, int h, double scale) {
  const int lw = std::max(1, int(std::ceil(w / scale - 0.001)));
  const int lh = std::max(1, int(std::ceil(h / scale - 0.001)));
  return {lw, lh, framebuffer_ratio(w, lw), framebuffer_ratio(h, lh)};
}
inline UiSize display_size(bool retina, int w, int h, float scale) {
  if (retina) return ui_size(w, h, scale);
  // Exactly the pre-Retina host arithmetic, including its float rounding.
  return {scale == 1 ? w : int(std::ceil(float(w) / scale - 0.001f)),
          scale == 1 ? h : int(std::ceil(float(h) / scale - 0.001f)), scale, scale};
}
// Desktop capture contract: at most 8192 on either side and 16 million pixels.
inline PixelSize capture_size(int w, int h, bool retina) {
  if (!retina || w <= 0 || h <= 0) return {w, h};
  const double scale = std::min({1.0, 8192.0 / w, 8192.0 / h,
                                std::sqrt(16000000.0 / (double(w) * h))});
  return {std::max(1, int(std::floor(w * scale))), std::max(1, int(std::floor(h * scale)))};
}
// Packed RGB, nearest-neighbor at pixel centers. Row orientation is preserved.
// The returned dimensions must be used by the image encoder AND its metadata.
inline PixelSize limit_capture(std::vector<uint8_t>& rgb, int w, int h, bool retina) {
  const auto out = capture_size(w, h, retina);
  if (out.w == w && out.h == h) return out; // below the cap: retain every byte
  std::vector<uint8_t> resized(size_t(out.w) * out.h * 3);
  for (int y = 0; y < out.h; ++y) {
    const int sy = int((int64_t(2) * y + 1) * h / (int64_t(2) * out.h));
    for (int x = 0; x < out.w; ++x) {
      const int sx = int((int64_t(2) * x + 1) * w / (int64_t(2) * out.w));
      std::memcpy(resized.data() + (size_t(y) * out.w + x) * 3,
                  rgb.data() + (size_t(sy) * w + sx) * 3, 3);
    }
  }
  rgb.swap(resized);
  return out;
}
} // namespace mac_embed
