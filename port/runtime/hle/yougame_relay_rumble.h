// App rumble protocol and bounded change/refresh mailbox. No platform I/O.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
namespace yougame::relay_pads {
struct RumbleCommand {
  int seat = 0, strong = 0, weak = 0, ms = 0;
};
inline std::string rumble_line(RumbleCommand c) {
  c.seat = std::clamp(c.seat, 0, 3);
  c.strong = std::clamp(c.strong, 0, 65535); c.weak = std::clamp(c.weak, 0, 65535);
  c.ms = c.strong || c.weak ? std::clamp(c.ms, 0, 5000) : 0;
  char line[128];
  std::snprintf(line, sizeof line, "yg {\"rumble\":{\"seat\":%d,\"strong\":%d,\"weak\":%d,\"ms\":%d}}",
      c.seat, c.strong, c.weak, c.ms);
  return line;
}
class Rumble {
 public:
  void set(int seat, bool on, double now) {
    if (seat < 0 || seat >= 4) return;
    auto& s = seats_[seat];
    if (s.on == on) return;
    s.on = on; s.next = now + 4000;
    put({seat, on ? 65535 : 0, on ? 32768 : 0, on ? 5000 : 0});
  }
  void pulse(int seat, double now) {
    if (seat < 0 || seat >= 4) return;
    auto& s = seats_[seat]; s.on = false; s.stop = now + 300;
    put({seat, 65535, 65535, 300});
  }
  void tick(double now) {
    for (int i = 0; i < 4; ++i) {
      auto& s = seats_[i];
      if (s.stop && now >= s.stop) { s.stop = 0; s.on = false; put({i, 0, 0, 0}); }
      else if (s.on && now >= s.next) { s.next = now + 4000; put({i, 65535, 32768, 5000}); }
    }
  }
  bool take(RumbleCommand& out) {
    if (!count_) return false;
    out = queue_[head_]; head_ = (head_ + 1) % queue_.size(); --count_; return true;
  }
 private:
  struct Seat { bool on = false; double next = 0, stop = 0; };
  std::array<Seat, 4> seats_{};
  std::array<RumbleCommand, 64> queue_{};
  size_t head_ = 0, count_ = 0;
  void put(RumbleCommand c) {
    // A stalled stdout cannot grow memory or block PADRead. On saturation drop the oldest
    // command; subsequent changes (especially stops) and refreshes remain deliverable.
    if (count_ == queue_.size()) { head_ = (head_ + 1) % queue_.size(); --count_; }
    queue_[(head_ + count_++) % queue_.size()] = c;
  }
};
} // namespace yougame::relay_pads
