// GPU-free regression fixture; link with the shader generator and hash_bytes.
// No SDL, Metal device, guest state or game binary is used.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_metal_recipe.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

static unsigned checks = 0;
static void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
  ++checks;
}
int main() {
  try {
    static_assert(std::is_trivially_copyable_v<gx::metal::Recipe>);
    gx::DrawCall draw{};
    draw.components = gx::VB_HAS_COL0 | gx::VB_HAS_UV0;
    draw.vertex_count = 45;
    draw.textures[0].used = true;
    draw.textures[0].width = 104; draw.textures[0].height = 72;
    draw.textures[0].format = 0;
    draw.bp.reg[gx::BP_BLENDMODE] = 0x18;
    gx::set_css_scene(true);
    gx::set_true_widescreen(true, 21.0f / 9.0f);
    const auto requested = gx::metal::make_recipe(draw);
    check(requested.vs.xf_regs[0] == 1, "fixture did not select widened CSS shader");
    const auto source = gx::generate_vertex_shader(requested.vs);
    const auto key = requested.vs.hash();

    // Reproduce the old lossy reconstruction and a later frame's global state.
    draw.vertex_count = 0; draw.textures[0] = {};
    const auto lossy = gx::metal::make_recipe(draw);
    check(lossy.vs.hash() != key, "fixture does not distinguish lost texture/count");
    check(gx::generate_vertex_shader(lossy.vs) != source, "fixture generated identical variants");
    gx::set_css_scene(false); gx::set_true_widescreen(false);
    check(gx::generate_vertex_shader(requested.vs) == source, "worker source changed with later globals");

    // Persistent recipes preserve the exact UID, including computed CSS floats.
    gx::metal::Recipe restored{};
    std::memcpy(&restored, &requested, sizeof restored);
    check(restored.vs.hash() == key && restored.ps.hash() == requested.ps.hash(), "recipe round trip changed keys");
    check(restored.blend == 0x18 && restored.pixel_format == requested.pixel_format, "recipe lost pipeline state");
    check(gx::generate_vertex_shader(restored.vs) == source, "recipe round trip changed source");
    check(gx::metal::recipe_magic != 0x314C544D5847ull, "old recipe version accepted");
    for (const auto& valid : {requested, lossy, restored})
      check(gx::metal::valid_recipe(valid), "existing valid fixture rejected");
    auto reject = [&](auto corrupt) {
      auto bad = restored;
      corrupt(bad);
      check(!gx::metal::valid_recipe(bad), "malformed stored recipe accepted");
    };
    reject([](auto& r) { r.vs.numTexGens = 25; });
    reject([](auto& r) { r.vs.numColorChans = 3; });
    // Four count-minus-one bits can only encode 1..16; the attempted 17th
    // stage spills into a bit forbidden in a stored shader UID (BP culling).
    reject([](auto& r) { r.ps.bp.reg[gx::BP_GENMODE] = 16u << 10; });
    reject([](auto& r) { r.ps.numTexGens = 25; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_GENMODE] = 5u << 16; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_GENMODE] = 9; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_GENMODE] = 3u << 4; });
    reject([](auto& r) { r.vs.motion_vectors = 2; });
    reject([](auto& r) { r.ps.motion_vectors = 1; });
    reject([](auto& r) { r.blend = 0x1000; });
    reject([](auto& r) { r.pixel_format = 8; });
    reject([](auto& r) { r.vs.components = 1u << 31; });
    reject([](auto& r) { r.vs.xf_regs[0] = 2; });
    reject([](auto& r) {
      const float nan = std::numeric_limits<float>::quiet_NaN();
      std::memcpy(&r.vs.xf_regs[1], &nan, sizeof nan);
    });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_IND_CMD] = 4u << 9; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_IND_CMD] = 8u << 9; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_IND_CMD] = 12u << 9; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_IND_CMD] = 7u << 13; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_IND_CMD] = 7u << 16; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_ZTEX2] = 3; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_ZTEX2] = 3u << 2; });
    reject([](auto& r) { r.ps.bp.reg[gx::BP_FOGPARAM3] = 1u << 21; });

    gx::DrawCall maximum{};
    maximum.components = 0x7FFFFF;
    maximum.xf_regs[0x3F] = 8; maximum.xf_regs[0x09] = 2;
    maximum.bp.reg[gx::BP_GENMODE] = 8 | (2u << 4) | (15u << 10) | (4u << 16);
    for (unsigned i = 0; i < 8; ++i) {
      maximum.xf_regs[0x40 + i] = (12u << 7) | (3u << 4) | (7u << 12) | (7u << 15);
      maximum.xf_regs[0x50 + i] = 63 | (1u << 8);
      maximum.bp.reg[gx::BP_TREF + i] = 0xFFFFFF;
      maximum.bp.reg[gx::BP_TEV_KSEL + i] = 0xFFFFFF;
    }
    maximum.xf_regs[0x12] = 1;
    for (unsigned i = 0; i < 16; ++i) {
      maximum.bp.reg[gx::BP_TEV_COLOR_ENV + 2 * i] = 0xFFFFFF;
      maximum.bp.reg[gx::BP_TEV_ALPHA_ENV + 2 * i] = 0xFFFFFF;
      maximum.bp.reg[gx::BP_IND_CMD + i] = 3 | (3u << 2) | (7u << 4) | (3u << 7) |
        (11u << 9) | (6u << 13) | (6u << 16);
    }
    maximum.bp.reg[gx::BP_IREF] = 0xFFFFFF;
    maximum.bp.reg[gx::BP_ALPHACOMPARE] = 0xFF0000;
    maximum.bp.reg[gx::BP_FOGPARAM3] = 7u << 21;
    auto edge = gx::metal::make_recipe(maximum);
    check(gx::metal::valid_recipe(edge), "maximum legal GX counts/selectors rejected");
    check(!gx::generate_vertex_shader(edge.vs).empty() && !gx::generate_pixel_shader(edge.ps).empty(),
          "maximum legal recipe did not generate");
    auto reject_edge = [&](auto corrupt) {
      auto bad = edge; corrupt(bad);
      check(!gx::metal::valid_recipe(bad), "malformed vertex enum accepted");
    };
    reject_edge([](auto& r) { r.vs.xf_regs[0x47] = 4u << 4; });
    reject_edge([](auto& r) { r.vs.xf_regs[0x47] = 13u << 7; });
    reject_edge([](auto& r) { r.vs.xf_regs[0x0F] = 3u << 7; });
    reject_edge([](auto& r) { r.vs.xf_regs[0x11] = 3u << 7; });
    // Exercise the post-matrix modulo-64 path, and all legal lighting shifts.
    edge.vs.xf_regs[0x47] = 1u << 1;
    edge.vs.xf_regs[0x0F] = edge.vs.xf_regs[0x11] = 0x7F7F; // diffuse=2, all lights
    check(gx::metal::valid_recipe(edge) && !gx::generate_vertex_shader(edge.vs).empty(),
          "legal post-matrix/lighting boundary rejected");

    gx::metal::GLRecipe legacy{};
    legacy.components = maximum.components; legacy.bp = maximum.bp;
    std::memcpy(legacy.xf, maximum.xf_regs, sizeof legacy.xf);
    check(gx::metal::valid_recipe(gx::metal::gl_recipe_inputs(legacy)), "valid GL seed rejected");
    legacy.xf[0x3F] = 15;
    check(!gx::metal::valid_recipe(gx::metal::gl_recipe_inputs(legacy)), "unsafe GL UID construction permitted");
    legacy.xf[0x3F] = 8; legacy.xf[0x09] = 3;
    check(!gx::metal::valid_recipe(gx::metal::gl_recipe_inputs(legacy)), "invalid GL color count accepted");
    legacy.xf[0x09] = 2; legacy.xf[0x47] = 4u << 4;
    check(!gx::metal::valid_recipe(gx::metal::gl_recipe_inputs(legacy)), "invalid GL enum accepted");
    std::cout << "Metal recipe checks: " << checks << '/' << checks << " passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
