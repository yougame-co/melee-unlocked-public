// The character select easter egg (walter_egg.h): the state the game thread feeds and the drawing.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "walter_egg.h"
#include "walter_egg_picture.h"
#include "host.h"
#include "test_env.h"
#include "render_options.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "user_texture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

namespace walter_egg {
namespace {

// ---- game thread
Detector g_detectors[4];
std::atomic<uint32_t> g_css_retrace{0};    // the last retrace a character select frame ran (0 never)
std::atomic<bool> g_on{false};              // faces are out
std::atomic<uint32_t> g_sent[2];            // codes entered, ever: [0] Bailey (the code ended B A), [1] Walter (A B)
std::atomic<uint32_t> g_left_sent[2];       // g_sent when the character select was last left with faces out
std::atomic<uint32_t> g_visit{0};           // times the character select was left with faces out (they all go)
std::atomic<uint8_t> g_sound{0};            // a menu sound the game should play on its next CSS frame
std::atomic<uint32_t> g_hand[4];            // each port's character select hand: x10 << 16 | y10 (the game's units)
std::atomic<uint32_t> g_hand_at[4];         // the retrace it was last reported (0: not on this visit)
std::atomic<bool> g_a_held[4];              // each port's A button
uint32_t g_codes_this_visit = 0;

// The character select's camera (the game's menu camera, mn_8022BA1C): the eye before the C-stick
// turned it, the eye it turned to, what it looks at, its up and its vertical field of view.
struct Camera {
  bool ok = false;
  float eye0[3] = {0, 0, 0}, eye[3] = {0, 0, 0}, interest[3] = {0, 0, 0}, up[3] = {0, 1, 0};
  float fovy = 0;
};
std::mutex g_camera_mutex;
Camera g_camera;

bool css_visible_now() {
  const uint32_t last = g_css_retrace.load(std::memory_order_relaxed);
  return last != 0 && host::retrace_count() - last <= 4;
}

// ---- drawing thread
// A hand holding a face (A pressed with the hand on it): the spot it took hold of, in the face's
// own square (0..1), and how far it has pulled that spot since. Let go, the pull swings back and the
// face flies off the other way.
struct Grab {
  int port = -1;                    // the port holding on; -1 nobody
  float u = 0.5f, v = 0.5f;
  float hand_x0 = 0, hand_y0 = 0;   // where the hand was when it took hold (pixels)
  float pull_x = 0, pull_y = 0;     // pixels
  double released_at = -1;          // the swing back: since when, and from what pull
  float released_x = 0, released_y = 0;
};

constexpr double kStepsPerSecond = 60.0;   // a face's own pace along a path

// Its spin: a disc of the face's radius. In the air the spin keeps going and fades only slowly; a
// wall's friction pulls it toward rolling along that wall (the speed it slides along the wall over its
// radius), the way a ball skimming a wall picks up spin and a fast spinner slows; a corner adds a
// burst; a slingshot spins it by where it was held. Nothing ever turns it back upright.
constexpr double kSpinFade = 30.0;    // seconds for a spin to fade to a third in the air
constexpr double kWallGrip = 0.45;    // how much of the way to rolling one wall touch takes it
constexpr double kMaxSpin = 25.0;     // radians a second, about four turns (faster reads as backwards at 60 fps)
constexpr double kCornerSpin = 9.0;   // a corner's burst, radians a second
constexpr double kSlingSpin = 12.0;   // a full slingshot held at the rim, radians a second

// One face out. Each one flies, bounces, spins and can be held on its own.
struct Face {
  int which = 0;              // 0 Bailey, 1 Walter
  Launch path;
  double t = 0;               // steps along this path so far
  float box_w = 0, box_h = 0; // the travel range it was launched for
  long long last_step = 0;
  int bounces = 0;            // wall meetings so far (the outline's colour)
  double squash_x_at = -1, squash_y_at = -1;
  double pop_at = -1;         // a corner: when it popped
  double held = 0;            // seconds a hand has held it so far: its own clock stands still meanwhile
  double slung_at = -1;       // a slingshot: when and how hard (0..1)
  float slung_power = 0;
  double turn = 0;            // radians from upright, and radians a second (clockwise on screen)
  double spin = 0;
  bool slung = false;         // tests: a pinned face flies once it has been slung
  float depth = 0;            // the 3D view: 0 at the hands' depth .. 1 the nearest to the camera
  float rock_phase = 0;       // and its own rocking there
  Grab grab;
  Rng rng;
  // This frame: its own clock, size and where it is drawn (in the untilted picture).
  double now = 0;
  float size = 0, x = 0, y = 0, cx = 0, cy = 0, scale_x = 1, scale_y = 1, angle = 0;
};

// As many as the codes ask for, up to this many at once: then the oldest one leaves for each new one,
// so codes can be entered for as long as anyone likes and the picture never holds more than this.
constexpr size_t kMaxFaces = 200;
std::vector<Face> g_faces;
uint32_t g_seen_sent[2] = {0, 0};
uint32_t g_seen_visit = 0;
bool g_a_was[4] = {false, false, false, false};
double g_last_frame = 0;
double g_flash_at = -10, g_sound_at = -10;   // the last corner's flash and sound (frame time)
uint64_t g_serial = 0;
bool g_full_logged = false;
bool g_tilt_logged = false;

constexpr int kUpscale = 4;   // the texture is the 128 px picture enlarged smoothly, so it never shows its pixels
constexpr int kGlowPad = 48;  // the outline's texture: the face's shape with this much room around it (texture pixels)
struct Pictures { ImTextureData* face = nullptr; ImTextureData* glow = nullptr; };
Pictures g_pictures[2];
ImGuiContext* g_face_context[2] = {nullptr, nullptr};
ImGuiContext* g_glow_context[2] = {nullptr, nullptr};

// The compiled-in picture (run-length coded), enlarged kUpscale times with a Catmull-Rom filter on
// colour premultiplied by alpha: smooth at any window size, and the cut-out's edge takes no fringe.
// With it, the same shape blurred, in white: drawn in a bright colour behind the face it is its outline.
const Pictures& pictures(int which) {
  which = which == 1 ? 1 : 0;
  Pictures& p = g_pictures[which];
  if (p.face) {
    gx::keep_user_texture(p.face, g_face_context[which]);
    gx::keep_user_texture(p.glow, g_glow_context[which]);
    return p;
  }
  const uint32_t* runs = which == 1 ? kPictureWalter : kPictureBailey;
  const uint32_t run_count = which == 1 ? kPictureWalterRuns : kPictureBaileyRuns;
  const int n = kPictureSize, m = kPictureSize * kUpscale;
  std::vector<float> src((size_t)n * n * 4, 0.0f);   // premultiplied r g b, then a
  size_t at = 0;
  for (uint32_t i = 0; i + 1 < run_count * 2 && at < (size_t)n * n; i += 2)
    for (uint32_t k = 0; k < runs[i] && at < (size_t)n * n; ++k, ++at) {
      const uint32_t px = runs[i + 1];
      const float a = (float)(px >> 24) / 255.0f;
      src[at * 4 + 0] = (float)(px & 0xFF) / 255.0f * a;
      src[at * 4 + 1] = (float)((px >> 8) & 0xFF) / 255.0f * a;
      src[at * 4 + 2] = (float)((px >> 16) & 0xFF) / 255.0f * a;
      src[at * 4 + 3] = a;
    }
  // The four taps and weights for each output coordinate (the same across and down).
  std::vector<int> tap((size_t)m * 4);
  std::vector<float> weight((size_t)m * 4);
  for (int o = 0; o < m; ++o) {
    const float pos = ((float)o + 0.5f) / (float)kUpscale - 0.5f;
    const int base = (int)std::floor(pos);
    const float t = pos - (float)base;
    const float w[4] = {((-t + 2.0f) * t - 1.0f) * t * 0.5f, ((3.0f * t - 5.0f) * t * t + 2.0f) * 0.5f,
                        ((-3.0f * t + 4.0f) * t + 1.0f) * t * 0.5f, (t - 1.0f) * t * t * 0.5f};
    for (int k = 0; k < 4; ++k) { tap[(size_t)o * 4 + k] = std::clamp(base - 1 + k, 0, n - 1); weight[(size_t)o * 4 + k] = w[k]; }
  }
  std::vector<float> wide((size_t)m * n * 4);   // enlarged across
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < m; ++x)
      for (int c = 0; c < 4; ++c) {
        float v = 0;
        for (int k = 0; k < 4; ++k) v += weight[(size_t)x * 4 + k] * src[((size_t)y * n + tap[(size_t)x * 4 + k]) * 4 + c];
        wide[((size_t)y * m + x) * 4 + c] = v;
      }
  ImTextureData* face = IM_NEW(ImTextureData)();
  face->Create(ImTextureFormat_RGBA32, m, m);
  uint32_t* out = reinterpret_cast<uint32_t*>(face->GetPixels());
  const int gm = m + 2 * kGlowPad;
  std::vector<float> shape((size_t)gm * gm, 0.0f), pass((size_t)gm * gm, 0.0f);
  for (int y = 0; y < m; ++y)
    for (int x = 0; x < m; ++x) {
      float v[4] = {0, 0, 0, 0};
      for (int k = 0; k < 4; ++k)
        for (int c = 0; c < 4; ++c) v[c] += weight[(size_t)y * 4 + k] * wide[((size_t)tap[(size_t)y * 4 + k] * m + x) * 4 + c];
      const float a = std::clamp(v[3], 0.0f, 1.0f);
      uint32_t px = 0;
      if (a > 0.004f) {
        const auto byte = [a](float premultiplied) { return (uint32_t)(std::clamp(premultiplied / a, 0.0f, 1.0f) * 255.0f + 0.5f); };
        px = (uint32_t)(a * 255.0f + 0.5f) << 24 | byte(v[2]) << 16 | byte(v[1]) << 8 | byte(v[0]);
      }
      out[(size_t)y * m + x] = px;
      shape[(size_t)(y + kGlowPad) * gm + (x + kGlowPad)] = a;
    }
  face->UsedRect = ImTextureRect{0, 0, (unsigned short)m, (unsigned short)m};
  // The outline: the shape blurred (three box passes, close to a Gaussian), then made strong near the edge.
  constexpr int kRadius = 9;
  for (int round = 0; round < 3; ++round) {
    for (int y = 0; y < gm; ++y) {
      float sum = 0;
      for (int x = -kRadius; x <= kRadius; ++x) sum += shape[(size_t)y * gm + std::clamp(x, 0, gm - 1)];
      for (int x = 0; x < gm; ++x) {
        pass[(size_t)y * gm + x] = sum / (float)(2 * kRadius + 1);
        sum += shape[(size_t)y * gm + std::min(x + kRadius + 1, gm - 1)] - shape[(size_t)y * gm + std::max(x - kRadius, 0)];
      }
    }
    for (int x = 0; x < gm; ++x) {
      float sum = 0;
      for (int y = -kRadius; y <= kRadius; ++y) sum += pass[(size_t)std::clamp(y, 0, gm - 1) * gm + x];
      for (int y = 0; y < gm; ++y) {
        shape[(size_t)y * gm + x] = sum / (float)(2 * kRadius + 1);
        sum += pass[(size_t)std::min(y + kRadius + 1, gm - 1) * gm + x] - pass[(size_t)std::max(y - kRadius, 0) * gm + x];
      }
    }
  }
  ImTextureData* glow = IM_NEW(ImTextureData)();
  glow->Create(ImTextureFormat_RGBA32, gm, gm);
  uint32_t* gout = reinterpret_cast<uint32_t*>(glow->GetPixels());
  for (size_t i = 0; i < (size_t)gm * gm; ++i)
    gout[i] = (uint32_t)(std::clamp(shape[i] * 2.6f, 0.0f, 1.0f) * 255.0f + 0.5f) << 24 | 0x00FFFFFFu;
  glow->UsedRect = ImTextureRect{0, 0, (unsigned short)gm, (unsigned short)gm};
  p.face = face;
  p.glow = glow;
  gx::keep_user_texture(p.face, g_face_context[which]);
  gx::keep_user_texture(p.glow, g_glow_context[which]);
  return p;
}

// A bright colour per wall, as an outline around the face; the face itself keeps its own colours.
constexpr ImU32 kGlow[] = {IM_COL32(255, 70, 110, 240), IM_COL32(255, 150, 40, 240), IM_COL32(255, 225, 60, 240),
                           IM_COL32(70, 235, 120, 240), IM_COL32(50, 205, 255, 240), IM_COL32(125, 120, 255, 240),
                           IM_COL32(230, 100, 255, 240)};

// Each face's size, of the picture's height: Bailey, Walter.
constexpr float kFaceSize[2] = {0.17f, 0.14f};
// The character select's own units on screen, at the hand's depth: the picture is 57.3 of them tall
// (measured: the hand crosses 25.3 units in 318 of 720p's pixels), its middle is 0, 0, up is positive.
// The hand's position is the glove's corner; its fingertip is 5 units to the right and 1.1 down.
constexpr float kUnitsTall = 57.3f;
constexpr float kHandTipX = 5.0f, kHandTipY = -1.1f;   // from the hand's reported position to the fingertip (units)

// MELEE_TEST_EGG_SHOTS=1 (tests): a picture every 2 seconds while faces are out and a burst of 8 on a
// corner (the F2 capture path, capture/blink_N.ppm in the run folder), each one logged.
bool shots_on() {
  static const bool on = [] { const char* s = host::test_env::getenv("MELEE_TEST_EGG_SHOTS"); return s && *s == '1'; }();
  return on;
}
void shot(const char* why, unsigned frames) {
  if (!shots_on()) return;
  gx::gx_capture_request_set(frames);
  host::log("walter egg: shot %s (%u)", why, frames);
}
// MELEE_TEST_EGG_PIN=1 (tests): a face does not fly; it sits where port 1's hand was when it came out,
// so a script can take hold of it (a slingshot then sets it flying). MELEE_TEST_EGG_HANDMARK=1
// (tests): a small cross where each hand's fingertip is taken to be.
bool test_flag(const char* name) {
  const char* s = host::test_env::getenv(name);
  return s && *s == '1';
}
bool pinned() { static const bool on = test_flag("MELEE_TEST_EGG_PIN"); return on; }
bool hand_marks() { static const bool on = test_flag("MELEE_TEST_EGG_HANDMARK"); return on; }
// MELEE_TEST_EGG_EACH=n (tests): each code sends n faces (1 to kMaxFaces), so a run reaches the limit fast.
size_t faces_per_code() {
  static const size_t n = [] {
    const char* s = host::test_env::getenv("MELEE_TEST_EGG_EACH");
    const long v = s ? std::strtol(s, nullptr, 10) : 1;
    return (size_t)std::clamp<long>(v, 1, (long)kMaxFaces);
  }();
  return n;
}

int aimed_override() {
  // MELEE_TEST_EGG_AIMED=1 / 0 (tests): every launch aimed / none aimed; unset: about one in five.
  static const int v = [] { const char* s = host::test_env::getenv("MELEE_TEST_EGG_AIMED"); return s ? (*s == '1' ? 1 : 0) : -1; }();
  return v;
}

// The first few faces log their paths and corners; past that the log would only fill up.
bool chatty() { return g_faces.size() <= 4; }

// A face's pace along a path: about a sixth of the picture's height a second, the screensaver's.
double step_length(float h) { return std::max(1.0, (double)h / 0.8 / 60.0 * 0.22); }

// Its velocity along a path at step t (pixels a step, screen directions: x right, y down).
double velocity_x(const Launch& l, double t) {
  const long long k = wall_index(l.x0 + l.vx * t, l.w);
  const double v = (k % 2 == 0) ? l.vx : -l.vx;
  return l.mirror_x ? -v : v;
}
double velocity_y(const Launch& l, double t) {
  const long long k = wall_index(l.y0 + l.vy * t, l.h);
  const double v = (k % 2 == 0) ? l.vy : -l.vy;
  return l.mirror_y ? -v : v;
}

void new_launch(Face& f, float w, float h, double from_x, double from_y) {
  const int force = aimed_override();
  const bool aimed = force >= 0 ? force == 1 : f.rng.next() % 5 == 0;
  f.path = launch(w, h, step_length(h), aimed, f.rng, from_x, from_y);
  f.box_w = w; f.box_h = h;
  f.t = 0; f.last_step = 0;
  if (chatty())
    host::log("walter egg: launch %s%s", f.path.corner_step >= 0 ? "aimed at a corner, step " : "unaimed",
              f.path.corner_step >= 0 ? std::to_string(f.path.corner_step).c_str() : "");
}

// Let go of a pull: the face flies off the other way, as from a slingshot, the faster the further it
// was pulled (power 0..1), and eases back to its own pace as it bounces around; spin_add is the spin
// the pull's off-centre hold gives it.
void sling(Face& f, float w, float h, double now, double from_x, double from_y, float dir_x, float dir_y, float power,
           double spin_add) {
  const double length = step_length(h);
  Launch l;
  l.w = w > 1 ? w : 1; l.h = h > 1 ? h : 1;
  l.mirror_x = dir_x < 0; l.mirror_y = dir_y < 0;
  l.vx = std::abs(dir_x) * length; l.vy = std::abs(dir_y) * length;
  from_x = std::clamp(from_x, 0.0, l.w); from_y = std::clamp(from_y, 0.0, l.h);
  l.x0 = l.mirror_x ? l.w - from_x : from_x;
  l.y0 = l.mirror_y ? l.h - from_y : from_y;
  l.corner_step = -1;
  f.path = l; f.box_w = w; f.box_h = h;
  f.t = 0; f.last_step = 0;
  f.slung_at = now; f.slung_power = power; f.slung = true;
  f.spin = std::clamp(f.spin + spin_add, -kMaxSpin, kMaxSpin);
  host::log("walter egg: slung %.2f %.2f, power %.2f, spin %.1f", dir_x, dir_y, power, f.spin);
}

// A corner: the character select's own select sound and a flash over the picture. With many faces
// out corners come often, so the sound waits a moment between two and a flash at least 1.5 s.
void corner_cheer(double time) {
  if (time - g_sound_at >= 0.3) { g_sound_at = time; g_sound.store(1); }
  if (time - g_flash_at >= 1.5) g_flash_at = time;
}

struct Hands {
  const Frame* fr = nullptr;
  // Each hand's fingertip on screen (the untilted picture).
  bool at(int port, ImVec2* out) const {
    if (!g_hand_at[port].load()) return false;
    const float unit = (fr->game_y1 - fr->game_y0) / kUnitsTall;
    const uint32_t packed = g_hand[port].load();
    const float hx = (float)(int16_t)(packed >> 16) * 0.1f, hy = (float)(int16_t)(packed & 0xFFFF) * 0.1f;
    out->x = (fr->game_x0 + fr->game_x1) * 0.5f + (hx + kHandTipX) * unit;
    out->y = (fr->game_y0 + fr->game_y1) * 0.5f - (hy + kHandTipY) * unit;
    return true;
  }
};

// ---- the 3D view: the C-stick turns the character select's camera (up to 30 degrees around what it
// looks at). The faces are flat cards in that scene, each at its own depth between the hands and the
// camera: untilted they are exactly where the flat drawing puts them; turned, they turn with the
// picture, in perspective, spread apart by their depths, and rock in 3D the further it is turned.
struct V3 { double x = 0, y = 0, z = 0; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 unit(V3 a) { const double l = std::sqrt(dot(a, a)); return l > 1e-12 ? a * (1.0 / l) : V3{}; }
V3 rotate(V3 v, V3 axis, double angle) {   // Rodrigues, axis of length 1
  const double c = std::cos(angle), s = std::sin(angle);
  return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

struct View3 {
  bool tilted = false;
  double turn = 0;                   // 0 untilted .. 1 a full 30 degree turn
  V3 eye0, right0, up0, ahead0;      // the camera before the C-stick turned it
  V3 eye1, right1, up1, ahead1;      // and after
  double focal = 1;                  // pixels at depth 1
  double hands_depth = 1;            // the hands' plane, from eye0 along ahead0
  float cx = 0, cy = 0;              // the picture's middle
};

View3 view3(const Frame& fr) {
  View3 v;
  Camera c;
  { std::lock_guard<std::mutex> lock(g_camera_mutex); c = g_camera; }
  if (!c.ok) return v;
  const V3 e0{c.eye0[0], c.eye0[1], c.eye0[2]}, e1{c.eye[0], c.eye[1], c.eye[2]};
  const V3 at{c.interest[0], c.interest[1], c.interest[2]}, up{c.up[0], c.up[1], c.up[2]};
  v.ahead0 = unit(at - e0); v.ahead1 = unit(at - e1);
  v.right0 = unit(cross(v.ahead0, up)); v.right1 = unit(cross(v.ahead1, up));
  v.up0 = cross(v.right0, v.ahead0); v.up1 = cross(v.right1, v.ahead1);
  if (dot(v.right0, v.right0) < 0.5 || dot(v.right1, v.right1) < 0.5) return v;
  const double angle = std::acos(std::clamp(dot(v.ahead0, v.ahead1), -1.0, 1.0));
  if (angle < 0.2 * 3.14159265358979 / 180.0) return v;   // the C-stick is in its dead zone: flat, as always
  const float gh = fr.game_y1 - fr.game_y0;
  const double half = std::tan(std::clamp((double)c.fovy, 5.0, 150.0) * 3.14159265358979 / 360.0);
  v.focal = gh * 0.5 / half;
  v.hands_depth = kUnitsTall * 0.5 / half;
  v.eye0 = e0; v.eye1 = e1;
  v.cx = (fr.game_x0 + fr.game_x1) * 0.5f; v.cy = (fr.game_y0 + fr.game_y1) * 0.5f;
  v.turn = std::min(1.0, angle / (30.0 * 3.14159265358979 / 180.0));
  v.tilted = true;
  return v;
}

// A point of the untilted picture at this depth along its own ray from the untilted eye.
V3 to_world(const View3& v, float sx, float sy, double depth) {
  return v.eye0 + (v.ahead0 + v.right0 * ((sx - v.cx) / v.focal) - v.up0 * ((sy - v.cy) / v.focal)) * depth;
}
ImVec2 to_screen(const View3& v, V3 w) {
  const V3 d = w - v.eye1;
  const double z = std::max(1e-3, dot(d, v.ahead1));
  return ImVec2((float)(v.cx + v.focal * dot(d, v.right1) / z), (float)(v.cy - v.focal * dot(d, v.up1) / z));
}
double face_depth(const View3& v, const Face& f) { return v.hands_depth * (1.0 - 0.22 * f.depth); }

// A new face of this kind; at the limit, the oldest one nobody holds leaves to make room.
void add_face(int which, const Frame& fr, const Hands& hands) {
  if (g_faces.size() >= kMaxFaces) {
    auto oldest = std::find_if(g_faces.begin(), g_faces.end(), [](const Face& f) { return f.grab.port < 0; });
    g_faces.erase(oldest == g_faces.end() ? g_faces.begin() : oldest);
    if (!g_full_logged) {
      g_full_logged = true;
      host::log("walter egg: %zu out, the oldest leaves for each new one", kMaxFaces);
    }
  }
  const float gw = fr.game_x1 - fr.game_x0, gh = fr.game_y1 - fr.game_y0;
  Face f;
  f.which = which == 1 ? 1 : 0;
  // Faces sent on the same frame each take their own seed, so each takes its own path.
  uint64_t seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() + 0x9E3779B97F4A7C15ull * ++g_serial;
  seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
  seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
  f.rng.s = (seed ^ (seed >> 31)) | 1;
  f.size = std::round(gh * kFaceSize[f.which]);
  // It comes out already turning a little, either way.
  f.spin = ((f.rng.next() & 1) ? 1.0 : -1.0) * (1.0 + 1.5 * f.rng.unit());
  f.depth = (float)f.rng.unit();
  f.rock_phase = (float)(f.rng.unit() * 6.283185307179586);
  const float w = gw - f.size, h = gh - f.size;
  ImVec2 hand;
  if (pinned() && hands.at(0, &hand))
    new_launch(f, w, h, std::clamp(hand.x - fr.game_x0 - f.size * 0.5f, 0.0f, w), std::clamp(hand.y - fr.game_y0 - f.size * 0.5f, 0.0f, h));
  else
    new_launch(f, w, h, -1, -1);
  g_faces.push_back(f);
}

// One face's travel this frame (its clock, path, walls, corners and spin), then where it is drawn.
void move(Face& f, const Frame& fr, double frame_time) {
  const float gh = fr.game_y1 - fr.game_y0, gw = fr.game_x1 - fr.game_x0;
  f.size = std::round(gh * kFaceSize[f.which]);
  const float size = f.size, w = gw - size, h = gh - size;
  // Its own clock: it stands still while a hand holds it.
  if (f.grab.port >= 0) f.held += frame_time;
  const double now = fr.time - f.held;
  f.now = now;
  if (std::abs(w - f.box_w) > 0.5f || std::abs(h - f.box_h) > 0.5f) {
    // The window changed size: carry on from the same spot of the new picture, a fresh path.
    const double px = f.path.x(f.t) / std::max(1.0, f.path.w) * w, py = f.path.y(f.t) / std::max(1.0, f.path.h) * h;
    new_launch(f, w, h, std::clamp(px, 0.0, (double)w), std::clamp(py, 0.0, (double)h));
  }
  // It moves unless a hand holds it; faster just after a slingshot.
  double pace = 1.0;
  if (f.slung_at >= 0) pace += 9.0 * f.slung_power * std::exp(-(now - f.slung_at) / 1.1);
  const bool still = (pinned() && !f.slung) || f.grab.port >= 0;
  if (!still) f.t += frame_time * kStepsPerSecond * pace;
  const double radius = std::max(1.0, 0.5 * (double)size);
  const double per_second = kStepsPerSecond * pace;
  const auto grip = [&](double rolling) { f.spin += kWallGrip * (rolling - f.spin); };
  const long long step = (long long)std::floor(f.t);
  for (long long s = f.last_step + 1; s <= step; ++s) {
    f.last_step = s;
    const int hit = f.path.hits(s);
    if (!hit) continue;
    ++f.bounces;
    if (hit & 1) f.squash_x_at = now;
    if (hit & 2) f.squash_y_at = now;
    // How fast it came in along each wall it met (pixels a second), turned into spin by the wall's grip:
    // rolling along a side wall is its speed down that wall over its radius, along the top or bottom its
    // speed across.
    const double vx = velocity_x(f.path, (double)s - 1.0) * per_second, vy = velocity_y(f.path, (double)s - 1.0) * per_second;
    if (hit & 1) grip((f.path.x((double)s) > f.path.w * 0.5 ? -vy : vy) / radius);
    if (hit & 2) grip((f.path.y((double)s) > f.path.h * 0.5 ? vx : -vx) / radius);
    if (hit == 3) {
      // A corner: a burst of spin, a pop, the select sound and a flash, and straight on out of it.
      f.spin += (f.spin >= 0 ? 1.0 : -1.0) * kCornerSpin;
      f.pop_at = now;
      corner_cheer(fr.time);
      shot("corner", 8);
      if (chatty()) host::log("walter egg: corner at step %lld (%s)", s, f.path.corner_step == s ? "aimed" : "unaimed");
      new_launch(f, w, h, f.path.x((double)s), f.path.y((double)s));
      break;
    }
  }
  if (f.path.corner_step < 0 && !f.slung && f.t > 12000.0)
    new_launch(f, w, h, f.path.x(f.t), f.path.y(f.t));   // past the checked stretch: a new path
  f.x = fr.game_x0 + (float)f.path.x(f.t);
  f.y = fr.game_y0 + (float)f.path.y(f.t);

  // Squash on a wall (a short ease back) and a pop in a corner.
  auto squash = [&](double at) { const double a = now - at; return a >= 0 && a < 0.18 ? (float)(1.0 - a / 0.18) : 0.0f; };
  const float sx_ = squash(f.squash_x_at), sy_ = squash(f.squash_y_at);
  float pop = 1.0f;
  if (const double a = now - f.pop_at; f.pop_at >= 0 && a >= 0 && a < 0.35) pop += 0.22f * (float)std::sin(3.14159265358979 * a / 0.35);
  f.scale_x = (1.0f - 0.22f * sx_ + 0.12f * sy_) * pop;
  f.scale_y = (1.0f - 0.22f * sy_ + 0.12f * sx_) * pop;
  // The spin coasts, fading slowly; a held face does not turn.
  if (f.grab.port < 0) {
    f.spin = std::clamp(f.spin, -kMaxSpin, kMaxSpin) * std::exp(-frame_time / kSpinFade);
    f.turn = std::remainder(f.turn + f.spin * frame_time, 6.283185307179586);
  }
  f.angle = (float)f.turn;
  f.cx = f.x + size * 0.5f;
  f.cy = f.y + size * 0.5f;
}

// A held face follows its hand; let go, it is slung, spinning by where it was held.
void follow_hand(Face& f, const Frame& fr, const Hands& hands) {
  Grab& g = f.grab;
  if (g.port < 0) return;
  ImVec2 hand;
  if (!g_a_held[g.port].load() || !hands.at(g.port, &hand)) {
    host::log("walter egg: port %d let go (pulled %.0f %.0f px)", g.port + 1, g.pull_x, g.pull_y);
    g.released_at = fr.time; g.released_x = g.pull_x; g.released_y = g.pull_y;
    g.port = -1;
    const float reach = std::sqrt(g.pull_x * g.pull_x + g.pull_y * g.pull_y);
    const float w = (fr.game_x1 - fr.game_x0) - f.size, h = (fr.game_y1 - fr.game_y0) - f.size;
    if (reach > f.size * 0.2f) {   // a real pull: the slingshot
      const float dir_x = -g.pull_x / reach, dir_y = -g.pull_y / reach, power = std::min(1.0f, reach / (f.size * 1.6f));
      // The pull acts at the held spot: off the line through the middle it turns the face (clockwise
      // when the held spot is pulled clockwise around the middle).
      const float c = std::cos(f.angle), s = std::sin(f.angle);
      const float lu = (g.u - 0.5f) * f.size, lv = (g.v - 0.5f) * f.size;
      const float rx = lu * c - lv * s, ry = lu * s + lv * c;
      const double lever = (rx * dir_y - ry * dir_x) / std::max(1.0f, 0.5f * f.size);
      sling(f, w, h, f.now, f.x - fr.game_x0, f.y - fr.game_y0, dir_x, dir_y, power, kSlingSpin * power * lever);
    }
  } else {
    float px = hand.x - g.hand_x0, py = hand.y - g.hand_y0;
    const float reach = std::sqrt(px * px + py * py), most = f.size * 1.6f;
    if (reach > most) { px *= most / reach; py *= most / reach; }
    g.pull_x = px; g.pull_y = py;
  }
}

void draw_face(ImDrawList* dl, Face& f, const Frame& fr, const Pictures& pic, const View3& v) {
  Grab& g = f.grab;
  const float size = f.size, scale_x = f.scale_x, scale_y = f.scale_y, cx = f.cx, cy = f.cy;
  const float c = std::cos(f.angle), s = std::sin(f.angle);
  float pull_x = 0, pull_y = 0;
  if (g.port >= 0) { pull_x = g.pull_x; pull_y = g.pull_y; }
  else if (g.released_at >= 0) {
    const double a = fr.time - g.released_at;
    if (a < 1.4) {
      const float k = (float)(std::exp(-a * 4.5) * std::cos(a * 22.0));   // it swings back and settles
      pull_x = g.released_x * k; pull_y = g.released_y * k;
    } else {
      g.released_at = -1;
    }
  }
  const float pull_length = std::sqrt(pull_x * pull_x + pull_y * pull_y);
  const bool pulled = pull_length > 0.01f;
  const float toward_x = pulled ? pull_x / pull_length : 0, toward_y = pulled ? pull_y / pull_length : 0;
  const float reach2 = (0.34f * size) * (0.34f * size);

  // Its square as a grid, so a pull stretches it from the spot the hand holds. What is behind that
  // spot (against the pull) follows less the further back it is, so it stretches; what is ahead of it
  // is pushed along whole. Every point keeps its order along the pull, so it never folds over itself.
  const auto place = [&](float u, float vv) {
    const float dx = (u - 0.5f) * size * scale_x, dy = (vv - 0.5f) * size * scale_y;
    ImVec2 p(cx + dx * c - dy * s, cy + dx * s + dy * c);
    if (pulled) {
      const float ox = (u - g.u) * size * scale_x, oy = (vv - g.v) * size * scale_y;   // from the held spot, on screen
      const float px = ox * c - oy * s, py = ox * s + oy * c;
      const float along = px * toward_x + py * toward_y, across = -px * toward_y + py * toward_x;
      const float back = along < 0 ? along : 0.0f;
      const float follow = std::exp(-(back * back + across * across) / reach2);
      p.x += pull_x * follow; p.y += pull_y * follow;
    }
    return p;
  };
  // Turned by the C-stick: the card at its depth, rocking about its own across and up axes the
  // further the camera is turned, seen by the turned camera.
  const double depth = v.tilted ? face_depth(v, f) : 1.0;
  const V3 middle = v.tilted ? to_world(v, cx, cy, depth) : V3{};
  const double rock = v.tilted ? v.turn * 0.55 : 0.0;
  const double rock_a = rock * std::sin(f.now * 1.7 + f.rock_phase), rock_b = rock * std::sin(f.now * 1.3 + 2.1 * f.rock_phase);
  const auto point = [&](float u, float vv) {
    const ImVec2 p = place(u, vv);
    if (!v.tilted) return p;
    V3 offset = to_world(v, p.x, p.y, depth) - middle;
    offset = rotate(rotate(offset, v.up0, rock_a), v.right0, rock_b);
    return to_screen(v, middle + offset);
  };
  // A texture over its square from lo to hi (the outline's reaches a little past it), as a grid when
  // pulled or turned (so the picture keeps its shape in perspective).
  const auto mesh = [&](ImTextureData* tex, float lo, float hi, ImU32 colour) {
    const int cells = pulled ? 16 : v.tilted ? 4 : 1;
    for (int j = 0; j < cells; ++j)
      for (int i = 0; i < cells; ++i) {
        const float a0 = (float)i / (float)cells, a1 = (float)(i + 1) / (float)cells;
        const float b0 = (float)j / (float)cells, b1 = (float)(j + 1) / (float)cells;
        const float u0 = lo + (hi - lo) * a0, u1 = lo + (hi - lo) * a1, v0 = lo + (hi - lo) * b0, v1 = lo + (hi - lo) * b1;
        dl->AddImageQuad(tex->GetTexRef(), point(u0, v0), point(u1, v0), point(u1, v1), point(u0, v1),
                         ImVec2(a0, b0), ImVec2(a1, b0), ImVec2(a1, b1), ImVec2(a0, b1), colour);
      }
  };
  if (pic.face && pic.glow) {
    const float room = (float)kGlowPad / (float)(kPictureSize * kUpscale);   // the outline's texture reaches this far past its square
    mesh(pic.glow, -room, 1.0f + room, kGlow[f.bounces % (int)(sizeof kGlow / sizeof kGlow[0])]);
    mesh(pic.face, 0.0f, 1.0f, IM_COL32(255, 255, 255, 255));
  }
}

}  // namespace

void watch_pads(const host::PadState pads[4]) {
  if (!css_visible_now()) {
    if (g_on.exchange(false)) {
      // Every face goes. The drawing thread starts the next visit from these counts.
      for (int k = 0; k < 2; ++k) g_left_sent[k].store(g_sent[k].load());
      g_visit.fetch_add(1);
      host::log("walter egg: off (left the character select, %u codes this visit)", g_codes_this_visit);
    }
    g_codes_this_visit = 0;
    for (Detector& d : g_detectors) d = Detector{};
    for (int p = 0; p < 4; ++p) { g_hand_at[p].store(0); g_a_held[p].store(false); }
    return;
  }
  for (int p = 0; p < 4; ++p) {
    if (pads[p].err != 0) { g_detectors[p].last = 0; g_a_held[p].store(false); continue; }
    g_a_held[p].store((pads[p].button & kA) != 0);
    if (const int code = g_detectors[p].feed(pads[p].button)) {
      // Each code sends one more face out: B A a Bailey, A B a Walter.
      const uint8_t which = code == kWalter ? 1 : 0;
      g_sent[which].fetch_add(1);
      g_on.store(true);
      ++g_codes_this_visit;
      host::log("walter egg: on (code on port %d, %s, %u codes this visit)", p + 1, which ? "Walter" : "Bailey", g_codes_this_visit);
    }
  }
}

void set_hand(int port, int16_t x10, int16_t y10) {
  if (port < 0 || port > 3) return;
  g_hand[port].store((uint32_t)(uint16_t)x10 << 16 | (uint16_t)y10);
  g_hand_at[port].store(std::max<uint32_t>(1, host::retrace_count()));
}

void set_camera(const float* camera) {
  std::lock_guard<std::mutex> lock(g_camera_mutex);
  if (!camera) { g_camera.ok = false; return; }
  for (int i = 0; i < 3; ++i) {
    g_camera.eye0[i] = camera[i];
    g_camera.eye[i] = camera[3 + i];
    g_camera.interest[i] = camera[6 + i];
    g_camera.up[i] = camera[9 + i];
  }
  g_camera.fovy = camera[12];
  bool finite = true;
  for (int i = 0; i < 13; ++i) finite = finite && std::isfinite(camera[i]);
  g_camera.ok = finite && camera[12] > 1.0f && camera[12] < 170.0f;
}

uint8_t css_frame() {
  g_css_retrace.store(std::max<uint32_t>(1, host::retrace_count()), std::memory_order_relaxed);
  return g_sound.exchange(0);
}

bool active() { return g_on.load() && css_visible_now(); }

void draw(const Frame& fr) {
  if (!active()) return;
  const float gw = fr.game_x1 - fr.game_x0, gh = fr.game_y1 - fr.game_y0;
  if (gw < 50 || gh < 50) return;
  const Hands hands{&fr};

  // A new visit to the character select starts with none out; then one face for each code since.
  const uint32_t visit = g_visit.load();
  if (visit != g_seen_visit) {
    g_seen_visit = visit;
    g_faces.clear();
    g_full_logged = false;
    for (int k = 0; k < 2; ++k) g_seen_sent[k] = g_left_sent[k].load();
  }
  bool added = false;
  for (int k = 0; k < 2; ++k) {
    const uint32_t sent = g_sent[k].load();
    for (; g_seen_sent[k] != sent; ++g_seen_sent[k])
      for (size_t n = 0; n < faces_per_code(); ++n) { add_face(k, fr, hands); added = true; }
  }
  if (added) {
    for (int p = 0; p < 4; ++p) g_a_was[p] = g_a_held[p].load();   // the A that ended a code takes no hold
    host::log("walter egg: %zu out", g_faces.size());
  }
  const double frame_time = g_last_frame > 0 ? std::clamp(fr.time - g_last_frame, 0.0, 0.25) : 0.0;
  g_last_frame = fr.time;
  if (g_faces.empty()) return;

  for (Face& f : g_faces) move(f, fr, frame_time);
  {
    static double last_shot = -10;
    if (fr.time - last_shot > 2.0) { last_shot = fr.time; shot("flying", 1); }
  }
  // The C-stick's turn of the character select's camera, and the order the cards are drawn in: the
  // furthest first (the same order turned or not, so nothing jumps when the stick moves).
  const View3 view = view3(fr);
  if (view.tilted != g_tilt_logged) {
    g_tilt_logged = view.tilted;
    host::log("walter egg: %s", view.tilted ? "turned with the camera (C-stick)" : "flat again");
  }
  std::vector<size_t> order(g_faces.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) { return g_faces[a].depth < g_faces[b].depth; });

  // A hand takes hold with A while its fingertip is on a face (the one in front), pulls as it moves,
  // and lets go with A.
  for (int p = 0; p < 4; ++p) {
    const bool a = g_a_held[p].load();
    const bool pressed = a && !g_a_was[p];
    g_a_was[p] = a;
    ImVec2 hand;
    if (!pressed || !hands.at(p, &hand)) continue;
    if (std::any_of(g_faces.begin(), g_faces.end(), [p](const Face& f) { return f.grab.port == p; })) continue;
    for (size_t k = order.size(); k-- > 0;) {
      Face& f = g_faces[order[k]];
      if (f.grab.port >= 0) continue;
      const float c = std::cos(f.angle), s = std::sin(f.angle);
      const float dx = hand.x - f.cx, dy = hand.y - f.cy;
      const float lx = (dx * c + dy * s) / (f.size * f.scale_x), ly = (-dx * s + dy * c) / (f.size * f.scale_y);   // its own square, -0.5..0.5
      if (lx * lx + ly * ly > 0.25f) continue;
      Grab& g = f.grab;
      g.port = p; g.u = lx + 0.5f; g.v = ly + 0.5f;
      g.hand_x0 = hand.x; g.hand_y0 = hand.y; g.pull_x = g.pull_y = 0; g.released_at = -1;
      f.spin = 0;   // a hand on it stops its spin
      host::log("walter egg: port %d took hold at %.2f %.2f", p + 1, g.u, g.v);
      break;
    }
  }
  for (Face& f : g_faces) follow_hand(f, fr, hands);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->PushClipRect(ImVec2(fr.game_x0, fr.game_y0), ImVec2(fr.game_x1, fr.game_y1), true);
  if (const double since = fr.time - g_flash_at; since >= 0 && since < 0.75) {   // a corner's flash
    const float a = (float)(1.0 - since / 0.75);
    dl->AddRectFilled(ImVec2(fr.game_x0, fr.game_y0), ImVec2(fr.game_x1, fr.game_y1), IM_COL32(255, 255, 255, (int)(150 * a * a)));
  }
  const Pictures* pics[2] = {nullptr, nullptr};
  for (const size_t i : order) {
    Face& f = g_faces[i];
    if (!pics[f.which]) pics[f.which] = &pictures(f.which);
    draw_face(dl, f, fr, *pics[f.which], view);
  }
  if (hand_marks()) {
    for (int p = 0; p < 4; ++p) {
      ImVec2 hand;
      if (!hands.at(p, &hand)) continue;
      dl->AddLine(ImVec2(hand.x - 12, hand.y), ImVec2(hand.x + 12, hand.y), IM_COL32(255, 0, 255, 255), 2.0f);
      dl->AddLine(ImVec2(hand.x, hand.y - 12), ImVec2(hand.x, hand.y + 12), IM_COL32(255, 0, 255, 255), 2.0f);
    }
  }
  dl->PopClipRect();
}

}  // namespace walter_egg
