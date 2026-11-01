// DXGI present policy, independent of Windows headers for host-only tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
namespace yougame::box_present {
inline constexpr unsigned allow_tearing = 0x200;
struct Choice { unsigned interval, flags; bool compositor = false, agent = false; };
inline Choice choose(bool embedded, bool vsync, bool fullscreen, bool agent_override,
                     bool flip = true, bool paced = true, bool tearing = true, bool vrr_sync = false,
                     bool toplevel = false) {
  // Verified in native platform performance review.
  // 10-30, 144 Hz: p50 126 vs 135 fps, ~50% more repeated frames, same latency), so it is an agent-test
  // experiment only (agent-test.json "boxPresent":"compositor").
  if (embedded && !toplevel && vsync && !fullscreen && flip && paced && agent_override) return {0, 0, true, true};
  const bool synced = vsync || vrr_sync;
  return {synced ? 1u : 0u, !synced && tearing ? allow_tearing : 0u};
}
#ifdef YOUGAME_AGENT_TEST_HOOK
inline std::atomic<bool> agent_compositor{false};
#endif
inline bool agent_override() {
#ifdef YOUGAME_AGENT_TEST_HOOK
  return agent_compositor.load(std::memory_order_relaxed);
#else
  return false;
#endif
}
inline const char* message(Choice choice) {
  return choice.compositor ? "present: embedded v-sync via compositor (interval 0, agent test)" : nullptr;
}
inline const char* announcement(Choice choice) {
  static std::atomic<bool> said{false};
  const auto line = message(choice);
  return line && !said.exchange(true) ? line : nullptr;
}
} // namespace yougame::box_present
