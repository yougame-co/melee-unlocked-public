// Immutable shader requests for Metal workers and persistent recipes.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../runtime/gx/gx_shader.h"

namespace gx::metal {
struct Recipe {
  VSUid vs{};
  PSUid ps{};
  uint32_t blend = 0, pixel_format = 0;
};
// The packaged Metal seed currently carries GL input records, not Metal UIDs.
struct GLRecipe { uint32_t components; BPMemory bp; uint32_t xf[0x58]; };

// Copy untrusted GL fields without calling either UID builder: make_vs_uid
// already indexes the XF image using its stored texgen count. CSS data is not
// present in GL seeds, and BP cull/zfreeze bits are not shader UID fields.
inline Recipe gl_recipe_inputs(const GLRecipe& legacy) {
  Recipe recipe{};
  recipe.vs.components = legacy.components;
  recipe.vs.numTexGens = recipe.ps.numTexGens = legacy.xf[0x3F] & 15;
  recipe.vs.numColorChans = legacy.xf[0x09] & 3;
  std::memcpy(recipe.vs.xf_regs, legacy.xf, sizeof legacy.xf);
  recipe.vs.xf_regs[0] = recipe.vs.xf_regs[1] = recipe.vs.xf_regs[2] = 0;
  recipe.ps.bp = legacy.bp;
  recipe.ps.bp.reg[BP_GENMODE] &= 0x7FFFFu & ~0xC000u;
  recipe.blend = legacy.bp.blendmode() & 0xFFFu;
  recipe.pixel_format = legacy.bp.zcontrol() & 7u;
  return recipe;
}

// Only stored/imported recipes use this gate. Do not change live GX draws.
// Masks in gx_regs.h also bound packed selectors; keep the table sizes here
// explicit so this audit stays paired with gx_shader.cpp's two generators.
inline bool valid_recipe(const Recipe& recipe) {
  const auto& vs = recipe.vs;
  const auto& ps = recipe.ps;
  const auto& bp = ps.bp;
  // XF arrays, materials[4], texmatrices[24], varyings, and lighting/component
  // shifts (including 8 * (j + 2)) all depend on these counts.
  if (vs.numTexGens > 8 || ps.numTexGens > 8 || vs.numColorChans > 2 ||
      vs.numTexGens != ps.numTexGens ||
      (vs.xf_regs[0x3F] & 15) != vs.numTexGens ||
      (vs.xf_regs[0x09] & 3) != vs.numColorChans ||
      bp.numtexgens() > 8 || bp.numcolchans() > 2 || bp.numindstages() > 4)
    return false;
  // TEV's count-minus-one occupies four bits: 1..16 stages. Reject bits
  // outside the UID's genmode mask too, including an attempted fifth count
  // bit (the live BP cull field), rather than silently accepting its truncation.
  if ((bp.reg[BP_GENMODE] & ~(0x7FFFFu & ~0xC000u)) || bp.numtevstages() + 1 > 16)
    return false;
  if (vs.components & ~0x7FFFFFu || vs.motion_vectors > 1 || ps.motion_vectors > 1 ||
      vs.motion_vectors != ps.motion_vectors || recipe.blend > 0xFFF || recipe.pixel_format > 7 ||
      vs.xf_regs[0] > 1)
    return false;
  if (vs.xf_regs[0]) {
    float a, k;
    std::memcpy(&a, &vs.xf_regs[1], sizeof a);
    std::memcpy(&k, &vs.xf_regs[2], sizeof k);
    if (!std::isfinite(a) || !std::isfinite(k)) return false;
  }
  for (uint32_t j = 0; j < vs.numColorChans; ++j) {
    for (const auto channel : {vs.xf_regs[0x0E + j], vs.xf_regs[0x10 + j]}) {
      // Diffuse has three modes; attenuation has four. Source flags are
      // one bit and the eight-bit light mask only addresses lights 0..7.
      if (lit_diffusefunc(channel) > 2 || lit_attnfunc(channel) > 3 ||
          lit_matsource(channel) > 1 || lit_ambsource(channel) > 1 ||
          lit_enable(channel) > 1 || lit_lightmask(channel) > 255) return false;
    }
  }
  for (uint32_t i = 0; i < vs.numTexGens; ++i) {
    const auto info = vs.xf_regs[0x40 + i];
    // Four XF generation modes, source rows position..tex7. The two emboss
    // selectors address eight lights/texgens; post-matrix row indices wrap
    // modulo 64 in the generator, including row+1/row+2.
    if (tmi_texgentype(info) > 3 || tmi_sourcerow(info) > 12 ||
        tmi_projection(info) > 1 || tmi_inputform(info) > 1 ||
        tmi_embosssourceshift(info) > 7 || tmi_embosslightshift(info) > 7 ||
        (vs.xf_regs[0x50 + i] & 0x3F) >= 64) return false;
  }
  for (uint32_t i = 0; i < bp.numtevstages() + 1; ++i) {
    const auto cc = bp.tev_color(i), ac = bp.tev_alpha(i), ind = bp.tevind(i);
    // cInput/overflow[16], aInput/aInputSource[8], cOut/aOut[4].
    for (int shift : {0, 4, 8, 12}) if (bits(cc, shift, 4) >= 16) return false;
    for (int shift : {4, 7, 10, 13}) if (bits(ac, shift, 3) >= 8) return false;
    for (const auto env : {cc, ac}) {
      // bias/left/right[4], op[2], lerpBias[2*op + bool] and
      // compare[2*scale + op + (alpha ? 2 : 0)] (ten entries).
      if (bits(env, 16, 2) >= 4 || bits(env, 18, 1) >= 2 ||
          bits(env, 20, 2) >= 4 || bits(env, 22, 2) >= 4) return false;
    }
    // Texture/coordinate/raster tables [8], konst tables [32], swap tables
    // [8] addressed by 2*swap + {0,1}; each swizzle selects "rgba"[4].
    if (bp.order_texmap(i) >= 8 || bp.order_texcoord(i) >= 8 || bp.order_colorchan(i) >= 8 ||
        bp.ksel_kc(i) >= 32 || bp.ksel_ka(i) >= 32) return false;
    for (const auto swap : {bits(ac, 0, 2), bits(ac, 2, 2)}) {
      if (swap >= 4) return false;
      for (const auto entry : {2 * swap, 2 * swap + 1})
        if (bp.ksel_swap1(entry) >= 4 || bp.ksel_swap2(entry) >= 4) return false;
    }
    // bt [4], format/mask/biasAdd [4], bump selector [4], biasField [8].
    // GX matrix IDs are off, 1..3, 5..7, 9..11. Holes 4/8 would emit
    // indtexmtx[-2]; 12..15 are reserved. Wrap 7 is reserved (GX ends at 6).
    const auto matrix = bits(ind, 9, 4);
    if (bits(ind, 0, 2) >= 4 || bits(ind, 2, 2) >= 4 || bits(ind, 4, 3) >= 8 ||
        bits(ind, 7, 2) >= 4 || matrix == 4 || matrix == 8 || matrix > 11 ||
        bits(ind, 13, 3) > 6 || bits(ind, 16, 3) > 6) return false;
  }
  // The indirect count bounds 1<<i, 6*i register shifts, indtexscale[2]
  // and declared indtex0..3. Missing texcoords are explicitly handled by GX.
  for (uint32_t i = 0; i < bp.numindstages(); ++i)
    if (bits(bp.iref(), 6 * i, 3) >= 8 || bits(bp.iref(), 6 * i + 3, 3) >= 8) return false;
  // Alpha funcs[8]/logics[4], fogFuncs[8] (GX fog modes 1/3 reserved),
  // and Z texture's three formats / three operations.
  const auto fog = bits(bp.fogparam3(), 21, 3);
  if (bits(bp.alpha_test(), 16, 3) >= 8 || bits(bp.alpha_test(), 19, 3) >= 8 ||
      bits(bp.alpha_test(), 22, 2) >= 4 || fog == 1 || fog == 3 ||
      bits(bp.ztex2(), 0, 2) > 2 || bits(bp.ztex2(), 2, 2) > 2) return false;
  return true;
}
inline Recipe make_recipe(const DrawCall& draw) {
  Recipe recipe{};
  // Resolve CSS texture/vertex-count and current widescreen state now. Workers
  // must never reconstruct this UID from an incomplete draw or later globals.
  recipe.vs = make_vs_uid(draw);
  recipe.ps = make_ps_uid(draw);
  recipe.blend = draw.bp.blendmode() & 0xFFFu;
  recipe.pixel_format = draw.bp.zcontrol() & 7u;
  return recipe;
}
inline constexpr uint64_t recipe_magic = 0x324C544D5847ull;
} // namespace gx::metal
