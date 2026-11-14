// Time sync decisions after the opening: one gap is closed once, not by both sides at the same time,
// and a build with this rule still converges against a build with the previous one.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_time_sync.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using namespace slippi::time_sync;
int failures = 0;
void check(bool ok, const char* name) {
  if (!ok) { std::printf("FAILED: %s\n", name); ++failures; }
}

// The rule before this change (and in builds that still have it): the side ahead holds a frame
// whenever it is more than two frames plus the slack ahead, for the whole match.
int OldHaltFrames(int32_t offset_us, int32_t frame) {
  const int32_t t2 = 2 * kFrameUs + kSlackUs;
  if (offset_us <= (frame <= kOpeningFrames ? kSlackUs : t2)) return 0;
  return std::min((offset_us - kSlackUs) / kFrameUs + 1, frame <= kOpeningFrames ? 5 : 1);
}
int OldAdvanceFrames(int32_t offset_us, int32_t frame) {
  const int32_t t2 = kFrameUs + kSlackUs;
  if (offset_us >= -t2) return 0;
  return std::min((-offset_us - kSlackUs) / kFrameUs + 1, frame > kOpeningFrames ? 3 : 0);
}

// The speed correction both builds run (slippi_online.cpp, should_advance_online_frame).
double Deviation(double off_us) {
  if (off_us > -250 && off_us < 8000) return 0;
  if (off_us < 0) return std::min(-off_us / (3 * 16683.0), 1.0) * 0.01;
  return std::min(off_us / (3 * 16683.0), 1.0) * -0.005;
}

double TrimmedMean(std::vector<double> b) {
  std::sort(b.begin(), b.end());
  const int n = (int)b.size(), off = (int)(n / 3.0), end = n - off;
  double sum = 0;
  for (int k = off; k < end; ++k) sum += b[k];
  return end > off ? sum / (end - off) : 0;
}

enum class Rule { Old, New };
struct Side { Rule rule; int pending_advance = 0; double deviation = 0; };

int Halt(Rule r, int32_t off, int32_t frame) { return r == Rule::Old ? OldHaltFrames(off, frame) : HaltFrames(off, frame); }
int Advance(Rule r, int32_t off, int32_t frame) { return r == Rule::Old ? OldAdvanceFrames(off, frame) : AdvanceFrames(off, frame); }

struct Result { double most_ahead_us = 0, largest_gap_us = 0, final_gap_us = 0; int halts = 0, advances = 0; };

// Two games' clocks. gap = how far B runs behind A (us). Both measure it over the last 30 frames
// (trimmed mean, as CalcTimeOffsetUs) and see mirrored offsets: A +gap, B -gap. At frame 300 B
// stalls for `stall_us` (a late packet). `b_slow` makes B's game run slower than A's throughout.
Result Run(Rule a_rule, Rule b_rule, double stall_us, double b_slow = 0, bool b_can_advance = true, int frames = 1200) {
  Side a{a_rule}, b{b_rule};
  double gap = 0;
  std::vector<double> window;
  Result r;
  for (int f = 1; f <= frames; ++f) {
    gap += kFrameUs * (a.deviation - b.deviation + b_slow);
    if (f == 300) gap += stall_us;
    if (a.pending_advance && f % 5 == 0) { --a.pending_advance; gap += kFrameUs; }
    if (b.pending_advance && f % 5 == 0) { --b.pending_advance; gap -= kFrameUs; }
    window.push_back(gap);
    if (window.size() > 30) window.erase(window.begin());
    if (f % 30 == 0) {
      const int32_t off_a = (int32_t)std::lround(TrimmedMean(window)), off_b = -off_a;
      if (int h = Halt(a.rule, off_a, f)) { gap -= h * kFrameUs; ++r.halts; }
      if (int h = Halt(b.rule, off_b, f)) { gap += h * kFrameUs; ++r.halts; }
      // The first extra frame runs on the check frame itself (a multiple of five), the rest every five.
      if (!a.pending_advance && (a.pending_advance = Advance(a.rule, off_a, f))) { ++r.advances; --a.pending_advance; gap += kFrameUs; }
      if (b_can_advance && !b.pending_advance && (b.pending_advance = Advance(b.rule, off_b, f))) { ++r.advances; --b.pending_advance; gap -= kFrameUs; }
      a.deviation = Deviation(off_a);
      b.deviation = Deviation(off_b);
    }
    if (f > 300) {
      r.most_ahead_us = std::min(r.most_ahead_us, gap);   // B now ahead of A: it overshot
      r.largest_gap_us = std::max(r.largest_gap_us, gap);
    }
  }
  r.final_gap_us = gap;
  return r;
}

void test_opening_and_advance_unchanged() {
  bool same_open = true, same_adv = true;
  for (int32_t frame = 30; frame <= 1200; frame += 30)
    for (int32_t off = -200000; off <= 200000; off += 250) {
      if (frame <= kOpeningFrames && HaltFrames(off, frame) != OldHaltFrames(off, frame)) same_open = false;
      if (AdvanceFrames(off, frame) != OldAdvanceFrames(off, frame)) same_adv = false;
    }
  check(same_open, "the opening's holds are unchanged");
  check(same_adv, "advancing is unchanged");
}

void test_hold_after_opening() {
  check(HaltFrames(45000, 210) == 0, "no hold for a gap the other side's advance covers (45 ms)");
  check(HaltFrames(60000, 210) == 0, "no hold at 60 ms");
  check(HaltFrames(kSlackUs + 4 * kFrameUs, 210) == 0, "no hold at three frames plus one plus the slack");
  check(HaltFrames(kSlackUs + 4 * kFrameUs + 1, 210) == 1, "one frame beyond what the other side can cover");
  check(HaltFrames(300000, 210) == 1, "never more than one frame after the opening");
  check(HaltFrames(-80000, 210) == 0, "a side that is behind never holds");
}

void test_one_gap_is_closed_once() {
  // The match on 2026-11-06: B (a Wi-Fi laptop) stalled 1-6 frames on late packets (mostly 2-3),
  // then both sides acted on the same gap. Stalls of 1-6 frames, then longer ones up to 150 ms.
  double worst_old = 0, worst_new = 0, worst_new_ahead = 0, worst_old_ahead = 0;
  bool never_worse = true, mixed_old_ahead_same_as_before = true, all_converge = true;
  for (double stall = 17000; stall <= 150000; stall += 500) {
    const Result oo = Run(Rule::Old, Rule::Old, stall), nn = Run(Rule::New, Rule::New, stall);
    const Result no = Run(Rule::New, Rule::Old, stall), on = Run(Rule::Old, Rule::New, stall);
    if (stall <= 6 * kFrameUs) {
      worst_old = std::min(worst_old, oo.most_ahead_us);
      worst_new = std::min(worst_new, nn.most_ahead_us);
      worst_new_ahead = std::min(worst_new_ahead, no.most_ahead_us);
      worst_old_ahead = std::min(worst_old_ahead, on.most_ahead_us);
    }
    // With either side on the new rule, B never ends up further ahead than with two old builds.
    if (nn.most_ahead_us < oo.most_ahead_us || no.most_ahead_us < oo.most_ahead_us) never_worse = false;
    // A has the old rule and B the new: B's advance is the same as before, so nothing changes.
    if (on.most_ahead_us != oo.most_ahead_us || on.final_gap_us != oo.final_gap_us) mixed_old_ahead_same_as_before = false;
    for (const Result* r : {&oo, &nn, &no, &on})
      if (std::fabs(r->final_gap_us) > 10000) all_converge = false;
  }
  std::printf("most B ran ahead after a 1-6 frame stall: old/old %.1f ms, new/new %.1f ms, new ahead + old behind %.1f ms, "
              "old ahead + new behind %.1f ms\n", worst_old / 1000, worst_new / 1000, worst_new_ahead / 1000, worst_old_ahead / 1000);
  check(worst_old < -kFrameUs - 5000, "two old builds overshoot by over a frame (the bug this fixes)");
  // What is left is the advance's own rounding and the speed correction, as in Slippi Dolphin.
  check(worst_new > -kFrameUs, "two new builds overshoot by under a frame");
  check(worst_new_ahead > -kFrameUs, "a new build ahead of an old one: under a frame");
  check(never_worse, "no stall from 17 to 150 ms leaves B further ahead than two old builds do");
  check(mixed_old_ahead_same_as_before, "an old build ahead of a new one behaves exactly as two old builds");
  check(all_converge, "every pairing settles within 10 ms");
}

void test_a_peer_that_cannot_catch_up() {
  // B runs 2 % slow (more than its 1 % speed-up makes up) and never advances: a machine that cannot
  // run extra frames. A, with the new rule, slows down and holds a frame once the gap passes what B
  // could have covered: the gap stays bounded instead of growing for the whole match.
  const Result r = Run(Rule::New, Rule::Old, 0, 0.02, false, 6000);
  std::printf("B 2%% slow, never advancing: largest gap %.1f ms, %d holds\n", r.largest_gap_us / 1000, r.halts);
  check(r.largest_gap_us < kSlackUs + 5 * kFrameUs, "the gap stays under five frames plus the slack");
  check(r.halts > 0, "A holds frames as the backstop");
}

}  // namespace

int main() {
  test_opening_and_advance_unchanged();
  test_hold_after_opening();
  test_one_gap_is_closed_once();
  test_a_peer_that_cannot_catch_up();
  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("slippi time sync: all checks passed\n");
  return 0;
}
