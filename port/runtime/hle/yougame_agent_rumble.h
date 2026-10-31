// Staging-only pulse scheduler, driven by the relay writer's clock.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef YOUGAME_AGENT_TEST_HOOK
#include <array>
#include <string>
namespace yougame::agent_test {
class RumblePulse {
 public:
  void arm(double boot) { boot_ = boot; armed_ = true; }
  template<class Send, class Log>
  void tick(double now, const std::array<bool, 4>& present, Send send, Log log) {
    if (!armed_ || now - boot_ < 5000) return;
    bool any = false;
    for (int seat = 0; seat < 4; ++seat) if (present[seat]) any = true;
    if (now - boot_ >= 60000) {
      log("agent test: rumble pulse: no relayed seats"); armed_ = false;
    } else if (any) {
      for (int seat = 0; seat < 4; ++seat) if (present[seat]) {
        send(seat);
        const auto line = "agent test: rumble pulse sent to slot " + std::to_string(seat);
        log(line.c_str());
      }
      armed_ = false;
    }
  }
 private:
  bool armed_ = false;
  double boot_ = 0;
};
} // namespace yougame::agent_test
#endif
