// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_shortcuts.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace yougame::shortcuts;
using namespace yougame::parity;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static void expect(Keys::Result event, bool swallowed, const char* menu = nullptr, const char* chat = nullptr) {
  CHECK(event.swallowed == swallowed);
  const char* expected[] = {menu, chat};
  for (int i = 0; i < 2; ++i) {
    CHECK((event.lines[i] == nullptr) == (expected[i] == nullptr));
    if (expected[i]) CHECK(std::string(event.lines[i]) == expected[i]);
  }
}
static Config configured(const std::string& text) {
  Config config;
  CHECK(parse(text, Config{}, config));
  return config;
}
static void parser() {
  Combo combo;
  CHECK(parse_combo("sHiFt+tAb", combo) && (combo == Combo{Shift, 9}));
  CHECK(parse_combo("CTRL+alt+k", combo) && (combo == Combo{Ctrl | Alt, 'K'}));
  CHECK(parse_combo("Alt+Ctrl+Shift+Enter", combo) && (combo == Combo{7, 13}));
  CHECK(parse_combo("Shift+Backquote", combo) && combo.key == 192);
  for (int f = 1; f <= 12; ++f) CHECK(parse_combo("Ctrl+F" + std::to_string(f), combo) && combo.key == 111 + f);
  for (char c = 'A'; c <= 'Z'; ++c) CHECK(parse_combo(std::string("Alt+") + c, combo) && combo.key == c);
  for (const char* bad : {"", "Tab", "Shift", "Shift+", "+Tab", "Shift++Tab", "Shift+Shift+Tab",
      "Shift+Ctrl", "Tab+Shift", "Shift+Tab+K", "Meta+K", "Control+K", "Shift+Escape", "Shift+Space",
      "Shift+F0", "Shift+F13", "Shift+F01", "Shift+1", "Shift+`", "Shift+AA", " Shift+Tab", "Shift+Tab ", "Shift+é"}) {
    const Combo previous = combo;
    CHECK(!parse_combo(bad, combo));
    CHECK(combo == previous);
  }
  const auto full = configured("shortcuts menu=Shift+Tab chat=Shift+Enter");
  CHECK((full.menu == Combo{Shift, 9}) && (full.chat == Combo{Shift, 13}));
  Config next;
  CHECK(parse("shortcuts MENU=Ctrl+Alt+K", full, next));
  CHECK((next.menu == Combo{Ctrl | Alt, 'K'}) && next.chat == full.chat);
  CHECK(parse("shortcuts chat=Shift+F12", full, next));
  CHECK(next.menu == full.menu && (next.chat == Combo{Shift, 123}));
  CHECK(parse("shortcuts chat=Shift+Enter menu=Shift+Tab", full, next) && next == full);
  for (const char* bad : {"shortcuts", "shortcuts ", "shortcuts menu=", "shortcuts chat=", "shortcuts other=Shift+K",
      "shortcuts menu=Shift+K chat=Unknown", "shortcuts menu=Shift+Tab menu=Alt+K", "shortcuts chat=Shift+K CHAT=Alt+K",
      "shortcuts  menu=Shift+Tab", "shortcuts menu=Shift+Tab ", "shortcuts menu=Shift+Tab garbage",
      "shortcuts menu=Shift+Tab\tchat=Shift+Enter", "shortcuts menu=Shift+Tab=", "shortcuts menu=Shift+Tab\n"}) {
    next = full;
    CHECK(!parse(bad, full, next));
    CHECK(next == full);  // Invalid suffix cannot apply a valid prefix.
  }
  CHECK(!parse(std::string("shortcuts menu=Shift+K") + std::string(129, ' '), full, next));
  CHECK(!parse(std::string("shortcuts menu=Shift+K\0", 23), full, next));
}
static void command_dispatch() {
  Commands input;
  std::vector<std::string> logs;
  auto log = [&](const char* line) { logs.emplace_back(line); };
  CHECK(input.snapshot() == Config{});
  CHECK(!input.take_settings());
  CHECK(input.dispatch("settings", log));
  CHECK(input.take_settings() && !input.take_settings());
  CHECK(input.dispatch("settings", log) && input.dispatch("settings", log));
  CHECK(input.take_settings() && !input.take_settings()); // F1's coalescing semantics.
  CHECK(!input.dispatch("settings junk", log) && !input.take_settings());
  CHECK(!input.dispatch("rect 0 0 640 480", log));
  CHECK(input.dispatch("shortcuts chat=Shift+Enter", log));
  const auto good = input.snapshot();
  CHECK((good.chat == Combo{Shift, 13}) && (good.menu == Combo{Shift, 9}));
  for (int i = 0; i < 3; ++i) CHECK(input.dispatch("shortcuts chat=Shift+Enter", log));
  CHECK(input.snapshot() == good && logs.empty());
  for (const char* bad : {"shortcuts menu=Shift+K chat=Bad", "shortcuts", "shortcuts unknown=Shift+Tab", "shortcuts\tmenu=Shift+Tab"}) {
    CHECK(input.dispatch(bad, log));
    CHECK(input.snapshot() == good);
  }
  CHECK(logs == std::vector<std::string>{"embed: ignored malformed shortcuts command"});
  CHECK(input.dispatch("shortcuts menu=Ctrl+K", log));
  CHECK(input.snapshot().chat == good.chat && (input.snapshot().menu == Combo{Ctrl, 'K'}));
}
static void lifecycle() {
  // No stdin line: today's Shift+Tab menu, plain Tab passes to the practice reservation, no chat.
  Keys keys;
  expect(keys.event(false, 9, true, Shift, false), false);
  CHECK(!keys.swallows(false, 9));
  expect(keys.event(true, 13, true, Shift, false), false);
  expect(keys.event(true, 9, true, 0, false), false);
  expect(keys.event(true, 9, false, 0, false), false);
  expect(keys.event(true, 9, true, Shift, false), true, kDown);
  expect(keys.event(true, 9, true, Shift, true), true);
  expect(keys.event(true, 16, false, 0, false), false, kUp); // First modifier release.
  CHECK(keys.swallows(true, 9));
  expect(keys.event(true, 9, true, 0, true), true);
  expect(keys.event(true, 9, false, 0, true), true);
  expect(keys.event(true, 9, true, Shift, false), true, kDown);
  expect(keys.cancel(), false, kCancel);
  expect(keys.event(true, 9, true, 0, false), false); // A lost release must not steal a new plain Tab.
  expect(keys.event(true, 9, false, 0, false), false);
  expect(keys.event(true, 9, false, 0, true), false);
  CHECK(!keys.swallows(true, 9));
  expect(keys.event(true, 9, true, Shift, false), true, kDown);
  expect(keys.event(true, 9, false, Shift, true), true, kUp); // Primary release first.
  expect(keys.event(true, 16, false, 0, true), false);
  expect(keys.event(true, 9, true, Shift, true), false); // Stray repeat never starts a chord.
  expect(keys.event(true, 9, false, 0, true), false);
  for (int modifier : {16, 160, 161, 17, 162, 163, 18, 164, 165}) {
    Keys test;
    expect(test.configure(configured("shortcuts menu=Shift+Ctrl+Alt+K")), false);
    expect(test.event(true, 'K', true, 7, false), true, kDown);
    expect(test.event(true, modifier, false, 7, true), false, kUp);
    expect(test.event(true, 'K', false, 0, true), true);
  }
  // Two simultaneous chords; their releases and cancellation are independent.
  keys = Keys{};
  const auto full = configured("shortcuts menu=Ctrl+Alt+K chat=Shift+Enter");
  expect(keys.configure(full), false);
  CHECK(!keys.swallows(true, 'K')); // Modifiers alone cannot reserve an unpressed primary.
  CHECK(!keys.swallows(true, 13));
  CHECK(!keys.swallows(true, 'A'));
  expect(keys.event(true, 'K', true, 7, false), true, kDown);
  expect(keys.event(true, 13, true, 7, false), true, nullptr, kChatDown);
  expect(keys.event(true, 13, true, 7, true), true);
  expect(keys.configure(full), false); // Repeated stdin config cannot interrupt a hold.
  expect(keys.event(true, 16, false, Ctrl | Alt, true), false, nullptr, kChatUp);
  expect(keys.event(true, 'K', false, Ctrl | Alt, true), true, kUp);
  expect(keys.event(true, 13, false, 0, true), true);
  expect(keys.event(true, 'K', true, 7, false), true, kDown);
  expect(keys.event(true, 13, true, 7, false), true, nullptr, kChatDown);
  expect(keys.cancel(), false, kCancel, kChatCancel); // All focus/close/destroy notifications.
  expect(keys.cancel(), false);
  expect(keys.event(true, 'K', true, 0, true), true);
  expect(keys.event(true, 'K', false, 0, true), true);
  expect(keys.event(true, 13, false, 0, true), true);
  expect(keys.event(true, 'K', true, Ctrl | Alt, false), true, kDown); // Lost key-up recovery.
  expect(keys.configure(configured("shortcuts menu=Shift+Tab chat=Shift+Enter")), false, kCancel);
  CHECK(keys.swallows(true, 'K')); // Remapping cannot leak the old held key.
  expect(keys.event(true, 'K', false, 0, true), true);
  expect(keys.event(true, 9, true, Shift, false), true, kDown);
  expect(keys.event(true, 9, false, 0, true), true, kUp);
  // Sharing a primary key remains deterministic; both app-selected chords fire.
  keys = Keys{};
  keys.configure(configured("shortcuts menu=Shift+K chat=Ctrl+K"));
  expect(keys.event(true, 'K', true, Shift | Ctrl, false), true, kDown, kChatDown);
  expect(keys.event(true, 'K', false, Shift | Ctrl, true), true, kUp, kChatUp);
#ifdef YOUGAME_AGENT_TEST_HOOK
  keys = Keys{};
  CHECK(keys.menu().script_down() == kDown);
  expect(keys.event(true, 16, false, 0, false), false); // Script duration remains independent.
  CHECK(keys.menu().script_up() == kUp);
#endif
}
// The shared pre-ImGui handler writes this same cache in window.cpp. Model only
// the later ordinary-key/Tab dispatch here; wiring tests verify that placement.
struct WindowKeyboard {
  Keys shortcuts;
  bool physical[256]{};
  Keys::Result message(int vk, bool down, unsigned mods, bool repeat = false,
                       bool system = false, bool imgui = false, bool stdio = true) {
    const auto result = shortcuts.window_event(physical, stdio, vk, down, system, mods, repeat);
    if (!result.swallowed && !system && vk == 9) physical[vk] = false; // Plain Tab reservation.
    else if (!result.swallowed && !imgui && !system && down) physical[vk] = true;
    return result;
  }
  bool game(int vk) const { return physical[vk] && !shortcuts.swallows(true, vk); }
};
static void window_cache_ordering() {
  // Shift was cached by WM_KEYDOWN. Its WM_SYSKEYUP must clear it even if
  // ImGui consumes the release, in either order of primary/modifier releases.
  for (bool imgui : {false, true}) for (bool primary_first : {false, true}) {
    WindowKeyboard window;
    window.shortcuts.configure(configured("shortcuts menu=Shift+Alt+K"));
    expect(window.message(16, true, Shift), false);
    CHECK(window.physical[16] && window.game(16)); // Shift-alone walking remains available.
    expect(window.message(18, true, Shift | Alt, false, true), false);
    CHECK(!window.physical[18]); // System presses retain their old treatment.
    expect(window.message('K', true, Shift | Alt, false, true), true, kDown);
    CHECK(!window.physical['K'] && !window.game('K'));
    if (primary_first) expect(window.message('K', false, Shift | Alt, true, true, imgui), true, kUp);
    expect(window.message(16, false, Alt, true, true, imgui), false, primary_first ? nullptr : kUp);
    CHECK(!window.physical[16] && !window.game(16));
    if (!primary_first) expect(window.message('K', false, Alt, true, true, imgui), true);
    expect(window.message(18, false, 0, true, true, imgui), false);
    CHECK(!window.physical['K'] && !window.physical[18]);
  }
  // The primary preceded Ctrl: neither Ctrl nor a K repeat may hide or erase
  // its held C-stick input, and no shortcut event may be emitted.
  for (bool primary_first : {false, true}) {
    WindowKeyboard window;
    window.shortcuts.configure(configured("shortcuts menu=Ctrl+K"));
    expect(window.message('K', true, 0), false);
    CHECK(window.physical['K'] && window.game('K'));
    expect(window.message(17, true, Ctrl), false);
    CHECK(window.physical['K'] && window.game('K'));
    expect(window.message('K', true, Ctrl, true), false);
    CHECK(window.physical['K'] && window.game('K'));
    if (primary_first) expect(window.message('K', false, Ctrl, true), false);
    expect(window.message(17, false, 0, true), false);
    CHECK(window.physical['K'] == !primary_first && window.game('K') == !primary_first);
    if (!primary_first) expect(window.message('K', false, 0, true), false);
    CHECK(!window.physical['K'] && !window.game('K'));
  }
  // Ctrl preceded the primary: keep the physical press, hide it from game
  // readers through modifier release/repeats, then clear it on primary release.
  WindowKeyboard window;
  window.shortcuts.configure(configured("shortcuts menu=Ctrl+K"));
  expect(window.message(17, true, Ctrl), false);
  expect(window.message('K', true, Ctrl), true, kDown);
  CHECK(window.physical['K'] && !window.game('K'));
  expect(window.message(17, false, 0, true, false, true), false, kUp);
  CHECK(!window.physical[17] && window.physical['K'] && !window.game('K'));
  expect(window.message('K', true, 0, true), true);
  CHECK(window.physical['K'] && !window.game('K'));
  expect(window.message('K', false, 0, true, false, true), true);
  CHECK(!window.physical['K'] && !window.game('K'));
  expect(window.message('K', true, 0), false);
  CHECK(window.physical['K'] && window.game('K'));
  // Cache clearing is independent of the bridge and ImGui; unswallowed presses
  // consumed by ImGui stay out of the cache as before.
  expect(window.message('K', false, 0, true, true, true, false), false);
  CHECK(!window.physical['K']);
  expect(window.message('K', true, 0, false, false, true), false);
  CHECK(!window.physical['K']);
  WindowKeyboard defaults;
  expect(defaults.message(16, true, Shift), false);
  CHECK(defaults.game(16));
  expect(defaults.message(9, true, Shift), true, kDown);
  CHECK(defaults.physical[9] && !defaults.game(9));
  expect(defaults.message(9, false, Shift, true), true, kUp);
  expect(defaults.message(9, true, 0), false);
  CHECK(!defaults.physical[9] && !defaults.game(9)); // Plain Tab remains reserved.
}
int main() {
  parser(); command_dispatch(); lifecycle(); window_cache_ordering();
  CHECK(std::string(kCaps).find("\"chat\",\"settings\"") != std::string::npos);
  CHECK(std::string(kToplevelCaps).find("\"chat\",\"settings\"") != std::string::npos);
  CHECK(std::string(kSettingsHint) == "F1: game settings");
  std::printf("YouGame shortcuts passed: %d checks\n", checks);
}
