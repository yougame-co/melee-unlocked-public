// Hook-only outgoing input stress arm. No ENet, platform or gameplay RNG dependencies.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef YOUGAME_AGENT_TEST_HOOK
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>

namespace yougame::net_test {
struct Config {
  uint32_t delay_ms = 0, jitter_ms = 0;
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  bool squad_local_test = false, squad_cancel_test = false;
#endif
  bool enabled() const { return delay_ms != 0; }
};
namespace detail { inline std::unique_ptr<const Config> installed; }
#if MU_SQUAD
// agent-test.json squadRefuse:true: this copy refuses every Friends Squad Strike setup as if a fighter
// were over its budget, so a live pair can time the host's return to the select. Set at startup.
inline std::atomic<bool> squad_refuse{false};
#endif
// Startup only, before matchmaking or worker threads. Consumers get an immutable value.
inline void install(Config config) {
  if (!detail::installed) detail::installed = std::make_unique<const Config>(config);
}
inline const Config& config() {
  static const Config off;
  return detail::installed ? *detail::installed : off;
}
inline bool mode_allowed(const Config& c, int mode) {
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  if (c.squad_local_test && mode == 1) return true;
#endif
  return !c.enabled() || mode == 2;
}
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
// Strict CLI syntax; the same positive delay and additive, ordered jitter as agent-test.json.
inline bool parse_squad_delay(const char* text, Config* out) {
  if (!text || !out) return false;
  const char* p = text;
  auto number = [&p](uint32_t& n, uint32_t limit) {
    n = 0;
    if (*p < '0' || *p > '9') return false;
    do { n = n * 10 + uint32_t(*p++ - '0'); if (n > limit) return false; }
    while (*p >= '0' && *p <= '9');
    return true;
  };
  Config value;
  if (!number(value.delay_ms, 250) || !value.delay_ms) return false;
  if (*p == ':') { ++p; if (!number(value.jitter_ms, 100)) return false; }
  if (*p) return false;
  value.squad_local_test = true;
  *out = value;
  return true;
}
#endif
inline const char* mode_name(int mode) {
  switch (mode) {
    case 0: return "ranked"; case 1: return "unranked"; case 2: return "direct";
    case 3: return "teams"; case 4: return "party"; default: return "unknown";
  }
}
inline bool input_message(uint8_t id) { return id == 0x80 || id == 0xE1; }

// Owned exclusively by the network thread. Times are monotonic integer milliseconds;
// timestamps are captured at SendAsync enqueue, not when this queue is drained.
// Each input gets delay + U[0,jitter], clamped to the preceding input deadline.
// Control packets bypass pending inputs. A reset also resets the private fixed-seed PRNG.
template<class T> class DelayQueue {
 public:
  explicit DelayQueue(Config config) : config_(config) {}
  void reset(uint64_t generation) {
    if (generation_ == generation) return;
    pending_.clear(); last_due_ = 0; generation_ = generation; state_ = 0x9E3779B97F4A7C15ull;
  }
  // True consumes the value (including obsolete inputs); false leaves it untouched.
  bool push(uint8_t id, uint64_t enqueued_ms, uint64_t generation, T&& value) {
    if (!config_.enabled() || !input_message(id)) return false;
    if (generation != generation_) return true;
    state_ ^= state_ << 13; state_ ^= state_ >> 7; state_ ^= state_ << 17;
    uint64_t due = enqueued_ms + config_.delay_ms + state_ % (config_.jitter_ms + 1);
    due = std::max(due, last_due_); last_due_ = due;
    pending_.push_back({due, std::move(value)});
    return true;
  }
  template<class Send> bool drain(uint64_t now_ms, Send send) {
    bool sent = false;
    while (!pending_.empty() && pending_.front().due <= now_ms) {
      send(pending_.front().value); pending_.pop_front(); sent = true;
    }
    return sent;
  }
  uint32_t wait_ms(uint64_t now_ms, uint32_t normal) const {
    if (pending_.empty()) return normal;
    return static_cast<uint32_t>(std::min<uint64_t>(normal,
        pending_.front().due > now_ms ? pending_.front().due - now_ms : 0));
  }
  size_t size() const { return pending_.size(); }
 private:
  struct Held { uint64_t due; T value; };
  const Config config_;
  uint64_t last_due_ = 0, generation_ = 0, state_ = 0x9E3779B97F4A7C15ull;
  std::deque<Held> pending_;
};
} // namespace yougame::net_test
#endif
