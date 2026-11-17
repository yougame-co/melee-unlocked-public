// The character select easter egg's pure parts (gx/walter_egg.h): the D-pad code from any order of
// presses, and the bouncer at every screen shape.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "walter_egg.h"
#include <cstdio>
#include <cmath>
#include <utility>
#include <vector>

using namespace walter_egg;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// Feeds presses (each held 3 polls, released 3 polls); returns how many times the code completed.
static int feed(Detector& d, const std::vector<uint16_t>& presses, int gap = 3) {
  int done = 0;
  for (uint16_t b : presses) {
    for (int i = 0; i < 3; ++i) done += d.feed(b);
    for (int i = 0; i < gap; ++i) done += d.feed(0);
  }
  return done;
}
static const std::vector<uint16_t> kCode = {kDU, kDU, kDD, kDD, kDL, kDR, kDL, kDR, kB, kA};

static void test_detector() {
  { Detector d; CHECK(feed(d, kCode) == 1); }
  { Detector d; CHECK(feed(d, kCode) == 1); CHECK(feed(d, kCode) == 1); }                   // again: the second toggle
  { Detector d; std::vector<uint16_t> v = {kDU}; v.insert(v.end(), kCode.begin(), kCode.end()); CHECK(feed(d, v) == 1); }  // Up Up Up ...
  { Detector d; std::vector<uint16_t> v = {kDU, kDU, kDD, kDU, kDU, kDD, kDD, kDL, kDR, kDL, kDR, kB, kA}; CHECK(feed(d, v) == 1); }
  { Detector d; std::vector<uint16_t> v = kCode; v[4] = kDR; CHECK(feed(d, v) == 0); }        // wrong order
  { Detector d; std::vector<uint16_t> v = kCode; std::swap(v[8], v[9]); CHECK(feed(d, v) == kWalter); }   // A then B: the other face
  { Detector d; CHECK(feed(d, kCode) == kBailey); }                                                       // B then A
  { Detector d; std::vector<uint16_t> v = kCode; v[9] = kB; CHECK(feed(d, v) == 0); }                     // B B
  { Detector d; std::vector<uint16_t> v = kCode; v[8] = kA; CHECK(feed(d, v) == 0); }                     // A A
  { // B, then a wrong press, then A: not the code; the code right after still counts
    Detector d; std::vector<uint16_t> v = kCode; v.insert(v.begin() + 9, kDL); CHECK(feed(d, v) == 0); CHECK(feed(d, kCode) == kBailey);
  }
  { // B, then Up starts the code over: Up (kept) Up Down Down Left Right Left Right A B
    Detector d; std::vector<uint16_t> v(kCode.begin(), kCode.begin() + 9);
    const std::vector<uint16_t> rest = {kDU, kDU, kDD, kDD, kDL, kDR, kDL, kDR, kA, kB};
    v.insert(v.end(), rest.begin(), rest.end()); CHECK(feed(d, v) == kWalter);
  }
  { // both codes, one after the other
    Detector d; std::vector<uint16_t> w = kCode; std::swap(w[8], w[9]);
    CHECK(feed(d, kCode) == kBailey); CHECK(feed(d, w) == kWalter); CHECK(feed(d, w) == kWalter);
  }
  { Detector d; std::vector<uint16_t> v = kCode; v.pop_back(); CHECK(feed(d, v) == 0); }       // unfinished
  { // a pause too long in the middle starts over
    Detector d; int done = feed(d, std::vector<uint16_t>(kCode.begin(), kCode.begin() + 5));
    for (int i = 0; i < kMaxGapPolls + 5; ++i) done += d.feed(0);
    done += feed(d, std::vector<uint16_t>(kCode.begin() + 5, kCode.end()));
    CHECK(done == 0);
  }
  { // a pause just under the limit still counts
    Detector d; int done = feed(d, kCode, kMaxGapPolls - 4); CHECK(done == 1);   // 89 polls from press to press
  }
  { // a held D-pad counts once: Up held for 40 polls is one Up
    Detector d; int done = 0;
    for (int i = 0; i < 40; ++i) done += d.feed(kDU);
    done += d.feed(0);
    done += feed(d, std::vector<uint16_t>(kCode.begin() + 2, kCode.end()));   // the rest without the second Up
    CHECK(done == 0);
  }
  { // other buttons in between are ignored (X, Y, Start, L, R, Z, the stick)
    Detector d; int done = 0;
    for (uint16_t b : kCode) { done += d.feed(0x0400); done += d.feed(0); done += d.feed(b); done += d.feed(0); done += d.feed(0x1000); done += d.feed(0); }
    CHECK(done == 1);
  }
  { // two watched buttons at once are not part of the code
    Detector d; std::vector<uint16_t> v = kCode; v[9] = kA | kB; CHECK(feed(d, v) == 0);
  }
  { // held with another button still a rising edge of the new one: Down held while Up pressed again
    Detector d; std::vector<uint16_t> v = kCode; CHECK(feed(d, v) == 1);
  }
}

static void test_bouncer() {
  // screens: 720p to 1440p, 4:3, small, ultrawide; sprite 20% of the height
  const double screens[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {960, 720}, {640, 360}, {3440, 1440}, {320, 240}, {5120, 1440}};
  Rng rng; rng.s = 12345;
  int aimed_ok = 0, aimed_total = 0;
  for (auto& s : screens) {
    const double size = s[1] * 0.2, w = s[0] - size, h = s[1] - size, speed = s[1] / 0.8 / 60.0 * 0.22;
    for (int k = 0; k < 6; ++k) {
      // inside the screen at every step, for 100 000 steps (fractional steps too)
      Launch l = launch(w, h, speed, false, rng);
      CHECK(l.corner_step < 0);
      bool inside = true; int corners = 0;
      for (long long t = 0; t <= 100000; ++t) {
        const double x = l.x(t + 0.37), y = l.y(t + 0.37);
        if (x < -1e-6 || x > w + 1e-6 || y < -1e-6 || y > h + 1e-6) inside = false;
        if (t <= 10000 && corner(l, t)) ++corners;
      }
      CHECK(inside);
      CHECK(corners == 0);   // an unaimed launch never meets a corner in 10 000 steps
      // aimed: reaches the corner exactly on its promised step, both walls on that step
      Launch a = launch(w, h, speed, true, rng);
      ++aimed_total;
      if (a.corner_step > 0 && corner(a, a.corner_step)) {
        ++aimed_ok;
        const double x = a.x((double)a.corner_step), y = a.y((double)a.corner_step);
        CHECK((x < 1e-3 || x > w - 1e-3) && (y < 1e-3 || y > h - 1e-3));
        int walls = 0;
        for (long long t = 1; t < a.corner_step; ++t) walls += (a.hits(t) & 1) ? 1 : 0;
        CHECK(walls >= 1 && walls <= 4);   // after a few bounces
      }
      bool a_inside = true;
      for (long long t = 0; t <= 100000; t += 7) { const double x = a.x(t), y = a.y(t); if (x < -1e-6 || x > w + 1e-6 || y < -1e-6 || y > h + 1e-6) a_inside = false; }
      CHECK(a_inside);
    }
  }
  CHECK(aimed_ok == aimed_total);
  std::printf("aimed launches reaching their corner: %d/%d\n", aimed_ok, aimed_total);
  // a start given (a relaunch from where he is) is kept
  Launch r = launch(1000, 600, 4, false, rng, 250, 120);
  CHECK(std::abs(r.x(0) - 250) < 1e-6 && std::abs(r.y(0) - 120) < 1e-6);
}

int main() {
  test_detector();
  test_bouncer();
  std::printf(g_fail ? "walter egg: %d failures\n" : "walter egg: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
