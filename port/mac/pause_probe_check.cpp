// GPU-free fake-clock check: no thread, Mach call, sleep, or game initialization.
// SPDX-License-Identifier: GPL-2.0-or-later
#define PAUSE_PROBE_POLICY_ONLY
#include "../runtime/host/pause_probe.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks = 0;
static void check(bool ok, const char* message) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
  using namespace host::pause_probe;
  // All timestamps are supplied by this fake clock, in nanoseconds.
  check(!pacing_gap(5 * ms, 5 * ms), "on-time sleep");
  check(!pacing_gap(5 * ms, 4 * ms), "early wake cannot underflow");
  check(!pacing_gap(5 * ms, 255 * ms), "exact 250 ms overshoot excluded");
  check(pacing_gap(5 * ms, 255 * ms + 1), "overshoot threshold strict");
  check(pacing_gap(5 * ms, 1873 * ms), "d0051 pacing fixture");
  GapWatch zero_origin{0};
  check(zero_origin.poll(501 * ms, 0).sample, "zero-origin fake clock can sample first stall");
  GapWatch healthy{1000 * ms};
  check(!healthy.poll(1010 * ms, 1000 * ms).gap, "normal heartbeat");
  check(!healthy.poll(1260 * ms, 1250 * ms).gap, "exact 250 ms gap excluded");
  check(healthy.poll(1510 * ms + 1, 1500 * ms).gap == 250 * ms + 1, "gap threshold strict");
  check(!healthy.poll(1500 * ms, 1510 * ms).sample, "progress raced ahead of clock");
  GapWatch whole_process{1000 * ms};
  const auto both = whole_process.poll(2873 * ms, 1000 * ms);
  check(both.gap == 1873 * ms && both.sample, "whole-process scheduling gap also samples");
  check(!whole_process.poll(2883 * ms, 1000 * ms).sample, "one sample per unchanged progress");
  GapWatch main_only{1000 * ms};
  bool saw_sample = false;
  for (uint64_t t = 1010 * ms; t <= 1700 * ms; t += 10 * ms) {
    const auto event = main_only.poll(t, 1000 * ms);
    check(!event.gap, "main-only stall keeps heartbeat continuous");
    if (t == 1500 * ms) check(!event.sample, "exact 500 ms main stall excluded");
    if (event.sample) {
      check(t == 1510 * ms && !saw_sample, "sample once at first >500 ms poll");
      saw_sample = true;
    }
  }
  check(saw_sample && main_only.samples == 1, "continuous heartbeat still detects main stall");
  check(main_only.poll(2510 * ms, 2000 * ms).sample, "new stalled frame second sample");
  check(main_only.poll(3510 * ms, 3000 * ms).sample, "new stalled frame third sample");
  check(!main_only.poll(4510 * ms, 4000 * ms).sample && main_only.samples == 3, "three per run cap");
  GapWatch recovered{1000 * ms};
  const auto resumed = recovered.poll(2873 * ms, 2870 * ms);
  check(resumed.gap && !resumed.sample, "resumed main reports heartbeat gap without stale sample");
  MemoryCadence memory;
  for (unsigned tick = 1; tick < 10; ++tick)
    check(!memory.poll(false), "memory APIs skipped for first nine heartbeat ticks");
  check(memory.poll(false), "memory sampled on tenth heartbeat tick");
  check(!memory.poll(false), "routine memory sample resets cadence");
  check(memory.poll(resumed.gap || resumed.sample), "recovered whole-process gap forces memory sample");
  check(memory.poll(both.gap || both.sample), "combined gap and stack attempt forces memory sample");
  check(memory.poll(saw_sample), "main-only stack attempt forces memory sample");
  for (unsigned tick = 1; tick < 10; ++tick)
    check(!memory.poll(false), "diagnostic memory sample resets cadence");
  check(memory.poll(false), "routine sampling resumes ten ticks after diagnostic");
  std::printf("Pause probe fake-clock checks: %u/%u passed\n", checks, checks);
}
