// Keyboard binding application shared by gameplay, previews and ImGui navigation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "input_bindings.h"

namespace host {
// ImGui 1.92.8 NavUpdate/NavUpdateCreateTabbingRequest/NavUpdatePageUpPageDown
// and NavUpdateContextMenuRequest/NavUpdateWindowing already consume these backend keys. VK_RETURN also covers KeypadEnter on both hosts.
// Keypad digits are NOT navigation keys in ImGui: SDL maps KP arrows to Keypad1..9,
// and Win32 maps them to arrows/Home/etc only when NumLock is off (those VKs match below).
inline bool imgui_keyboard_navigation_key(int vk, bool shift_only = false) {
  switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_SPACE: case VK_RETURN: case VK_ESCAPE: case VK_TAB:
    case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case 0x5D: return true; // VK_APPS -> ImGuiKey_Menu (no SDL keyboard binding).
    case VK_F10: return shift_only; // NavUpdateContextMenuRequest: precisely Shift+F10.
    default: return false;
  }
}

template <class Key> uint32_t keyboard_apply_bindings(PadState& kb, const KeyBindings& bindings, Key key,
                                                   box::State& box_state, int modifier_percent = 50) {
  const uint32_t actions = button_apply_bindings(kb, 0, 0,
      [&](int i) { const int vk = bindings.vk[i]; return vk && key(vk); }, [&](int i) { return (int)bindings.level[i]; });
  apply_stick_actions(actions, kb.stick_x, kb.stick_y, modifier_percent);
  if (kb.button & 0x40) kb.trig_l = 255;
  if (kb.button & 0x20) kb.trig_r = 255;
  // Mod X or Mod Y bound: the keyboard is a box, and its stick and C-stick take the box coordinates.
  apply_box_actions(bindings.vk[(size_t)BindAction::ModX] || bindings.vk[(size_t)BindAction::ModY],
                    actions, kb, box_state);
  return actions;
}
} // namespace host
