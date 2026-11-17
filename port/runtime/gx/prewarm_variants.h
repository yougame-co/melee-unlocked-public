// Which pipeline variants the Direct3D 12 startup prewarm builds from each saved recipe, and which
// of them the start waits for. SPDX-License-Identifier: GPL-2.0-or-later
//
// A pipeline key's mvec field (gx_d3d12.cpp get_pso): bit 0 is the motion-vector outputs an
// upscaler reads, bits 1..5 the path tracer's G-buffer, the HUD mask and the Remix layer, and bits 8
// and up the MSAA sample count (MSAA only draws without motion vectors).
//
// The start used to wait for both the plain and the motion-vector variant of every recipe whenever
// an upscaler was available on the PC (XeSS runs on any Direct3D 12 card), whatever the player had
// chosen. A session only ever draws with one of them, so only that one was in the saved library: the
// next start compiled the other half behind "Preparing graphics for this PC (first start only)" (pc3,
// AMD Radeon iGPU, XeSS available, upscaler off, 11-15: "prewarmed 80 pipelines from 40 recipes",
// cache_hits=40 compiled=40, the panel at 61/80 on the second launch). The start now waits only for
// the variant this session draws with; the other one compiles behind it, after the boot, so
// switching the upscaler on later still finds it built.
#pragma once
#include <cstdint>

namespace gx::prewarm_variants {

constexpr uint32_t kMotionVectors = 1u;
constexpr uint32_t kMsaaMask = 0xFF00u;

struct Variant {
  uint32_t mvec = 0;   // the pipeline key's mvec field
  bool wait = false;   // the start waits for it (and the progress panel counts it)
};
struct Plan {
  Variant variants[2];
  int count = 0;
};

// session_upscaler: this session draws with an upscaler (every draw uses the motion-vector variant).
// upscaler_possible: an upscaler is available on this PC, so the player may switch it on or off.
// msaa_samples: the sample count plain draws use this session (1 = off).
// prewarm_only: the precompile pass, which ends once the prewarm returns, so it waits for all.
inline Plan plan(bool session_upscaler, bool upscaler_possible, uint32_t msaa_samples, bool prewarm_only) {
  const uint32_t plain = msaa_samples > 1 ? (msaa_samples << 8) & kMsaaMask : 0u;
  Plan p;
  p.variants[p.count++] = {session_upscaler ? kMotionVectors : plain, true};
  if (upscaler_possible || session_upscaler)
    p.variants[p.count++] = {session_upscaler ? plain : kMotionVectors, prewarm_only};
  return p;
}

inline bool motion_vectors(uint32_t mvec) { return (mvec & kMotionVectors) != 0; }

// A compiled pipeline's recipe is the game's own draw state, kept in recipes.bin, when its key is
// one plan() builds again: plain or motion vectors, at any MSAA count. Keeping only mvec == 0 lost
// every recipe a session drew with an upscaler or MSAA on.
inline bool keeps_recipe(uint32_t mvec) { return (mvec & ~(kMsaaMask | kMotionVectors)) == 0; }

}  // namespace gx::prewarm_variants
