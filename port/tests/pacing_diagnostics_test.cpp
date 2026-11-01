#include "pacing_diagnostics.h"
#include <cstdio>
#include <cmath>

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace gx::pacing_diag;
int main() {
  FrameRing<128> frames;
  auto s = frames.summarize(10);
  CHECK(s.samples == 0 && s.frames == 0 && s.p99 == 0 && s.late == 0);
  frames.present(0);
  s = frames.summarize(10);
  CHECK(s.frames == 1 && s.samples == 0);
  double time = 0;
  for (int ms = 1; ms <= 100; ++ms) { time += ms / 1000.0; frames.present(time); }
  s = frames.summarize(10);
  CHECK(s.frames == 100 && s.samples == 100);
  CHECK(std::fabs(s.p50 - 50) < 1e-8);
  CHECK(std::fabs(s.p95 - 95) < 1e-8);
  CHECK(std::fabs(s.p99 - 99) < 1e-8);
  CHECK(std::fabs(s.maximum - 100) < 1e-8);
  CHECK(s.late == 85);
  s = frames.summarize(10);
  CHECK(s.samples == 0 && s.frames == 0);
  frames.present(time + .010);
  s = frames.summarize(10);
  CHECK(s.samples == 1 && s.frames == 1 && s.late == 0);
  CHECK(std::fabs(s.p50 - 10) < 1e-8 && s.p50 == s.p95 && s.p95 == s.p99);
  FrameRing<3> ring;
  ring.present(0); ring.present(.001); ring.present(.003); ring.present(.006); ring.present(.010);
  s = ring.summarize(2);
  CHECK(s.frames == 5 && s.samples == 3);
  CHECK(std::fabs(s.p50 - 3) < 1e-8 && std::fabs(s.maximum - 4) < 1e-8);
  CHECK(s.late == 1);
  FrameRing<3> boundary;
  boundary.present(0); boundary.present(.015); boundary.present(.031);
  s = boundary.summarize(10);
  CHECK(s.late == 1); // strict >, not >=
  boundary.present(.031); boundary.present(.030);
  CHECK(boundary.summarize(0).samples == 0);
  Statistics stats;
  stats.add({10, 100, 101}); stats.add({11, 101, 102});
  auto c = stats.take();
  CHECK(c.samples == 2 && c.shown_twice == 0 && c.never_shown == 0);
  stats.add({12, 103, 104});
  c = stats.take(); CHECK(c.shown_twice == 1 && c.never_shown == 0);
  stats.add({15, 104, 105});
  c = stats.take(); CHECK(c.never_shown == 2 && c.shown_twice == 0);
  stats.add({15, 104, 106}); stats.add({15, 104, 107});
  c = stats.take(); CHECK(c.shown_twice == 0 && c.never_shown == 0);
  stats.add({16, 107, 108});
  c = stats.take(); CHECK(c.shown_twice == 2); // stationary tail counted once, when completed
  stats.unavailable(true); stats.add({50, 500, 501});
  c = stats.take(); CHECK(c.disjoint == 1 && c.unavailable == 1 && c.never_shown == 0 && c.shown_twice == 0);
  stats.add({51, 501, 502});
  c = stats.take(); CHECK(c.never_shown == 0 && c.shown_twice == 0);
  stats.unavailable(false); stats.add({100, 900, 901});
  c = stats.take(); CHECK(c.disjoint == 0 && c.unavailable == 1 && c.never_shown == 0);
  stats.add({1, 1, 1});
  c = stats.take(); CHECK(c.never_shown == 0 && c.shown_twice == 0); // reset/wrap
  stats.add({2, 1, 2});
  c = stats.take(); CHECK(c.never_shown == 1); // two IDs in the same refresh
  stats.add({3, 2, 1});
  c = stats.take(); CHECK(c.never_shown == 0 && c.shown_twice == 0); // sync clock reset
  Statistics clock_reset;
  clock_reset.add({10, 10, 10}); clock_reset.add({10, 10, 20});
  clock_reset.add({11, 19, 15}); // Sync clock reset after a stationary sample: discard the gap.
  c = clock_reset.take(); CHECK(c.shown_twice == 0 && c.never_shown == 0);
  std::printf("%d checks passed\n", checks);
}
