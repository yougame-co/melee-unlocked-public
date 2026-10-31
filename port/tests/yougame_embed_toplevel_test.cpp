// Synthetic embed policy checks: no HWND, game, disc, or process is opened.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_embed_toplevel.h"
#include "yougame_box_present.h"
#include "yougame_parity.h"
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <string>
#include <vector>

namespace {
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace yougame::embed_toplevel_policy;
std::vector<std::string> lines;
void write_line(const char* line) { lines.emplace_back(line); }
void window_styles() {
  CHECK(!enabled(false, "1"));
  CHECK(enabled(true, "1"));
  for (const char* flag : {"", "0", "true", "01", "1 "}) CHECK(!enabled(true, flag));
  for (auto backend : {Backend::D3D11, Backend::D3D12, Backend::OpenGL})
    for (bool flip : {false, true}) {
      const auto standalone = styles(false, flip, backend, 0x00cf0000u, 0x40000u);
      CHECK(standalone.style == 0x00cf0000u && standalone.exstyle == 0x40000u);
      const auto child = styles(false, flip, backend, popup);
      CHECK(child.style == popup && child.exstyle == 0);
      const auto top = styles(true, flip, backend, 0x00cf0000u);
      CHECK(top.style == popup);
      CHECK((top.style & 0x40000000u) == 0); // no WS_CHILD
      CHECK(top.exstyle == (toolwindow | (flip && backend != Backend::OpenGL ? no_redirection : 0)));
    }
}
void geometry() {
  yg_embed_box box{};
  CHECK(rect("rect -3840 -2160 2560 1440", true, box));
  CHECK(box.x == -3840 && box.y == -2160 && box.w == 2560 && box.h == 1440);
  CHECK(!rect("rect -3840 -2160 2560 1440", false, box));
  CHECK(rect("rect 100 200 640 480", false, box));
  auto child = yg_embed_view_init(1280, 960, 1);
  yg_embed_view_update(&child, {80, 120, 800, 600}, true);
  const auto clipped = yg_embed_view_update(&child, box, false);
  CHECK(clipped.window.x == 80 && clipped.window.y == 120);
  CHECK(clipped.clip.x == 20 && clipped.clip.y == 80);
  // Toplevel preserves screen origins and caps dimensions before allocation.
  CHECK(rect("rect 100000 -100000 80000 70000", true, box));
  CHECK(first(pack(box.x, box.y)) == 100000 && second(pack(box.x, box.y)) == -100000);
  CHECK(first(pack(box.w, box.h)) == 16384 && second(pack(box.w, box.h)) == 16384);
  CHECK(rect("rect 0 0 16384 16383", true, box));
  CHECK(box.w == 16384 && box.h == 16383);
  CHECK(rect("rect 0 0 1 2147483647", true, box));
  CHECK(box.w == 1 && box.h == 16384);
  CHECK(rect("rect 0 0 2147483647 1", true, box));
  CHECK(box.w == 16384 && box.h == 1);
  CHECK(rect("rect 0 0 80000 70000", false, box));
  CHECK(box.w == 80000 && box.h == 70000);
  for (int x : {INT_MIN, -65536, -1, 0, 65536, INT_MAX})
    for (int y : {INT_MIN, -1, 0, INT_MAX}) {
      CHECK(first(pack(x, y)) == x);
      CHECK(second(pack(x, y)) == y);
    }
  CHECK(rect("rect -2147483648 2147483647 1 1", true, box));
  for (const char* line : {"rect 2147483648 0 1 1", "rect -2147483649 0 1 1",
       "rect 0 0 0 1", "rect 0 0 1 -1", "rect 0 0 1 1 junk", "rect 0 0 1",
       "rect 0 0 1x 1", "view 0 0 1 1", "", "rect 0 0 1 9999999999999999999"})
    CHECK(!rect(line, true, box));
}
void capabilities() {
  using namespace yougame::parity;
  CHECK(std::string(caps(false)) == kCaps);
  CHECK(std::string(caps(false)).find("toplevel") == std::string::npos);
  CHECK(std::string(caps(true)) == "yg {\"caps\":[\"ready\",\"overlay\",\"join\",\"notice\",\"pads\",\"rumble\",\"view\",\"volume\",\"toplevel\"]}");
  Signals top;
  top.start(false, write_line, true);
  CHECK(lines.empty());
  top.start(true, write_line, true);
  top.start(true, write_line, false);
  top.presented(true);
  CHECK(lines == (std::vector<std::string>{kToplevelCaps, kReady}));
  lines.clear();
  Signals child;
  child.start(true, write_line);
  CHECK(lines == (std::vector<std::string>{kCaps}));
}
void present() {
  using yougame::box_present::choose;
  for (bool vsync : {false, true}) for (bool fullscreen : {false, true})
    for (bool agent : {false, true}) for (bool flip : {false, true})
      for (bool paced : {false, true}) for (bool tearing : {false, true})
        for (bool vrr : {false, true}) {
          const auto top = choose(true, vsync, fullscreen, agent, flip, paced, tearing, vrr, true);
          const auto standalone = choose(false, vsync, fullscreen, agent, flip, paced, tearing, vrr);
          CHECK(top.interval == standalone.interval && top.flags == standalone.flags);
          CHECK(!top.compositor && !top.agent);
          CHECK(top.interval == (vsync || vrr ? 1u : 0u));
          CHECK(top.flags == (!vsync && !vrr && tearing ? yougame::box_present::allow_tearing : 0u));
          const auto child = choose(true, vsync, fullscreen, agent, flip, paced, tearing, vrr);
          CHECK(child.compositor == (vsync && !fullscreen && flip && agent && paced));
          CHECK(child.agent == child.compositor);
          CHECK(child.interval == (child.compositor ? 0u : vsync || vrr ? 1u : 0u));
        }
}
void focus() {
  CHECK(app_pid("1234") == 1234);
  CHECK(app_pid("4294967294") == 4294967294u);
  for (const char* pid : {"", "0", "-1", "+1", " 123", "123 ", "12x", "1.0",
       "4294967295", "4294967296", "99999999999999999999999999999"}) CHECK(app_pid(pid) == 0);
  CHECK(preferred_app_pid(1234, 5678) == 1234);
  CHECK(preferred_app_pid(1234, 1234) == 1234);
  CHECK(preferred_app_pid(1234, 0) == 1234);
  CHECK(preferred_app_pid(0, 5678) == 5678);
  CHECK(preferred_app_pid(0, 0) == 0);
  CHECK(preferred_app_pid(UINT32_MAX, 5678) == 5678);
  CHECK(preferred_app_pid(UINT32_MAX, 0) == 0);
  for (bool foreground : {false, true}) for (bool focused : {false, true})
    CHECK(emit_focus_return(foreground, focused) == (foreground || focused));
  CHECK(std::string(kFocusReturn) == "yg {\"focusReturn\":1}");
  CHECK(hwnd_line(0x1A2B3C) == "yg {\"toplevelHwnd\":1715004}");
  CHECK(focus_return_message(1234) == "embed: toplevel focus return pid=1234");
  CHECK(focus_return_message(0) == "embed: toplevel focus return pid=none");
  FocusReturn state;
  CHECK(state.changed());
  CHECK(!state.changed());
  state.focused();
  CHECK(state.changed());
  CHECK(!state.changed());
}
}
int main() {
  window_styles(); geometry(); capabilities(); present(); focus();
  std::printf("yougame embed toplevel: %d checks passed\n", checks);
}
