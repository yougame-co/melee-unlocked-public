// No SDL/AppKit initialization, WindowServer calls, game or sockets.
#include "embed_mac_state.h"
#include "embed_mac_pixels.h"
#include "embed_mac_png.h"
#include "../runtime/hle/yougame_parity.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <condition_variable>
#include <thread>
using namespace mac_embed;
namespace {
int checks = 0;
void check(bool ok, const char* text) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
void screenshots() {
  using namespace mac_embed::shot;
  check(parse("screenshot 1") == 1, "first screenshot id");
  check(parse("screenshot 9007199254740991") == max_id, "maximum JavaScript id");
  for (auto line : {"screenshot", "screenshot ", "screenshot 0", "screenshot 01", "screenshot -1", "screenshot +1",
                    "screenshot 1 ", "screenshot  1", "screenshot 1\n", "screenshot 1\r", "screenshot 1x",
                    "screenshot 1.0", "embed screenshot 1", "screenshot 9007199254740992", "screenshot 18446744073709551616"})
    check(!parse(line), "invalid screenshot id rejected");
  check(!parse(std::string("screenshot 1\0junk", 17)), "embedded NUL rejected");
  std::vector<std::string> replies;
  auto emit = [&](const std::string& line) { replies.push_back(line); };
  Slot slot;
  check(!slot.begin(0, 0, emit) && !slot.begin(max_id + 1, 0, emit), "invalid ids never own slot");
  auto first = slot.begin(3, 0, emit);
  check(first && slot.requested() && !slot.begin(4, 0, emit), "second pending request busy");
  check(slot.take() == first->ticket && !slot.requested() && !slot.take() && !slot.begin(4, 0, emit),
        "GPU and encoding keep slot busy; take exactly once");
  slot.finish(first->ticket, 1, "done", true, emit);
  auto second = slot.begin(4, 2, emit);
  check(second && slot.take() == second->ticket, "completion releases slot");
  slot.finish(second->ticket, 3, "done", true, emit);
  // Fake-clock deadline, including a GPU-claimed request with no further presents.
  for (bool claimed : {false, true}) {
    Slot timed;
    replies.clear();
    auto old = timed.begin(31, 100, emit);
    if (claimed) check(timed.take() == old->ticket, "deadline fixture claims GPU request");
    timed.expire(old->ticket, 1599, emit);
    check(replies.empty(), "deadline is not early");
    timed.expire(old->ticket, 1600, emit);
    timed.expire(old->ticket, 1601, emit);
    check(replies == std::vector<std::string>{error(31, "failed")} && old->cancelled.load() && !timed.requested(),
          "1500ms failure once without presents; clears requested flag");
    auto fresh = timed.begin(31, 1602, emit); // deliberately reuse the protocol id
    check(fresh && fresh->ticket != old->ticket, "deadline frees slot with a new generation");
    timed.finish(old->ticket, 1603, "late-success", true, emit);
    timed.expire(old->ticket, 9999, emit);
    check(replies.size() == 1 && timed.take() == fresh->ticket, "late completion/timer cannot reply or clear newer slot");
    timed.finish(fresh->ticket, 1604, "fresh-success", true, emit);
    check(replies.back() == "fresh-success" && replies.size() == 2, "new request completes normally");
  }
  {
    Slot delayed_timer;
    replies.clear();
    auto request = delayed_timer.begin(32, 0, emit);
    delayed_timer.finish(request->ticket, 1500, "too-late", true, emit);
    check(replies == std::vector<std::string>{error(32, "failed")}, "completion enforces deadline if timer is delayed");
  }
  check(error(4, "busy") == "yg {\"type\":\"screenshot\",\"id\":4,\"error\":\"busy\"}", "busy reply shape");
  for (auto why : {"refused", "failed"}) check(error(4, why).find(quote(why)) != std::string::npos, "error vocabulary");
#ifdef __APPLE__
  check(std::string(yougame::parity::caps(false)).find("\"screenshot\"") != std::string::npos, "Apple advertises screenshot");
#endif
  check(path("/run/data", 123) == "/run/data/shot-123.png", "exact spelling and direct child");
  for (auto dir : {"", "relative", "/run/../data", "/run/./data", "/run//data", "/run/data/"})
    check(path(dir, 1).empty(), "nonabsolute/noncanonical spelling refused");
  check(path(std::string("/data\0else", 10), 1).empty(), "NUL path refused");
  check(!size_ok(0, 1) && !size_ok(8193, 1) && !size_ok(4001, 4000), "hard dimension and pixel limits");
  check(size_ok(8192, 1) && size_ok(4000, 4000), "inclusive dimension/pixel limits");
  check(!source_ok(32769, 1) && !source_ok(8001, 8000), "readback allocation budget");
  // Four distinct corners. Metal is BGRA/top-down with padded rows; GL is RGBA/bottom-up.
  const uint8_t metal[] = {0,0,255,255, 0,255,0,255, 99,99,99,99,
                           255,0,0,255, 255,255,255,255, 88,88,88,88};
  const uint8_t gl[] = {0,0,255,255, 255,255,255,255, 255,0,0,255, 0,255,0,255};
  const std::vector<uint8_t> expected{255,0,0, 0,255,0, 0,0,255, 255,255,255};
  std::vector<uint8_t> rgb, other;
  auto size = shot::rgb(rgb, metal, 2, 2, 12, true, false);
  auto gl_size = shot::rgb(other, gl, 2, 2, 8, false, true);
  check(rgb == expected && other == expected && size.w == 2 && gl_size.h == 2, "upright RGB corners match both renderers");
  auto png = encode(rgb, size);
  auto encoded = png_size(png);
  check(encoded && encoded->w == 2 && encoded->h == 2, "real encoded PNG IHDR size");
  check(success(12, "/data/shot-12.png", png) == "yg {\"type\":\"screenshot\",\"id\":12,\"path\":\"/data/shot-12.png\",\"w\":2,\"h\":2}", "success reply uses encoded size");
  check(quote("/a\"b\\c\n") == "\"/a\\\"b\\\\c\\u000a\"", "JSON path quoting keeps one control line");
  check(success(1, std::string(4096, 'x'), png).empty(), "oversize reply refused");
  check(success(1, std::string(4000, '\n'), png).empty(), "escaped reply limit counts encoded bytes");
  auto invalid = png; invalid[19] = 3;
  check(png_size(invalid)->w == 3 && success(1, "/a", invalid).find("\"w\":3") != std::string::npos, "metadata is read from IHDR, never source dimensions");
  invalid[0] = 0; check(!png_size(invalid), "bad signature refused");
  invalid = png; invalid[15] = 'X'; check(!png_size(invalid), "missing IHDR refused");
  invalid = png; invalid[16] = 1; check(!png_size(invalid), "oversize IHDR refused");
  invalid = png; invalid.resize(max_png + 1); check(!png_size(invalid), "PNG byte limit enforced");
  std::vector<uint8_t> wide(size_t(8200) * 4, 52), limited;
  size = shot::rgb(limited, wide.data(), 8200, 1, 8200 * 4, true, false);
  check(size.w == 8192 && size.h == 1 && limited.size() == 8192 * 3, "production worker conversion uses Retina cap");
  auto capped = encode(limited, size);
  check(png_size(capped)->w == 8192 && success(8, "/a", capped).find("\"w\":8192") != std::string::npos, "capped encoded/reply dimensions agree");
  // Under the run's own TMPDIR (check_embed.py isolates it), never a path relative to the checkout.
  const char* tmp_root = std::getenv("TMPDIR");
  std::string folder_template = std::string(tmp_root && *tmp_root ? tmp_root : "/tmp") + "/embed-shot-XXXXXX";
  std::vector<char> folder_buffer(folder_template.begin(), folder_template.end()); folder_buffer.push_back(0);
  char* folder = folder_buffer.data();
  check(mkdtemp(folder) != nullptr, "isolated fixture directory");
  char* canonical = realpath(folder, nullptr); check(canonical != nullptr, "absolute fixture root");
  std::string dir = canonical; std::free(canonical);
  const auto file = path(dir, 12);
  check(write(dir, 12, png) == nullptr, "exclusive PNG create and complete close");
  struct stat st{}; check(!lstat(file.c_str(), &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 && size_t(st.st_size) == png.size(), "regular direct child, one hard link, complete size");
  int fd = open(file.c_str(), O_RDONLY | O_NOFOLLOW);
  std::vector<uint8_t> actual(png.size());
  check(fd >= 0 && read(fd, actual.data(), actual.size()) == ssize_t(actual.size()) && actual == png, "file bytes match encoded PNG"); close(fd);
  check(std::string(write(dir, 12, png)) == "refused", "existing regular file refused");
  const auto link = path(dir, 13);
  check(!symlink(file.c_str(), link.c_str()), "fixture symlink created");
  check(std::string(write(dir, 13, png)) == "refused", "existing symlink refused without following");
  check(!lstat(file.c_str(), &st) && size_t(st.st_size) == png.size(), "symlink target unchanged");
  const auto hard = path(dir, 14);
  check(!::link(file.c_str(), hard.c_str()), "fixture hard link created");
  check(std::string(write(dir, 14, png)) == "refused", "existing hard link refused");
  const auto alias = dir + "-alias";
  check(!symlink(dir.c_str(), alias.c_str()), "directory alias fixture created");
  check(std::string(write(alias, 15, png)) == "refused", "symlink directory refused even though absolute");
  check(std::string(write(dir + "/../escape", 16, png)) == "refused", "traversal refused");
  check(std::string(write(dir, 17, invalid)) == "refused" && access(path(dir,17).c_str(), F_OK), "invalid/oversized PNG creates nothing");
  // Shutdown before GPU submission, and a reader holding a copied callback.
  {
    Slot closing;
    replies.clear();
    auto request = closing.begin(21, 0, emit);
    auto copied_reader = [&] { return closing.begin(22, 1, emit); };
    closing.stop_accepting(); closing.cancel(emit);
    check(!copied_reader() && !closing.take() && !closing.requested(), "shutdown atomically rejects copied reader/claim");
    bool queued = false;
    closing.work(request->ticket, [&](auto) { queued = true; });
    check(!queued && replies == std::vector<std::string>{error(21, "failed")}, "pending shutdown settles once and cannot enqueue");
    check(std::string(request->output.write(dir, 21, png)) == "failed" && access(path(dir,21).c_str(), F_OK),
          "pending shutdown cannot create a later file");
  }
  // Pause a real writer after it has created and partially written its own file.
  // Shutdown must remove it BEFORE that writer resumes, including the timeout case.
  {
    Slot closing;
    replies.clear();
    auto request = closing.begin(23, 0, emit);
    check(closing.take() == request->ticket, "in-flight shutdown request claimed");
    std::mutex gate;
    std::condition_variable ready;
    bool paused = false, resume = false;
    const char* result = nullptr;
    std::thread writer([&] {
      result = request->output.write(dir, 23, png, [&](int output, const uint8_t* bytes, size_t count) -> ssize_t {
        const auto n = ::write(output, bytes, std::min(count, size_t(8)));
        std::unique_lock<std::mutex> lock(gate);
        paused = true; ready.notify_one();
        ready.wait(lock, [&] { return resume; });
        return n;
      });
    });
    {
      std::unique_lock<std::mutex> lock(gate);
      ready.wait(lock, [&] { return paused; });
    }
    check(!lstat(path(dir,23).c_str(), &st) && st.st_size == 8, "fixture holds an actual partial PNG");
    closing.stop_accepting(); closing.cancel(emit);
    check(access(path(dir,23).c_str(), F_OK) && replies == std::vector<std::string>{error(23, "failed")},
          "shutdown removes partial file while writer still in flight");
    { std::lock_guard<std::mutex> lock(gate); resume = true; } ready.notify_one();
    writer.join();
    closing.finish(request->ticket, 1, "late-shutdown-success", true, emit);
    check(result && std::string(result) == "failed" && access(path(dir,23).c_str(), F_OK) && replies.size() == 1,
          "late writer cannot recreate file or reply after shutdown");
  }
  // The same exclusive owner cleans up failure and completion/deadline races.
  {
    Output failed;
    check(std::string(failed.write(dir, 24, png, [](int, const uint8_t*, size_t) -> ssize_t { errno = EIO; return -1; })) == "failed" &&
          access(path(dir,24).c_str(), F_OK), "write failure removes exclusive partial file");
    Slot timed;
    replies.clear();
    auto request = timed.begin(25, 0, emit);
    check(!request->output.write(dir, 25, png), "deadline fixture closes output before reply");
    timed.expire(request->ticket, 1500, emit);
    check(access(path(dir,25).c_str(), F_OK) && replies.size() == 1, "deadline removes even a closed file awaiting completion");
  }
  for (const auto input : {PixelSize{1,1}, {640,480}, {8192,1}, {8200,1}, {8000,8000},
                           {32768,1}, {1,32768}, {16384,3906}, {32768,32768}, {0,1}, {-1,3}}) {
    const auto out = readback_size(input.w, input.h);
    if (!source_ok(input.w, input.h)) check(!out.w && !out.h, "GL invalid source rejected before allocation");
    else {
      const auto expected = capture_size(input.w, input.h, true);
      check(out.w == expected.w && out.h == expected.h && size_ok(out.w, out.h) &&
            size_t(out.w) * out.h * 4 <= 64000000, "GL PBO size uses capped dimensions and <=64MB");
    }
  }
  check(readback_size(8000,8000).w == 4000 && readback_size(8000,8000).h == 4000,
        "64MP GL frame becomes 16MP before readback");
  unlink(alias.c_str()); unlink(hard.c_str()); unlink(link.c_str()); unlink(file.c_str()); rmdir(dir.c_str());
}
void math() {
  struct Case { Rect parent, box, cocoa; double primary, backing_scale; };
  const Case cases[] = {
    {{100, 200, 1000, 700}, {10, 58, 640, 480}, {110, 342, 640, 480}, 1080, 1},
    {{100, 200, 1000, 700}, {10, 58, 640, 480}, {110, 342, 640, 480}, 1080, 2},
    {{-1500, 20, 1000, 700}, {0, 28, 960, 640}, {-1500, 392, 960, 640}, 1080, 2},
    {{1800, -900, 1200, 900}, {25, 0, 800, 600}, {1825, 1380, 800, 600}, 1080, 1},
    {{-50, 1180, 1000, 700}, {10, 28, 640, 480}, {-40, -608, 640, 480}, 1080, 2},
    {{10.5, 20.25, 900, 600}, {3, 28, 800, 500}, {13.5, 351.75, 800, 500}, 900, 2},
  };
  for (const auto& c : cases) {
    auto frame = screen_rect(c.parent, c.box, c.primary);
    check(equal(frame, c.cocoa), "top-left/points/negative-display/titlebar conversion");
    check(frame.w * c.backing_scale == c.box.w * c.backing_scale,
          "Retina backing size changes, placement stays in points");
  }
  // The app's box is in backing pixels: 2x display, theater and the in-window full screen (10-26 melee.log).
  check(equal(points_from_backing({142, 240, 2326, 1383}, 2), {71, 120, 1163, 691.5}), "theater box: pixels to points at 2x");
  check(equal(points_from_backing({142, 0, 3314, 2122}, 2), {71, 0, 1657, 1061}), "full-window box: pixels to points at 2x");
  check(equal(points_from_backing({10, 58, 640, 480}, 1), {10, 58, 640, 480}), "1x display: unchanged");
  check(equal(points_from_backing({10, 58, 640, 480}, 0), {10, 58, 640, 480}), "unknown scale counts as 1");
  check(equal(screen_rect({0, 33, 1728, 1084}, points_from_backing({142, 240, 2326, 1383}, 2), 1117),
              {71, 272.5, 1163, 691.5}), "2x theater panel sits inside the parent, at the box's size in points");
  check(overlap_area({0, 0, 100, 100}, {50, 50, 100, 100}) == 2500, "overlap area");
  check(overlap_area({0, 0, 100, 100}, {100, 0, 100, 100}) == 0, "touching displays do not overlap");
  check(intersects({-100, 0, 200, 100}, {0, 0, 1000, 1000}), "partial offscreen");
  check(!intersects({-200, 0, 200, 100}, {0, 0, 1000, 1000}), "fully offscreen");
  check(!intersects({0, 0, 0, 100}, {0, 0, 1000, 1000}), "empty box");
}
void commands() {
  check(parent_id("12345") == 12345, "CGWindowID decimal");
  for (auto value : {"", "0", "-1", "+1", "1 ", " 1", "0x42", "4294967296", "1x"})
    check(!parent_id(value), "invalid CGWindowID");
  for (auto line : {"rect 10 28 640 480", "rect -20 -3 960 640", "rect 0 0 0 0", "view -20 -3 960 640", "view 0 0 0 0",
                    "show", "hide", "focus", "blur", "quit", "scale 2", "scale 1.5", "scale 0.5", "scale 8", "scale 2.000"})
    check(bool(parse_command(line)), "valid command");
  check(parse_command("scale 2")->kind == Kind::Scale && parse_command("scale 2")->scale == 2, "scale value");
  check(parse_command("scale 1.25")->scale == 1.25, "fractional scale");
  for (auto line : {"scale", "scale ", "scale 0.49", "scale 8.01", "scale 0", "scale -2", "scale 2 x", "scale 2x",
                    "scale nan", "scale inf", "scale 1e9", "scales 2", "scale 2\nquit", "embed scale 2"})
    check(!parse_command(line), "malformed scale rejected");
  for (auto line : {"rect 1 2 3", "rect 1 2 3 4 tail", "rect 1 2 -3 4", "rect 1 2 3 -4",
                    "rect 1.5 2 3 4", "rect 2147483648 0 32 32", "rect 0 0 100001 50",
                    "rect 0 0 3 4\nquit", "embed show", "volume 0", "show ", "quitter", ""})
    check(!parse_command(line), "malformed/unsupported command rejected");
  for (auto line : {"view 1 2 3", "view 1 2 3 4 tail", "view 1 2 -3 4", "view 1.5 2 3 4",
                    "view 2147483648 0 32 32", "view 0 0 100001 50", "view 0 0 3 4\nquit"})
    check(!parse_command(line), "view uses rect validation");
  check(parse_command("view 10 20 800 600")->kind == Kind::View, "view command kind");
  Presentation clipping;
  clipping.first_frame();
  clipping.apply(*parse_command("rect 110 80 600 400"));
  check(equal(clipping.picture(), clipping.box), "legacy rect is picture");
  clipping.apply(*parse_command("view 10 20 800 600"));
  const auto fixed = clipping.picture();
  for (auto line : {"rect 210 80 500 400", "rect -50 -50 1000 1000", "rect 0 0 0 0"}) {
    clipping.apply(*parse_command(line));
    check(equal(clipping.picture(), fixed), "rect never changes drawable with view");
    for (double scale : {1.0, 1.5, 2.0}) {
      const auto points = points_from_backing(clipping.picture(), scale);
      const auto pixels = backing_size(points.w, points.h, scale);
      check(pixels.w == 800 && pixels.h == 600, "backing size independent of clip at each scale");
    }
  }
  check(clipping.visible_box().w == 0, "empty rect clips everything");
  clipping.apply(*parse_command("rect -50 -50 1000 1000"));
  check(equal(clipping.visible_box(), fixed), "rect outside picture clamped");
  check(contains_point(clipping.visible_box(), 10, 20) && !contains_point(clipping.visible_box(), 810, 20),
        "hit testing excludes the clipped edges");
  Presentation view_first;
  view_first.first_frame(); view_first.apply(*parse_command("view 10 20 800 600"));
  check(!view_first.visible(Parent{true, true}), "view before rect remains hidden");
  view_first.apply(*parse_command("rect 110 80 600 400"));
  check(equal(view_first.picture(), fixed), "view before rect also keeps picture size");
  Presentation p;
  Parent parent{true, true, {100, 100, 800, 600}, 123};
  check(!p.visible(parent), "starts hidden");
  p.apply(*parse_command("rect 0 28 640 480"));
  check(!p.visible(parent), "rect without first frame hidden");
  p.first_frame(); check(p.visible(parent), "first frame plus rect shown");
  p.apply(*parse_command("hide")); check(!p.visible(parent), "hide immediate");
  p.apply(*parse_command("show")); check(p.visible(parent), "show restores");
  p.apply(*parse_command("rect 0 0 1 1"));
  p.apply(*parse_command("show")); check(!p.visible(parent), "tiny rect stays hidden through show");
  p.apply(*parse_command("rect 0 0 32 32")); check(p.visible(parent), "minimum rect restores");
  parent.on_screen = false; check(!p.visible(parent), "minimized/other Space hidden");
  parent.on_screen = true;
  p.apply(*parse_command("blur")); check(!p.focused && p.visible(parent), "blur preserves picture");
  p.apply(*parse_command("focus")); check(p.focused, "focus");
  check(p.app_scale == 0, "no app scale until one arrives");
  p.apply(*parse_command("scale 2")); check(p.app_scale == 2 && p.visible(parent), "app scale kept, picture unchanged");
  p.apply(*parse_command("scale 1")); check(p.app_scale == 1, "app scale follows a display change");
  p.apply(*parse_command("quit")); check(p.quit && !p.visible(parent), "quit");
  Presentation frame_first;
  frame_first.first_frame(); check(!frame_first.visible(parent), "first frame without rect hidden");
}
void retina_pixels() {
  struct Case { int w, h; PixelBox at1, at2; int efb1, efb2; };
  const Case cases[] = {
    {1163, 692, {120, 0, 923, 692}, {240, 0, 1845, 1384}, 2, 3}, // rounded theater panel
    {1643, 1061, {114, 0, 1415, 1061}, {228, 0, 2829, 2122}, 3, 5}, // full screen
    {1001, 733, {12, 0, 977, 733}, {23, 0, 1955, 1466}, 2, 3}, // odd spare pixel
    {801, 601, {0, 0, 801, 601}, {0, 0, 1602, 1202}, 2, 3},
    {1921, 721, {480, 0, 961, 721}, {959, 0, 1923, 1442}, 2, 3}, // wide theater
    {641, 1001, {0, 260, 641, 481}, {0, 520, 1282, 962}, 2, 4}, // top/bottom bars
  };
  for (const auto& c : cases) for (int scale : {1, 2}) {
    const auto pixels = backing_size(c.w, c.h, scale);
    check(pixels.w == c.w * scale && pixels.h == c.h * scale, "panel surface uses backing pixels");
    const auto b = letterbox(pixels.w, pixels.h, 4.0 / 3.0);
    const auto expected = scale == 1 ? c.at1 : c.at2;
    check(b.x == expected.x && b.y == expected.y && b.w == expected.w && b.h == expected.h,
          "theater/fullscreen/odd-size exact GL letterbox");
    check(b.x >= 0 && b.y >= 0 && b.x + b.w <= pixels.w && b.y + b.h <= pixels.h,
          "picture fits entire Retina drawable, no half-size lower-left math");
    const int metal_y = pixels.h - b.y - b.h;
    check(std::abs(metal_y - b.y) <= 1, "Metal flip differs only by odd spare pixel");
    check(efb_scale(0, pixels.h) == (scale == 1 ? c.efb1 : c.efb2), "auto EFB follows backing height");
    check(efb_scale(2, pixels.h) == 2, "pinned EFB stays pinned at both display scales");
    for (double ui : {1.0, 1.5, 2.0}) {
      const auto logical = ui_size(pixels.w, pixels.h, ui);
      check(int(logical.w * logical.framebuffer_x) == pixels.w && int(logical.h * logical.framebuffer_y) == pixels.h,
            "ImGui's float product and truncation produce the entire drawable at odd sizes");
      const double click = point_to_ui(c.w * 0.375, pixels.w, c.w, logical.framebuffer_x);
      check(std::abs(click * logical.framebuffer_x - pixels.w * 0.375) < 1e-8,
            "rounded logical UI maps a click to its exact backing pixel");
      const double point = c.w * 0.375;
      const double mapped = point_to_ui(point, pixels.w, c.w, ui);
      check(std::abs(mapped * ui - point * scale) < 1e-9, "mouse/touch point hits same framebuffer pixel after UI scaling");
      const double factor = point_to_ui(1, pixels.w, c.w, ui);
      check(std::abs(mapped / factor - point) < 1e-9, "ImGui pointer warp reverses SDL event/poll mapping");
    }
  }
  check(backing_size(1163, 691.5, 2).h == 1383, "odd backing theater height before AppKit point rounding");
  check(backing_size(801, 601, 0).w == 801, "unknown backing scale uses 1x");
  check(efb_scale(0, 528) == 1 && efb_scale(0, 529) == 2, "auto ceil threshold");
  check(efb_scale(0, 1061, 2) == 6 && efb_scale(2, 2122, 2) == 4, "existing GL SSAA multiplier and cap");
  check(efb_scale(0, 692) == 2 && efb_scale(0, 1384) == 3 && efb_scale(0, 692) == 2,
        "2x-to-1x display move lowers auto EFB again");
  for (int w = 631; w < 3500; w += 17) {
    const auto logical = ui_size(w, w + 71, (w + 71) / 1080.0);
    check(int(logical.w * logical.framebuffer_x) == w && int(logical.h * logical.framebuffer_y) == w + 71,
          "fractional UI scales retain every pixel across representative odd sizes");
  }
}
void retina_off_compatibility() {
  // No settings key and an unset environment: this is the production activation predicate.
  const bool enabled = retina_enabled(false, nullptr);
  check(!enabled, "missing retina key and unset environment default OFF");
  int snapshot_reads = 0;
  check(!query_retina_size(enabled, [&] { ++snapshot_reads; return true; }),
        "OFF query reports inactive even without a snapshot");
  check(snapshot_reads == 0, "OFF query never accesses the snapshot or its mutex");
  check(query_retina_size(true, [&] { ++snapshot_reads; return true; }) && snapshot_reads == 1,
        "ON query reads the snapshot");
  check(!query_retina_size(true, [] { return false; }), "ON query respects an invalid fallback snapshot");
  check(retina_enabled(true, nullptr) && retina_enabled(false, "1"), "both explicit opt-ins work");
  for (auto value : {"", "0", "true", "2", "1 "})
    check(!retina_enabled(false, value), "environment enables only exact 1");
  struct Case { int w, h; PixelBox box; int efb; };
  const Case cases[] = {
    {1163, 692, {120, 0, 923, 692}, 2}, // theater
    {1643, 1061, {114, 0, 1415, 1061}, 3}, // full screen
  };
  for (const auto& c : cases) for (int backing : {1, 2}) {
    const auto retina = backing_size(c.w, c.h, backing);
    int presenter_w = c.w, presenter_h = c.h, gl_w = c.w, gl_h = c.h;
    check(!override_size(enabled, retina, &presenter_w, &presenter_h), "OFF presenter override declined");
    check(!override_size(enabled, retina, &gl_w, &gl_h), "OFF GL override declined");
    check(presenter_w == c.w && presenter_h == c.h && gl_w == c.w && gl_h == c.h,
          "OFF presenter and GL surface preserve pre-change SDL sizes at 1x/2x");
    const auto box = letterbox(presenter_w, presenter_h, 4.0 / 3.0);
    check(box.x == c.box.x && box.y == c.box.y && box.w == c.box.w && box.h == c.box.h,
          "OFF letterbox equals pre-change theater/fullscreen values at 1x/2x");
    check(efb_scale(0, presenter_h) == c.efb, "OFF auto EFB equals pre-change values at 1x/2x");
    for (float ui : {1.0f, 1.5f, 2.0f}) {
      // Independent copy of the host's pre-6c07548 arithmetic.
      const float old_w = ui == 1 ? float(c.w) : std::ceil(float(c.w) / ui - 0.001f);
      const float old_h = ui == 1 ? float(c.h) : std::ceil(float(c.h) / ui - 0.001f);
      const auto logical = display_size(enabled, c.w, c.h, ui);
      check(logical.w == old_w && logical.h == old_h &&
            logical.framebuffer_x == ui && logical.framebuffer_y == ui,
            "OFF ImGui display/framebuffer dimensions equal pre-change arithmetic at 1x/2x");
    }
  }
}
void retina_fallback_scale() {
  // Shared by GL and Metal. No SDL access on ordinary launches or after the one fallback update.
  for (int ssaa : {1, 2}) {
    EmbedScaleState state;
    int sdl_reads = 0, w = 2326, h = 1384;
    const auto sdl_size = [&](int& w, int& h) { ++sdl_reads; w = 1280; h = 720; };
    check(!state.update(false, w, h, sdl_size) && sdl_reads == 0,
          "ordinary OFF/standalone EFB update does no SDL work");
    check(state.update(true, w, h, sdl_size) && sdl_reads == 0,
          "embedded EFB update uses only backing dimensions");
    check(efb_scale(0, h, ssaa) == 3 * ssaa, "1384 px starts at auto EFB x3 (GL SSAA preserved)");
    w = h = 0; // The snapshot was invalidated by parent loss.
    check(state.update(false, w, h, sdl_size) && sdl_reads == 1 && w == 1280 && h == 720,
          "fallback consumes SDL point dimensions once");
    check(efb_scale(0, h, ssaa) == 2 * ssaa, "720 pt fallback recomputes auto EFB x2");
    check(efb_scale(2, h, ssaa) == 2 * ssaa, "fallback preserves pinned EFB and SSAA");
    check(!state.update(false, w, h, sdl_size) && sdl_reads == 1,
          "later regular-window frames return to the old OFF path");
    // A fallback before the first frame must also work; constructors seed the state.
    state.active = true;
    check(!state.update(false, w, h, [](int& w, int& h) { w = h = 0; }) && state.active,
          "invalid SDL dimensions keep the fallback pending");
    check(state.update(false, w, h, sdl_size) && !state.active && h == 720,
          "fallback retries once SDL dimensions become valid");
  }
}
void capture_limits() {
  struct Case { int w, h, out_w, out_h; };
  const Case cases[] = {
    {3286, 2122, 3286, 2122}, // 1643x1061 points at 2x: full pixels
    {6016, 3384, 5333, 3000}, // 3008x1692 points at 2x: 20.36 MP -> <16 MP
    {8200, 1000, 8192, 999}, // side cap, below the area cap
    {1000, 8200, 999, 8192}, // tall counterpart
    {8192, 1953, 8192, 1953}, // just below both caps
    {4000, 4000, 4000, 4000}, // exactly 16 million pixels
  };
  for (const auto& c : cases) {
    const auto output = capture_size(c.w, c.h, true);
    check(output.w == c.out_w && output.h == c.out_h, "capture fixture exact output dimensions");
    check(output.w <= 8192 && output.h <= 8192 && int64_t(output.w) * output.h <= 16000000,
          "capture respects BOTH desktop side and area limits");
    const auto off = capture_size(c.w, c.h, false);
    check(off.w == c.w && off.h == c.h, "OFF capture keeps pre-change full pixels even over cap");
    check(std::abs(double(output.w) / c.w - double(output.h) / c.h) <=
          1.0 / std::min(c.w, c.h), "capture aspect ratio differs by at most rounding one pixel");
    std::printf("Capture: %dx%d -> %dx%d (%lld pixels), nearest-neighbor\n",
                c.w, c.h, output.w, output.h, (long long)output.w * output.h);
  }
  // Exercise real resampling without allocating a 20 MP frame in the window-free test.
  std::vector<uint8_t> rgb(size_t(8200) * 3);
  for (int x = 0; x < 8200; ++x) for (int c = 0; c < 3; ++c) rgb[size_t(x) * 3 + c] = uint8_t(x + c);
  const auto old = rgb;
  auto output = limit_capture(rgb, 8200, 1, false);
  check(rgb == old && output.w == 8200, "OFF image bytes untouched");
  output = limit_capture(rgb, 8200, 1, true);
  check(output.w == 8192 && output.h == 1 && rgb.size() == 8192 * 3, "resized payload matches encoder dimensions");
  // Pin known source samples around the first dropped column, midpoint and edges.
  struct Sample { int out, source; };
  for (auto s : {Sample{0, 0}, {511, 511}, {512, 513}, {4095, 4099}, {4096, 4100}, {8191, 8199}})
    check(std::equal(rgb.begin() + s.out * 3, rgb.begin() + (s.out + 1) * 3, old.begin() + s.source * 3),
          "nearest RGB samples preserve channels at edges and dropped-column boundaries");
  std::vector<uint8_t> below(31 * 17 * 3, 79);
  const auto original = below;
  output = limit_capture(below, 31, 17, true);
  check(output.w == 31 && output.h == 17 && below == original, "below-limit image keeps every pixel and byte");
}
void timing() {
  QueryTimer t(100);
  check(t.due(0), "first query");
  t.request(); check(!t.due(1) && !t.due(19), "20ms forced floor");
  check(t.due(20), "deferred force remains pending");
  check(!t.due(119) && t.due(120), "normal 100ms cache resumes");
  StartupBackoff backoff;
  for (auto delay : {20, 50, 100, 200, 200, 200}) check(backoff.next() == delay, "startup backoff");
  Tracker startup;
  std::vector<Millis> attempts;
  for (Millis now = 0; now <= 2200; ++now)
    startup.tick(now, [&] { attempts.push_back(now); return Parent{}; });
  check(attempts == std::vector<Millis>({0,20,70,170,370,570,770,970,1170,1370,1570,1770,1970,2170}),
        "startup fake clock exact attempts; no per-frame queries");
  check(startup.fallback(), "startup absence falls back after grace plus bounded retry");
  Tracker late_parent;
  for (Millis now = 0; now < 2170; ++now)
    late_parent.tick(now, [&] { return Parent{now >= 1999, now >= 1999, {0,0,900,700}, 88}; });
  check(!late_parent.fallback(), "grace decision cannot use stale cached absence");
  late_parent.tick(2170, [] { return Parent{true,true,{0,0,900,700},88}; });
  check(late_parent.parent().exists && !late_parent.fallback(), "late startup parent survives");

  struct FakeWindowList {
    bool exists = true, on_screen = true;
    int queries = 0;
    Parent query() { ++queries; return {exists, on_screen, {100,200,900,700}, on_screen ? 77 : 0}; }
  } windows;
  Tracker tracker;
  int ordering = 0;
  for (Millis now = 0; now < 1000; ++now) {
    tracker.tick(now, [&] { return windows.query(); });
    if (tracker.parent().on_screen && tracker.ordering_due(now)) ++ordering;
  }
  check(windows.queries == 10 && ordering == 4, "steady 10 parent + 4 ordering queries/second");
  windows.on_screen = false;
  for (Millis now = 1000; now < 5000; ++now) tracker.tick(now, [&] { return windows.query(); });
  check(tracker.parent().exists && !tracker.fallback(), "minimize/other Space never parent loss");
  check(tracker.parent().owner == 77, "hidden parent retains owner for activation refresh");
  windows.on_screen = true;
  tracker.owner_activated(); tracker.tick(5000, [&] { return windows.query(); });
  check(tracker.parent().on_screen && tracker.ordering_due(5000), "owner activation refresh");
  check(!tracker.ordering_due(5001), "activation ordering not repeated each frame");
  windows.exists = false; windows.on_screen = false;
  tracker.tick(5100, [&] { return windows.query(); });
  tracker.tick(6999, [&] { return windows.query(); });
  check(!tracker.fallback(), "parent absent just under grace");
  windows.exists = true; windows.on_screen = true;
  tracker.refresh(); tracker.tick(7019, [&] { return windows.query(); });
  check(!tracker.fallback(), "parent returns at force floor before decision");
  windows.exists = false; windows.on_screen = false;
  tracker.tick(7220, [&] { return windows.query(); });
  tracker.tick(9220, [&] { return windows.query(); });
  check(tracker.fallback(), "permanent loss fallback after grace");

  Tracker forced;
  int queries = 0, above = 0;
  for (Millis now = 0; now < 1000; ++now) {
    forced.refresh();
    forced.tick(now, [&] { ++queries; return Parent{true,true,{0,0,800,600},77}; });
    if (forced.ordering_due(now)) ++above;
  }
  check(queries == 50 && above == 50, "command flood max 50+50 queries/second");
  SettleGate settle;
  Rect bounds{100,200,900,700};
  settle.observe(0, true, bounds);
  check(!settle.ready(499) && settle.ready(500), "500ms settle before order-in");
  bounds.x += 1; settle.observe(501, true, bounds);
  check(!settle.ready(1000) && settle.ready(1001), "moving parent restarts settle");
  settle.observe(1002, false, bounds);
  check(!settle.ready(5000), "hidden parent cannot accrue stable time");
}
} // namespace
int main() {
  screenshots(); math(); commands(); timing(); retina_pixels(); retina_off_compatibility(); retina_fallback_scale(); capture_limits();
  std::printf("Mac embed: %d checks passed; steady 14 queries/s; forced <=100 queries/s; no windows\n", checks);
}
