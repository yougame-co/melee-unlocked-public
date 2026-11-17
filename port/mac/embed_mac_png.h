// Private PNG writer: independent of texture-pack global flip/compression settings.
// Included only by the worker implementation and the GPU-free fixture.
#pragma once
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#undef STB_IMAGE_WRITE_STATIC
#undef STBI_WRITE_NO_STDIO
#include "embed_mac_screenshot.h"
namespace mac_embed::shot {
inline std::vector<uint8_t> encode(const std::vector<uint8_t>& rgb, PixelSize size) {
  if (!size_ok(size.w, size.h) || rgb.size() != size_t(size.w) * size.h * 3) return {};
  struct Output { std::vector<uint8_t> bytes; bool overflow = false; } out;
  const auto append = [](void* context, void* bytes, int n) {
    auto& out = *static_cast<Output*>(context);
    if (n < 0 || out.overflow || size_t(n) > max_png - out.bytes.size()) { out.overflow = true; return; }
    auto* begin = static_cast<uint8_t*>(bytes);
    try { out.bytes.insert(out.bytes.end(), begin, begin + n); } catch (...) { out.overflow = true; }
  };
  if (!stbi_write_png_to_func(append, &out, size.w, size.h, 3, rgb.data(), size.w * 3) || out.overflow) return {};
  return std::move(out.bytes);
}
}
