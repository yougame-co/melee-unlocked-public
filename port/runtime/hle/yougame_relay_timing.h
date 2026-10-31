// Bounded relay timing windows; stamps are steady_clock milliseconds, including helper hs.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>

namespace yougame::relay_pads {
inline double steady_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
class Timing {
 public:
  // More than 8 kHz for 30 seconds. If exhausted, retain the most recent samples;
  // counts and maxima still cover every line. No allocation or sorting on arrival.
  static constexpr size_t capacity = 262144;
  struct Samples {
    std::unique_ptr<std::array<double, capacity>> values{new std::array<double, capacity>};
    uint64_t count = 0;
    double maximum = 0;
    void add(double value) { (*values)[count++ % capacity] = value; maximum = std::max(maximum, value); }
    double percentile(double p) {
      const size_t n = size_t(std::min<uint64_t>(count, capacity));
      if (!n) return 0;
      const size_t at = size_t(std::ceil(p * n)) - 1;
      std::nth_element(values->begin(), values->begin() + at, values->begin() + n);
      return (*values)[at];
    }
    void clear() { count = 0; maximum = 0; }
  };
  void arrival(double now, bool has_hs, double hs, bool attached) {
    if (!attached) { reset(); return; }
    if (!active_) { active_ = true; start_ = now; }
    if (pending_) ++overwritten_;
    ++lines_;
    if (has_hs) {
      ++hs_;
      if (now < hs) ++negative_;
      else arrival_.add(now - hs);
    }
    pending_ = true; pending_stats_ = has_hs && now >= hs; arrived_ = now;
  }
  void poll(double now) {
    if (!pending_) return;
    if (pending_stats_) poll_.add(std::max(0.0, now - arrived_));
    pending_ = false;
  }
  bool due(double now) const { return active_ && now - start_ >= 30000; }
  // The caller allocates the destination before locking. Only buffer pointers and
  // counters move here; the live pending arrival still belongs to the next poll.
  bool take_report(double now, Timing& completed) {
    if (!due(now)) return false;
    std::swap(arrival_, completed.arrival_); std::swap(poll_, completed.poll_);
    completed.lines_ = lines_; completed.hs_ = hs_;
    completed.negative_ = negative_; completed.overwritten_ = overwritten_;
    start_ = now; lines_ = hs_ = negative_ = overwritten_ = 0;
    arrival_.clear(); poll_.clear();
    return true;
  }
  void format(char* out, size_t size) {
    const double a50 = arrival_.percentile(.50), a95 = arrival_.percentile(.95);
    const double p50 = poll_.percentile(.50), p95 = poll_.percentile(.95);
    std::snprintf(out, size, "[relay-pads] 30 s: lines=%llu hs=%llu arrival-hs p50=%.3f p95=%.3f max=%.3f poll-arrival p50=%.3f p95=%.3f max=%.3f neg=%llu overwritten=%llu",
        (unsigned long long)lines_, (unsigned long long)hs_, a50, a95, arrival_.maximum,
        p50, p95, poll_.maximum, (unsigned long long)negative_, (unsigned long long)overwritten_);
  }
  bool report(double now, char* out, size_t size) {
    if (!due(now)) return false;
    format(out, size);
    start_ = now; lines_ = hs_ = negative_ = overwritten_ = 0;
    arrival_.clear(); poll_.clear();
    return true;
  }
  void reset() {
    active_ = pending_ = pending_stats_ = false;
    lines_ = hs_ = negative_ = overwritten_ = 0;
    arrival_.clear(); poll_.clear();
  }
 private:
  Samples arrival_, poll_;
  bool active_ = false, pending_ = false, pending_stats_ = false;
  double start_ = 0, arrived_ = 0;
  uint64_t lines_ = 0, hs_ = 0, negative_ = 0, overwritten_ = 0;
};
} // namespace yougame::relay_pads
