#include "netplay_summary.h"
#include "enet/yg_net_mirror.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
int main() {
  slippi::diagnostics::Summary s;
  s.begin(2, true); s.frames = 120; s.max_rollback = 7; s.held_up = 2; s.fell_behind = 3;
  for (unsigned i = 1; i <= 100; ++i) s.ping.add(i);
  CHECK(s.line(9, 4, 1, "completed") == "netplay summary: mode friends frames 120 secs 2.000 ping p50 50 p95 95 max 100 rollbacks 9 max-rollback 7 stalls 4 held-up 2 fell-behind 3 desyncs 1 result completed");
  s.begin(1, false); CHECK(s.ping.count == 0 && s.mode == "unranked" && s.held_up == 0);
  YGNetMirror m;
  yg_mirror_reset(&m, 1000, 1);
  CHECK(yg_mirror_percentile(&m.latency, 50) == 0);
  m.notified = 1; m.notified_ms = 1000; m.notified_until = 100;
  for (unsigned i = 1; i <= 100; ++i) {
    yg_mirror_read(&m, 101-i, i-1, 1000+i);
    yg_mirror_service(&m, 1000+i*2, i);
  }
  CHECK(yg_mirror_percentile(&m.depth, 50) == 50 && m.depth.max == 100);
  CHECK(yg_mirror_percentile(&m.latency, 50) == 50);
  CHECK(yg_mirror_percentile(&m.latency, 99) == 99 && m.latency.max == 100);
  CHECK(m.service.count == 99 && m.service.max == 2);
  yg_mirror_read(&m, 1, 100, 2000); CHECK(m.latency.count == 100); // newer slot has no timestamp
  yg_mirror_sample(&m.service, 9999);
  CHECK(m.service.max == 9999 && yg_mirror_percentile(&m.service, 100) == 2048);
  m.out_batch = 5; yg_mirror_service(&m, 3000, 85);
  CHECK(m.out_batch_max == 5 && m.out_batch == 0);
  yg_mirror_reset(&m, 11000, 1);
  CHECK(m.start_ms == 11000 && !m.depth.count && !m.latency.count && !m.service.count && !m.ping.count && !m.out_batch_max);
  yg_mirror_reset(&m, 12000, 0); yg_mirror_read(&m, 2, 0, 12001); yg_mirror_service(&m, 12001, 9);
  CHECK(!m.depth.count && !m.service.count && !m.ping.count);
  std::puts("net diagnostics: exact summary, percentiles, maxima, observed notification boundary and window resets passed");
}
