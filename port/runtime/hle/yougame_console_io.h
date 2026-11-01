// Deadline-bounded filesystem work shared by console startup and handoff.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <future>
#include <thread>
#include <cstdint>
#include <utility>

namespace yougame::console {
// A filesystem call cannot be interrupted portably. Give the worker owned arguments and
// abandon its result at the session deadline; a late staged file cannot override save-choice.json.
// Callbacks must own their dependencies (production captures none), never session stack references.
template<class Clock, class Cancelled, class T, class Work>
T card_io_until(Clock clock, Cancelled cancelled, uint64_t until, T fallback, Work work) {
  const auto now = clock();
  if (now >= until) return fallback;
  const auto wall_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(until-now);
  try {
    std::packaged_task<T()> task(std::move(work));
    auto result = task.get_future();
    std::thread(std::move(task)).detach();
    for (;;) {
      if (result.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
        if (clock() >= until || std::chrono::steady_clock::now() >= wall_until) return fallback;
        try { return result.get(); } catch (...) { return fallback; }
      }
      if (cancelled() || clock() >= until || std::chrono::steady_clock::now() >= wall_until) return fallback;
    }
  } catch (...) { return fallback; } // even worker allocation/start failure must not refuse START
}
} // namespace yougame::console
