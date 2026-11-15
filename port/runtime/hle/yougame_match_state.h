// Best-effort host presence. Nothing here is part of a rollback snapshot or a game rule.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace yougame::match_state {
constexpr uint64_t kRenewMs = 4 * 60 * 1000;

struct Pending {
  uint64_t sent = 0, renew = 0;
  int take(uint64_t edges, uint64_t now) {
    if (sent < edges) {
      ++sent;
      renew = now + kRenewMs;
      return int(sent & 1);
    }
    if ((sent & 1) && now >= renew) { renew = now + kRenewMs; return 1; }
    return -1;
  }
};

class Worker {
 public:
  ~Worker() { stop(); }
  // Called at host boot, before any game frame. send may block or refuse on this worker only.
  void start(std::function<void(int)> send) {
    thread_ = std::thread([this, send = std::move(send)] {
      Pending pending;
      while (!stopping_.load(std::memory_order_relaxed)) {
        const auto edges = edges_.load(std::memory_order_acquire);
        // Edges alternate open/closed. Replay every edge, even an entire match or a same-ticket
        // rematch that happened while the app was slow. A closed edge can never be coalesced away.
        const int open = pending.take(edges, now_ms());
        if (open >= 0) {
          try { send(open); } catch (...) {} // refusals never enter gameplay
          continue;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stopping_.load(); });
      }
      if (pending.sent & 1) { try { send(0); } catch (...) {} }
    });
  }
  // The simulation/host hooks do only this lock-free atomic handoff. No allocation, clock,
  // thread creation, mutex, callback, pipe, socket or filesystem access on their thread.
  void set(bool open) {
    auto old = edges_.load(std::memory_order_relaxed);
    while (bool(old & 1) != open && !edges_.compare_exchange_weak(
        old, old + 1, std::memory_order_release, std::memory_order_relaxed)) {}
  }
  void stop() {
    stopping_.store(true);
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
  }
 private:
  static_assert(std::atomic<uint64_t>::is_always_lock_free, "Match hints must never lock a frame");
  static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  std::atomic<uint64_t> edges_{0};
  std::atomic<bool> stopping_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
};
} // namespace yougame::match_state
