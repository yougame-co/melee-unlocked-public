// A picture the overlay keeps for the whole session (the practice menu's game font, a room's
// portraits, the character select's extra) while the overlay itself can be rebuilt: Direct3D 12
// rebuilds it, with a new ImGui context, when HDR output turns on or off (at start, too).
//
// Two things went wrong with such a texture. It was registered again only when the context
// pointer changed, but a new context can be allocated at the old one's address: the texture then
// never joined the new context's list, was never uploaded, and drew as nothing (the practice
// menu's bars came up with no words on them, GitHub report, RTX 5060; reproduced with
// MELEE_TEST_OVERLAY_REBUILD). And a texture registered in each new context without leaving the old
// one counted 2, so the next renderer teardown, which frees only textures counted once, left it
// with a dead handle. keep_user_texture() looks at the current context's own list instead, so a
// new overlay always gets a fresh registration and upload, and the count stays at one.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"
#include "imgui_internal.h"

namespace gx {

// Once a frame before drawing `tex`. `registered_in` is kept for the callers' bookkeeping only.
inline void keep_user_texture(ImTextureData* tex, ImGuiContext*& registered_in) {
  ImGuiContext* now = ImGui::GetCurrentContext();
  if (!tex || !now) return;
  if (!now->UserTextures.contains(tex)) {
    // Not in this context's list: a new overlay. Whatever registration and GPU copy it had belonged
    // to a context and a renderer that are gone (freed there when the count was one).
    tex->RefCount = 0;
    if (tex->Status != ImTextureStatus_WantCreate) {
      tex->SetTexID(ImTextureID_Invalid);
      tex->BackendUserData = nullptr;
      tex->SetStatus(ImTextureStatus_WantCreate);
    }
    ImGui::RegisterUserTexture(tex);
  } else if (tex->Status == ImTextureStatus_Destroyed) {
    tex->SetStatus(ImTextureStatus_WantCreate);   // a renderer reset inside the same context
  }
  registered_in = now;
}

}  // namespace gx
