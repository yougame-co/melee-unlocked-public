#include "yougame_friends.h"
#include "yougame_mode_policy.h"
#include "yougame_parity.h"
#include <cstdio>
#include <cstdlib>
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
int main() {
  using namespace yougame::friends;
  for (const char* source : {"relayed", "adapter", "keyboard mapped START"}) {
    (void)source;
    InviteEdge e;
    CHECK(e.poll(true, 0x1000, true, true, false));
    CHECK(!e.poll(true, 0x1000, true, true, false));
    CHECK(!e.poll(true, 0, true, true, false));
    CHECK(e.poll(true, 0x1000, true, true, false));
    CHECK(!e.poll(true, 0x1000, false, true, false));
  }
  InviteEdge e;
  CHECK(!e.poll(true, 0x100, true, true, false)); // A must still type on Direct
  CHECK(!e.poll(true, 0, false, false, true));
  CHECK(e.poll(true, 0x100, false, false, true));
  CHECK(!e.poll(true, 0x100, false, false, true));
  CHECK(!e.poll(false, 0x100, false, false, true));
  CHECK(!e.poll(true, 0x100, false, false, true));   // still held after focus/connection came back: no press
  CHECK(!e.poll(true, 0x100, false, false, true));
  CHECK(!e.poll(true, 0, false, false, true));       // released
  CHECK(e.poll(true, 0x100, false, false, true));    // a real press counts again
  CHECK(!e.poll(true, 0, false, false, true));
  CHECK(e.poll(true, 0x1000, false, false, true));
  CHECK(!e.poll(true, 0x1000, false, false, true));
  CHECK(!yougame::mode_policy::friends_offered(false));
  CHECK(yougame::mode_policy::friends_offered(true) == (YOUGAME_FRIENDS_ROW != 0));   // off in the main build
  CHECK(!yougame::mode_policy::yougame_ranked_offered);
  CHECK(version_reason({{"reason", "version"}}));
  for (const auto& j : {json(), json::array(), json{{"reason", 1}}, json{{"reason", "Version"}}, json{{"error", "version"}}})
    CHECK(!version_reason(j));
  Events events;
  events.online_menu(true, true);
  CHECK(events.picker_context());
  CHECK(events.poll_pad(3, true, 0x100));
  CHECK(events.trigger() == "pad 4 buttons 0100");
  CHECK(events.take_host());
  CHECK(!events.take_host());
  CHECK(!events.poll_pad(3, true, 0x100));
  CHECK(events.push({{"type", "notice"}, {"reason", "version"}}, 10));
  CHECK(events.toast(11) == kVersion);
  CHECK(events.report_version());
  CHECK(!events.report_version());
  CHECK(events.take_version());
  CHECK(!events.take_version());
  events.request_host();
  events.online_menu(false);
  CHECK(!events.take_host());
  CHECK(!events.online_menu() && !events.friend_entry() && !events.picker_context());
  CHECK(!events.poll_pad(0, true, 0x1100));
  CHECK(!events.take_host());

  // Match window.cpp's ownership: OverlayPad claims its chord before Friends sees
  // the remaining buttons; settings capture and a settings-opening edge are skipped.
  for (bool entry : {false, true}) {
    Events input;
    yougame::parity::OverlayPad overlay;
    if (entry) input.online_menu(true, true);
    else { input.code_screen(true); input.code_empty(true); }
    auto poll = [&](uint16_t buttons, bool opened = false, bool capture = false) {
      const auto event = overlay.poll(true, 0, true, buttons, 100);
      if (!opened && !capture) input.poll_pad(0, true, buttons);
      return event != nullptr;
    };
    CHECK(poll(0x1004));  // Down + START opens YouGame only
    CHECK(!input.take_host());
    CHECK(!poll(0x1000));  // releasing Down first still leaves START claimed
    CHECK(!input.take_host());
    CHECK(!poll(0));
    CHECK(!poll(0x1000));  // plain START still invites once
    CHECK(input.take_host());
    CHECK(!poll(0x1000));
    CHECK(!input.take_host());
    for (uint16_t buttons : {0x1000, 0x0100}) {
      poll(0);
      poll(buttons, true, false);
      CHECK(!input.take_host());
      poll(0);
      poll(buttons, false, true);
      CHECK(!input.take_host());
    }
  }
  std::printf("friends input: %d/%d checks passed\n", checks, checks);
}
