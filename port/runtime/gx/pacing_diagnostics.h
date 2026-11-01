// Pure diagnostic math. No platform calls, pacing decisions, or per-frame allocation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gx::pacing_diag {
struct Summary {
  uint64_t frames = 0, late = 0;
  size_t samples = 0;
  double p50 = 0, p95 = 0, p99 = 0, maximum = 0;
};

// Nearest-rank percentiles (ceil(p*N), one based). The latest 65536 intervals fit
// a minute at up to ~1092 FPS. If overwritten, frames still counts every Present;
// samples explicitly identifies the subset used for percentiles and late count.
template<size_t Capacity = 65536> class FrameRing {
 public:
  void present(double seconds) {
    ++frames_;
    if (have_time_ && seconds > previous_) {
      values_[next_] = (seconds - previous_) * 1000.0;
      next_ = (next_ + 1) % Capacity;
      if (size_ < Capacity) ++size_;
    }
    previous_ = seconds;
    have_time_ = true;
  }
  Summary summarize(double target_ms) {
    Summary s; s.frames = frames_; s.samples = size_;
    if (size_) {
      std::sort(values_.begin(), values_.begin() + size_);
      const auto percentile = [&](double p) { return values_[size_t(std::ceil(p * size_)) - 1]; };
      s.p50 = percentile(.50); s.p95 = percentile(.95); s.p99 = percentile(.99);
      s.maximum = values_[size_ - 1];
      if (target_ms > 0)
        for (size_t i = 0; i < size_; ++i) if (values_[i] > 1.5 * target_ms) ++s.late;
    }
    frames_ = 0; size_ = next_ = 0; // Retain previous_ for the next Present-to-Present interval.
    return s;
  }
 private:
  std::array<double, Capacity> values_{};
  size_t size_ = 0, next_ = 0;
  uint64_t frames_ = 0;
  bool have_time_ = false;
  double previous_ = 0;
};

struct StatisticsSample { uint32_t present = 0, present_refresh = 0, sync_refresh = 0; };
struct DisplayCounts {
  uint64_t shown_twice = 0, never_shown = 0, samples = 0, unavailable = 0, disjoint = 0;
};
class Statistics {
 public:
  void unavailable(bool disjoint) {
    have_ = false; ++counts_.unavailable;
    if (disjoint) ++counts_.disjoint;
  }
  void add(StatisticsSample s) {
    ++counts_.samples;
    if (!have_ || s.present < last_.present || s.present_refresh < last_.present_refresh ||
        s.sync_refresh < last_.sync_refresh) {
      // Reset/wrap/disjoint: never subtract across different statistics epochs.
      last_ = s; have_ = true; return;
    }
    if (s.present == last_.present) {
      last_.sync_refresh = s.sync_refresh; // Track resets even while the displayed ID is stationary.
      return; // No completed display interval yet.
    }
    const uint64_t dp = s.present - last_.present;
    const uint64_t dr = s.present_refresh - last_.present_refresh;
    // PresentCount is the ID of the latest displayed present; PresentRefreshCount
    // is its refresh, NOT the number of frames displayed. Across completed display
    // intervals, dr > dp proves at least dr-dp repeated refreshes; dp > dr indicates
    // at least dp-dr presents without a separate refresh. SyncRefreshCount is the
    // sampling clock: use it to detect resets, not to charge the still-pending tail
    // repeatedly. Samples can hide a skip followed by a repeat, so these are LOWER
    // BOUNDS, not exact counts. With interval-0 tearing, multiple images may scan
    // in one refresh: "never-shown" then means no separate full refresh, not no
    // visible pixels. Failed/disjoint samples break the baseline; zero with no
    // readable samples means unavailable, not proof of smooth display delivery.
    if (dr > dp) counts_.shown_twice += dr - dp;
    if (dp > dr) counts_.never_shown += dp - dr;
    last_ = s;
  }
  DisplayCounts take() { auto c = counts_; counts_ = {}; return c; }
 private:
  StatisticsSample last_{};
  bool have_ = false;
  DisplayCounts counts_{};
};
} // namespace gx::pacing_diag
