// Opt-in native Metal GX backend. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace gx {
struct Backend;
struct RenderOptions;
Backend* create_metal_backend(void* sdl_window, int width, int height, const RenderOptions& options);
}
