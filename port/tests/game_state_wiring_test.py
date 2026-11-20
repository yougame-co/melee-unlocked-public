"""The game tells YouGame playing / paused / menu (gameState, contract v1) so its button hides in play."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def body(text, marker):
    start = text.index("{", text.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]


def compiler():
    if os.environ.get("CXX"):
        return shlex.split(os.environ["CXX"])
    for name in ("c++", "g++", "clang++"):
        if path := shutil.which(name):
            return [path]
    return None


HARNESS = r'''
#include "yougame_game_state.h"
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
using namespace yougame::game_state;
static std::mutex m; static std::vector<std::string> sent; static std::atomic<uint64_t> epoch{0};
static void wait_for(size_t n) { for (int i = 0; i < 200; ++i) { { std::lock_guard<std::mutex> l(m); if (sent.size() >= n) return; } std::this_thread::sleep_for(std::chrono::milliseconds(5)); } }
static size_t count() { std::lock_guard<std::mutex> l(m); return sent.size(); }
int main() {
  if (classify(false, false, false) != kMenu || classify(true, false, false) != kPlaying ||
      classify(true, true, false) != kPaused || classify(true, false, true) != kMenu ||
      classify(false, true, false) != kMenu) return 10;
  Pending p;
  if (p.due(kNone, 1) || p.due(kPlaying, 0) || !p.due(kPlaying, 1)) return 11;
  p.done(kPlaying, 1);
  if (p.due(kPlaying, 1) || !p.due(kPaused, 1) || !p.due(kPlaying, 2)) return 12;
  Worker w;
  w.start([] { return epoch.load(); }, [](int s) { std::lock_guard<std::mutex> l(m); sent.push_back(name(s)); });
  w.set(kMenu);                                   // held: no connection yet (hello not answered)
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  if (count() != 0) return 13;
  epoch = 1; wait_for(1);                         // hello done: the held state goes
  if (count() != 1 || sent[0] != "menu") return 14;
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  if (count() != 1) return 15;                    // only on a change
  w.set(kPlaying); wait_for(2);
  w.set(kPaused); wait_for(3);
  w.set(kPlaying); wait_for(4);
  if (count() != 4 || sent[1] != "playing" || sent[2] != "paused" || sent[3] != "playing") return 16;
  epoch = 0; std::this_thread::sleep_for(std::chrono::milliseconds(120));
  epoch = 2; wait_for(5);                         // reconnect: the same state once more
  if (count() != 5 || sent[4] != "playing") return 17;
  w.stop();
  std::puts("ok");
  return 0;
}
'''


class GameStateWiring(unittest.TestCase):
    def test_channel_request_and_resend(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        send = body(link, "void yg_game_state(int state) {")
        self.assertIn('{{"type", "gameState"}, {"state", name}}', send)
        # The same narrow path as matchState: stdio or the local channel, never bridge()'s refusal policy.
        self.assertIn("bridge_stdio(body, 1000", send)
        self.assertIn("} else if (g_channel_allowed && g_channel_up) {", send)
        self.assertIn("bridge_channel(body, 1000);", send)
        # 122.20: one log line per state actually sent (stdio and channel), none when nothing is sent.
        self.assertEqual(send.count('host::log("yougame: gameState %s", name);'), 2)
        self.assertLess(send.index('host::log("yougame: gameState %s", name);'), send.index("bridge_stdio(body, 1000"))
        self.assertNotIn("bridge(body", send)
        # Every connection that comes up (only after its hello answered) gets a new epoch.
        self.assertIn("if (up) { g_channel_epoch.fetch_add(1); g_channel_up = true;", link)
        epoch = body(link, "uint64_t game_state_epoch() {")
        self.assertIn("g_channel_up ? g_channel_epoch.load() : 0", epoch)
        start = body(link, "void start() {")
        self.assertIn("game_state_worker().start(game_state_epoch, yg_game_state);", start)
        self.assertIn("game_state_worker().set(state);", body(link, "void report_game_state(int state) {"))

    def test_states_from_the_scene_every_retrace(self):
        practice = source("port/runtime/hle/native_practice.cpp")
        tick = body(practice, "\nvoid tick() {")
        self.assertTrue(tick.lstrip().startswith("report_game_state();"), "before every early return")
        # Every retrace in every mode: host::retrace() polls the Slippi options unconditionally, and the
        # Source Port's branch of that poll (host::game_image, offline Training/VS/1P/menus included)
        # ticks the practice bridge before it returns.
        retrace = body(source("port/runtime/host/host.cpp"), "\nvoid retrace() {")
        self.assertIn("slippi::poll_options();", retrace)
        poll = body(source("port/runtime/hle/exi_slippi.cpp"), "void poll_options() {")
        native = poll[poll.index("if (host::game_image) {"):poll.index("return;")]
        self.assertIn("native_practice::tick();", native)
        report = body(practice, "static void report_game_state() {")
        self.assertIn("int32_t a[5] = {-1, 0, 0, 0, 0};", report)   # a query: never pauses or resumes
        self.assertIn("match = a[4] != 0; paused = a[0] > 0 || a[3] != 0;", report)
        self.assertIn("overlay_menu_open.load(", report)
        # Static Recomp has no bridge: it reads gm_GetDbPauseFlag's byte itself, or it never says paused.
        self.assertIn("(host::rd8(0x80479D68u) & 0x7) != 0", report)
        shim = source("sourceport/game/shim/mu_yougame_pause.h")
        self.assertIn("a[3] = gm_GetDbPauseFlag(1) || gm_GetDbPauseFlag(2);", shim)
        self.assertIn("if (n >= 5) {", shim)
        settings = source("port/runtime/gx/pc_settings.cpp")
        self.assertIn("yougame::game_state::overlay_menu_open.store(state.open || state.menu_open || state.practice_open ||", settings)

    def test_build_number(self):
        self.assertEqual(source("YOUGAME_BUILD").strip(), "0.9.219-online-test.122.36")

    def test_worker_semantics(self):
        cxx = compiler()
        if not cxx:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "t.cpp", Path(tmp) / "t"
            src.write_text(HARNESS)
            subprocess.run(cxx + ["-std=c++17", "-pthread", "-I", str(ROOT / "port/runtime/hle"), str(src), "-o", str(exe)],
                           check=True)
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main()
