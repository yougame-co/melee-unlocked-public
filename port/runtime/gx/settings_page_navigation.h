// Shared page entry and label-only focus observation for the settings presentations.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace gx::settings_page {
inline const char* public_label(const char* text) {
  if (!text || *text != '#') return text;
  // These fields put their visible caption beside the widget instead of in its ImGui ID.
  static constexpr const char* names[][2] = {
      {"##gecko_name", "Name"}, {"##gecko_body", "Code lines"},
      {"##profile", "Profile"}, {"##new_name", "Name"},
      {"##dlss5_profile", "Profile"}, {"##dlss5_name", "Name"},
      {"##shaderfilter", "Search presets"}, {"##current_menu_style", "Menu style"},
      {"##palette_swatch", "Palette"}, {"##menu_accent_wheel", "Custom menu accent"},
      {"##variant", "Variant"}, {"##pick", "Variant"}, {"##editing", "Controller"},
      {"##palette", "Palette"}, {"##appearance_preview", "Menu style"},
      {"##appearance_picture", "Menu style"}, {"##appearance_style", "Menu style"}};
  for (const auto& name : names) if (std::strcmp(text, name[0]) == 0) return name[1];
  return text;
}
struct Navigation {
  int tab = -1, style = -1;
  ImGuiID logged_focus = 0;
  bool active = false;
  std::unordered_map<ImGuiID, std::string> labels;

  void begin(int next_tab, int next_style) {
    active = true;
    labels.clear();
    if (tab != next_tab || style != next_style) {
      // Unlike default focus, this also replaces a previously visited child's NavLastIds.
      ImGui::SetScrollY(0.0f);
      ImGui::SetKeyboardFocusHere();
      // Focus a slider/input without opening its editor or consuming the player's next Down.
      GImGui->NavMoveFlags &= ~ImGuiNavMoveFlags_Activate;
      logged_focus = 0;
    }
    tab = next_tab;
    style = next_style;
  }
  void leave() { active = false; tab = style = -1; logged_focus = 0; labels.clear(); }
  void label(ImGuiID id, const char* text) {
    text = public_label(text);
    if (!active || !id || !text || !*text || *text == '#') return;
    // Never include an ImGui ID suffix or a field's value in the log.
    const char* end = ImGui::FindRenderedTextEnd(text);
    labels[id] = std::string(text, end);
    for (char& c : labels[id]) if (c == '\n' || c == '\r') c = ' ';
  }
  const char* changed_focus() {
    const ImGuiID id = ImGui::GetFocusID();
    if (id == logged_focus) return nullptr;
    auto found = labels.find(id);
    if (found == labels.end()) { logged_focus = 0; return nullptr; }
    logged_focus = id;
    return found->second.c_str();
  }
};
// Only the content being submitted in this ImGui frame is observed. Helpers used elsewhere
// (practice, category homes, popups outside settings) cannot produce settings-focus messages.
inline thread_local Navigation* observer = nullptr;
inline void label_item(const char* label) {
  if (observer) observer->label(ImGui::GetItemID(), label);
}
inline void label_focused_parts(const char* label) {
  // Compound widgets (RGB inputs, two-component sizes) focus a child ID. Associate that
  // rectangle with the control caption, without inspecting any input buffer or component value.
  auto& g = *GImGui;
  auto* window = ImGui::GetCurrentWindow();
  if (!observer || !g.NavId || g.NavWindow != window) return;
  ImRect rect = window->NavRectRel[g.NavLayer];
  rect.Translate(window->Pos);
  if (g.LastItemData.Rect.Contains(rect)) observer->label(g.NavId, label);
}
}  // namespace gx::settings_page

namespace gx::settings_widgets {
// Preserve widget IDs, arguments, return values and default parameters. Record the public label
// before a combo/popup submits its own items; never inspect the value being edited.
#define SETTINGS_OBSERVED_WIDGET(name) \
  template<class... Args> bool name(const char* label, Args&&... args) { \
    if (settings_page::observer) settings_page::observer->label(ImGui::GetID(label), label); \
    const bool result = ImGui::name(label, std::forward<Args>(args)...); \
    settings_page::label_focused_parts(label); \
    return result; \
  }
SETTINGS_OBSERVED_WIDGET(Button)
SETTINGS_OBSERVED_WIDGET(SmallButton)
SETTINGS_OBSERVED_WIDGET(Checkbox)
SETTINGS_OBSERVED_WIDGET(Combo)
SETTINGS_OBSERVED_WIDGET(BeginCombo)
SETTINGS_OBSERVED_WIDGET(SliderInt)
SETTINGS_OBSERVED_WIDGET(SliderFloat)
SETTINGS_OBSERVED_WIDGET(InputInt)
SETTINGS_OBSERVED_WIDGET(InputInt2)
SETTINGS_OBSERVED_WIDGET(InputFloat)
SETTINGS_OBSERVED_WIDGET(InputText)
SETTINGS_OBSERVED_WIDGET(InputTextWithHint)
SETTINGS_OBSERVED_WIDGET(InputTextMultiline)
SETTINGS_OBSERVED_WIDGET(ColorEdit3)
SETTINGS_OBSERVED_WIDGET(ColorEdit4)
SETTINGS_OBSERVED_WIDGET(ColorPicker3)
SETTINGS_OBSERVED_WIDGET(ColorButton)
SETTINGS_OBSERVED_WIDGET(RadioButton)
SETTINGS_OBSERVED_WIDGET(Selectable)
SETTINGS_OBSERVED_WIDGET(CollapsingHeader)
SETTINGS_OBSERVED_WIDGET(TreeNode)
SETTINGS_OBSERVED_WIDGET(TreeNodeEx)
SETTINGS_OBSERVED_WIDGET(BeginTabItem)
SETTINGS_OBSERVED_WIDGET(ArrowButton)
SETTINGS_OBSERVED_WIDGET(ImageButton)
#undef SETTINGS_OBSERVED_WIDGET
}  // namespace gx::settings_widgets
