// An easter egg on every character select: the D-pad code (Up Up Down Down Left Right Left Right, then
// B A or A B) sends a dog's face bouncing around the picture like the old DVD screensaver: B A is
// Bailey, A B is Walter. Each code sends one more, from any port, up to 200 at once (then the oldest
// leaves for each new one). Most launches never meet a corner; about one in five is aimed so that after
// a few bounces it lands exactly in one, which gets a flash, a burst of spin and a menu sound. Each
// face spins like a disc: freely in the air, and walls grip it toward rolling along them. Leaving the
// character select sends them all away. A hand that presses A on one takes hold: moving it stretches
// the face from that spot, and letting go slings it, spinning by where it was held. Tilting the
// C-stick turns the character select's camera; the faces then turn with it, as cards at their own
// depths in its 3D scene.
//
// Local drawing only: the detector reads the pads and changes nothing in them, nothing is sent to
// an opponent, and nothing is drawn in a match or a replay. The pure parts (the code detector and the
// bouncer) are here so the tests can run them without a window; walter_egg.cpp draws.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstdint>

namespace host { struct PadState; }

namespace walter_egg {

// ---------------------------------------------------------------- the game's side (walter_egg.cpp)
// Game thread, every pad poll: watches for the code (reads only; the pads are not changed).
void watch_pads(const host::PadState pads[4]);
// Game thread, every character select frame (the CSS frame command): marks a character select as
// on screen and returns a menu sound to play (0 none).
uint8_t css_frame();
// Game thread: a character select hand moved (the shim's hand command; any character select, any
// port), in tenths of the game's own units. A hand on him with A held takes hold and pulls.
void set_hand(int port, int16_t x10, int16_t y10);
// Game thread, every character select frame: the menu camera the C-stick turns (shim mu_menu_camera):
// the eye before the turn, the eye after, what it looks at, its up (three floats each) and its
// vertical field of view in degrees; nullptr when the frame came without one. Turned, the faces turn
// with the picture as cards in its 3D scene; untilted they are drawn flat, as always.
void set_camera(const float* camera13);
// He is out and a character select is on screen.
bool active();
struct Frame { float game_x0 = 0, game_y0 = 0, game_x1 = 0, game_y1 = 0; double time = 0; };
// Drawing thread, inside ImGui: draws him over the game picture while active().
void draw(const Frame& f);

// ---------------------------------------------------------------- the code
// GameCube pad bits (window.cpp's PAD_* values).
constexpr uint16_t kDL = 0x0001, kDR = 0x0002, kDD = 0x0004, kDU = 0x0008, kB = 0x0200, kA = 0x0100;
constexpr uint16_t kWatched = kDL | kDR | kDD | kDU | kA | kB;
constexpr uint16_t kSequence[8] = {kDU, kDU, kDD, kDD, kDL, kDR, kDL, kDR};   // then B A, or A B
constexpr int kSequenceLength = 8;
constexpr int kMaxGapPolls = 90;   // a pause longer than 1.5 s between two presses starts over
constexpr int kBailey = 1, kWalter = 2;   // what feed() answers: the code ended B A, or A B

// One port. Presses are rising edges of the six watched buttons; other buttons are ignored, a
// held button counts once, two watched buttons going down together are not part of the code.
struct Detector {
  uint16_t last = 0;
  int step = 0;        // how much of the code has been entered (8: the D-pad part; 9: and the first of B, A)
  uint16_t tail = 0;   // step 9: which of B and A came first
  int idle = 0;        // polls since the last watched press
  // One pad poll. On the poll that completes a code: kBailey (ended B A) or kWalter (ended A B); else 0.
  int feed(uint16_t buttons) {
    const uint16_t now = buttons & kWatched;
    const uint16_t pressed = (uint16_t)(now & ~last);
    last = now;
    if (!pressed) {
      if (step > 0 && ++idle > kMaxGapPolls) { step = 0; idle = 0; }
      return 0;
    }
    idle = 0;
    if (pressed & (pressed - 1)) { step = 0; return 0; }
    if (step == kSequenceLength + 1) {
      // The last press: the other one of B and A.
      const bool done = pressed == (tail == kB ? kA : kB);
      const int which = tail == kB ? kBailey : kWalter;
      step = !done && pressed == kSequence[0] ? 1 : 0;
      return done ? which : 0;
    }
    if (step == kSequenceLength && (pressed == kA || pressed == kB)) { tail = pressed; ++step; return 0; }
    if (step < kSequenceLength && pressed == kSequence[step]) { ++step; return 0; }
    // A wrong press: keep the longest end of what was entered (this press included) that is a
    // start of the code, so Up Up Up Down Down ... still counts.
    int keep = 0;
    for (int len = step < kSequenceLength ? step : kSequenceLength; len >= 1; --len) {
      bool ok = kSequence[len - 1] == pressed;
      for (int i = 0; ok && i < len - 1; ++i) ok = kSequence[i] == kSequence[step - (len - 1) + i];
      if (ok) { keep = len; break; }
    }
    step = keep;
    return 0;
  }
};

// ---------------------------------------------------------------- the bouncer
// Walter's top-left corner travels in [0, w] x [0, h] (the picture's size minus his own), in
// straight lines, reflecting off the four edges. Positions are closed-form in the step count, so a
// launch aimed at a corner reaches it exactly, and the drawing can use a fractional step.
inline double tri(double u, double span) {   // the reflected coordinate of an unfolded one
  if (span <= 0) return 0;
  double m = std::fmod(u, 2.0 * span);
  if (m < 0) m += 2.0 * span;
  return m <= span ? m : 2.0 * span - m;
}
inline long long wall_index(double u, double span) {   // which wall segment the unfolded coordinate is in
  return (long long)std::floor(u / span + 1e-9);
}

// A deterministic generator for the launches (the tests seed it).
struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
  double unit() { return (next() & 0xFFFFFF) / double(0x1000000); }
};

struct Launch {
  double w = 0, h = 0;          // travel range
  double x0 = 0, y0 = 0;        // unfolded start
  double vx = 0, vy = 0;        // per step, both positive (mirroring gives the other directions)
  bool mirror_x = false, mirror_y = false;
  long long corner_step = -1;   // aimed: the step he reaches the corner; -1 unaimed
  double x(double t) const { const double v = tri(x0 + vx * t, w); return mirror_x ? w - v : v; }
  double y(double t) const { const double v = tri(y0 + vy * t, h); return mirror_y ? h - v : v; }
  // Walls met on the way from step t - 1 to step t: bit 0 a side wall, bit 1 the top or bottom.
  int hits(long long t) const {
    if (t <= 0) return 0;
    int r = 0;
    if (wall_index(x0 + vx * t, w) != wall_index(x0 + vx * (t - 1), w)) r |= 1;
    if (wall_index(y0 + vy * t, h) != wall_index(y0 + vy * (t - 1), h)) r |= 2;
    return r;
  }
};
// A corner is both walls met on the same step.
inline bool corner(const Launch& l, long long t) { return l.hits(t) == 3; }

// speed: pixels per step along the path. Aimed launches meet a corner after 2 to 4 side walls.
inline Launch launch(double w, double h, double speed, bool aimed, Rng& rng, double start_x = -1, double start_y = -1) {
  Launch l;
  l.w = w > 1 ? w : 1; l.h = h > 1 ? h : 1;
  l.mirror_x = rng.next() & 1; l.mirror_y = rng.next() & 1;
  const double sx = start_x >= 0 ? (l.mirror_x ? l.w - start_x : start_x) : l.w * (0.15 + 0.7 * rng.unit());
  const double sy = start_y >= 0 ? (l.mirror_y ? l.h - start_y : start_y) : l.h * (0.15 + 0.7 * rng.unit());
  for (int attempt = 0; attempt < 400; ++attempt) {
    const double angle = (28.0 + 34.0 * rng.unit()) * 3.14159265358979 / 180.0;
    l.vx = speed * std::cos(angle); l.vy = speed * std::sin(angle);
    l.x0 = sx; l.y0 = sy;
    if (aimed) {
      // Side wall number k (unfolded x = k * w) at a whole step T, and the y start chosen so the
      // top or bottom is met on that same step.
      const int k = 2 + (int)(rng.next() % 3);
      const long long T = (long long)std::llround((k * l.w - l.x0) / l.vx);
      if (T < 30) continue;
      l.vx = (k * l.w - l.x0) / (double)T;
      const long long ky = (long long)std::floor(l.vy * T / l.h) + 1;
      l.y0 = ky * l.h - l.vy * (double)T;
      if (l.y0 < 0.05 * l.h || l.y0 > 0.95 * l.h) continue;
      // nudge the exact landing onto the walls (the floors in hits() then flip on step T)
      l.corner_step = T;
      if (corner(l, T)) return l;
      continue;
    }
    // Unaimed: keep only a path that meets no corner for a long time.
    bool clean = true;
    for (long long t = 1; t <= 12000 && clean; ++t) clean = !corner(l, t);
    if (clean) return l;
  }
  l.corner_step = -1;
  return l;
}

}  // namespace walter_egg
