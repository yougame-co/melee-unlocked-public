// Window-free regression: real keyboard bindings + real routed ImGui feed + bundled ImGui.
// This is automated coverage, not a real-game player-flow certification.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "keyboard_ui_navigation.h"
#include "settings_imgui_input.h"
#include "../runtime/hle/yougame_parity.h"
#include "imgui_internal.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static int checks, scenarios;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct Keyboard {
  host::KeyBindings bindings = host::default_key_bindings();
  host::box::State game, nav;
  host::PadState raw{}, navigation{};
  void poll(int held, bool swallowed = false, bool shift_only = false) {
    raw = {}; navigation = {};
    auto key = [&](int vk) { return !swallowed && vk == held; };
    host::keyboard_apply_bindings(raw, bindings, key, game);
    host::keyboard_apply_bindings(navigation, bindings,
        [&](int vk) { return !host::imgui_keyboard_navigation_key(vk, shift_only) && key(vk); }, nav);
  }
};

struct Menu {
  ImGuiID ids[3]{};
  int clicks[3]{};
  bool request_popup = false, popup_visible = false;
  Menu() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(640, 240);
    gx::settings_input::configure(io);
    gx::settings_input::navigation(io, true);
    io.Fonts->Build();
    frame({}); frame({}); frame({});
    CHECK(ImGui::GetFocusID() == ids[0]);
    ImGui::SetNavCursorVisible(true); // The F1 menu explicitly reveals its initial focus too.
  }
  ~Menu() { ImGui::DestroyContext(); }
  void frame(const host::PadState& pad) {
    auto& io = ImGui::GetIO();
    gx::settings_input::gamepad(io, pad);
    io.DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(600, 180), ImGuiCond_Always);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i);
      ImGui::SetCursorPos(ImVec2(20.f + i * 180.f, 60));
      if (ImGui::InvisibleButton("##action", ImVec2(160, 50), ImGuiButtonFlags_EnableNav)) ++clicks[i];
      ids[i] = ImGui::GetItemID();
      if (i == 0) ImGui::SetItemDefaultFocus();
      ImGui::PopID();
    }
    if (request_popup) { ImGui::OpenPopup("escape-regression"); request_popup = false; }
    popup_visible = ImGui::BeginPopup("escape-regression");
    if (popup_visible) { ImGui::TextUnformatted("Escape regression"); ImGui::EndPopup(); }
    ImGui::End(); ImGui::Render();
  }
};

void move(int source, int delay, bool unfiltered = false) {
  ++scenarios;
  Menu menu;
  Keyboard keyboard;
  if (source == 1) keyboard.bindings.vk[(int)host::BindAction::SRight] = 'D';
  auto& io = ImGui::GetIO();
  ImGuiID previous = menu.ids[0];
  int moves = 0;
  for (int frame = 0; frame < 60; ++frame) {
    // One backend down/up, no repeat; polling may see the same key on this or later frames.
    if (source == 0 && frame < 2) io.AddKeyEvent(ImGuiKey_RightArrow, frame == 0);
    const bool held = frame >= delay && frame < delay + 2;
    keyboard.poll(held ? (source == 1 ? 'D' : VK_RIGHT) : 0);
    host::PadState pad = unfiltered ? keyboard.raw : keyboard.navigation;
    if (source == 2) { pad = {}; pad.button = held ? 2 : 0; } // Physical controller, unchanged.
    menu.frame(pad);
    if (ImGui::GetFocusID() != previous) { ++moves; previous = ImGui::GetFocusID(); }
  }
  CHECK(moves == (unfiltered ? 2 : 1));
  CHECK(ImGui::GetFocusID() == menu.ids[unfiltered ? 2 : 1]);
  CHECK(menu.clicks[0] + menu.clicks[1] + menu.clicks[2] == 0);
}

void activate(ImGuiKey backend, int vk, host::BindAction action, bool rebound = false) {
  ++scenarios;
  Menu menu;
  Keyboard keyboard;
  keyboard.bindings.vk[(int)action] = vk;
  for (int frame = 0; frame < 60; ++frame) {
    if (!rebound && frame < 2) ImGui::GetIO().AddKeyEvent(backend, frame == 0);
    keyboard.poll(frame >= 2 && frame < 4 ? vk : 0);
    menu.frame(keyboard.navigation);
    CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadStart)); // Enter must never close Settings as Start.
  }
  if (menu.clicks[0] != 1) std::fprintf(stderr, "activation backend %d vk %d action %d rebound %d clicks %d,%d,%d\n", int(backend), vk, int(action), int(rebound), menu.clicks[0], menu.clicks[1], menu.clicks[2]);
  CHECK(menu.clicks[0] == 1 && menu.clicks[1] == 0 && menu.clicks[2] == 0);
}

// Reproduce the second Escape owner in real ImGui, then prove embedded routing
// leaves NavCancel idle even with Escape rebound to B in the raw gameplay pad.
void escape_cancel(bool routed) {
  ++scenarios;
  Menu menu;
  Keyboard keyboard;
  keyboard.bindings.vk[(int)host::BindAction::B] = VK_ESCAPE;
  menu.request_popup = true;
  menu.frame({}); menu.frame({});
  CHECK(menu.popup_visible);
  int back_steps = 0, relays = 0;
  for (int frame = 0; frame < 60; ++frame) {
    if (frame < 3) {
      const bool capture_key = yougame::parity::embedded_escape(
          frame != 2, false, frame == 1, false,
          [&](const char* key) { CHECK(std::string(key) == "escape"); ++relays; },
          [&] { ++back_steps; });
      CHECK(!capture_key);
      if (!routed && frame != 1) ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, frame == 0);
    }
    keyboard.poll(frame < 2 ? VK_ESCAPE : 0);
    CHECK(keyboard.navigation.button == 0); // Round-31 pad copy still filters Escape.
    menu.frame(keyboard.navigation);
    if (routed) {
      CHECK(!ImGui::IsKeyDown(ImGuiKey_Escape));
      CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight));
      CHECK(menu.popup_visible); // No extra NavCancel: game back callback is the sole owner.
    }
  }
  CHECK(back_steps == 1 && relays == 1);
  if (!routed) CHECK(!menu.popup_visible); // Old backend path independently cancels the popup too.
}

void binding_audit() {
  Keyboard keyboard;
  // Every keyboard-sourced gamepad key (including Z/Back and Start), plus all other actions:
  // filtering is by key contribution, never by output bit or whole keyboard device.
  for (int vk : {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_SPACE, VK_RETURN, VK_ESCAPE,
                 VK_TAB, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, VK_MENU, VK_LMENU, VK_RMENU,
                 0x5D, VK_F10, int('D'), VK_NUMPAD0 + 6}) {
    for (int a = 0; a < (int)host::BindAction::Count; ++a) {
      keyboard.bindings = {};
      keyboard.bindings.vk[a] = vk;
      keyboard.poll(vk);
      const auto p = keyboard.navigation;
      if (host::imgui_keyboard_navigation_key(vk))
        CHECK(p.button == 0 && p.stick_x == 0 && p.stick_y == 0 && p.sub_x == 0 && p.sub_y == 0 && p.trig_l == 0 && p.trig_r == 0);
      else CHECK(p.button == keyboard.raw.button && p.stick_x == keyboard.raw.stick_x && p.stick_y == keyboard.raw.stick_y);
      keyboard.poll(0);
    }
  }
  keyboard.bindings = host::default_key_bindings();
  keyboard.poll(VK_RETURN);
  CHECK(keyboard.raw.button & 0x1000);
  CHECK(keyboard.navigation.button == 0);
  keyboard.poll(VK_RIGHT);
  CHECK(keyboard.raw.stick_x == 127 && keyboard.navigation.stick_x == 0);
  keyboard.poll(VK_RETURN, true); // The existing embedded shortcut predicate applies to both copies.
  CHECK(keyboard.raw.button == 0 && keyboard.navigation.button == 0);
  // Rebound Start still closes Settings through GamepadStart; default Enter activates instead.
  keyboard.bindings.vk[(int)host::BindAction::Start] = 'P';
  keyboard.poll('P');
  CHECK(keyboard.navigation.button & 0x1000);
  keyboard.bindings.vk[(int)host::BindAction::A] = VK_F10;
  keyboard.poll(VK_F10);
  CHECK(keyboard.navigation.button & 0x100); // Plain F10 is still a rebound button.
  keyboard.poll(VK_F10, false, true);
  CHECK(keyboard.raw.button & 0x100);
  CHECK(!(keyboard.navigation.button & 0x100)); // Shift+F10 belongs to ImGui's context menu.
  // Separate box SOCD histories: filtering arrows must not change gameplay's last-input winner.
  keyboard.bindings = host::default_key_bindings();
  keyboard.bindings.vk[(int)host::BindAction::ModX] = 'M';
  keyboard.poll(VK_LEFT);
  CHECK(keyboard.raw.stick_x < 0 && keyboard.navigation.stick_x == 0);
  keyboard.poll(VK_RIGHT);
  CHECK(keyboard.raw.stick_x > 0 && keyboard.navigation.stick_x == 0);
  keyboard.bindings.vk[(int)host::BindAction::SRight] = 'D';
  keyboard.poll('D');
  CHECK(keyboard.raw.stick_x > 40 && keyboard.navigation.stick_x == keyboard.raw.stick_x);
}

int main() {
  for (int delay : {0, 1, 2}) for (int source : {0, 1, 2}) move(source, delay);
  move(0, 2, true); // Reproduce two focus moves from one backend press before filtering.
  activate(ImGuiKey_Enter, VK_RETURN, host::BindAction::Start);
  activate(ImGuiKey_KeypadEnter, VK_RETURN, host::BindAction::Start);
  activate(ImGuiKey_Space, VK_SPACE, host::BindAction::A);
  activate(ImGuiKey_Enter, VK_RETURN, host::BindAction::A);
  activate(ImGuiKey_None, 'J', host::BindAction::A, true);
  activate(ImGuiKey_None, VK_F10, host::BindAction::A, true);
  escape_cancel(false);
  escape_cancel(true);
  binding_audit();
  std::printf("keyboard_navigation: %d checks; %d real ImGui scenarios, 60 frames each; default/rebound/controller, delayed polling, old double move, activation, all 27 bindings, box history and Escape NavCancel ownership\n", checks, scenarios);
}
