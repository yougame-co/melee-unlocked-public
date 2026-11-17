// What the game is doing now (playing, paused or in a menu), for the YouGame button over it.
// Channel contract section 9: {"type":"gameState","state":"playing"|"paused"|"menu"}, fire-and-forget,
// sent only on a change; the latest state waits for hello and goes again after every reconnect.
// Best-effort host presence, like yougame_match_state.h: never part of a snapshot or a game rule.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace yougame::game_state {
enum : int { kNone = 0, kPlaying = 1, kPaused = 2, kMenu = 3 };

inline const char* name(int s) {
  return s == kPlaying ? "playing" : s == kPaused ? "paused" : s == kMenu ? "menu" : nullptr;
}

// MU's own menus over the game (Esc menu, F1 settings, Practice, the full-window launcher). The
// render thread publishes it every frame; the simulation thread reads it in classify's caller.
inline std::atomic<bool> overlay_menu_open{false};

// A live match scene (VS, sudden death, Training, single player, online) plays unless the game's
// pause (Start, the page's pause or frame advance) holds it. Everything else is a menu: title, main
// menu, character and stage select, results, and MU's own menus over a match.
inline int classify(bool match_scene, bool paused, bool overlay_menu) {
  if (!match_scene || overlay_menu) return kMenu;
  return paused ? kPaused : kPlaying;
}

// Which state goes to which connection. epoch is the connection's number (0: none up, hello not
// answered yet); a new connection hears the latest state once, even an unchanged one.
struct Pending {
  int sent = kNone;
  uint64_t epoch = 0;
  bool due(int want, uint64_t now_epoch) const {
    return want != kNone && now_epoch != 0 && (want != sent || now_epoch != epoch);
  }
  void done(int want, uint64_t now_epoch) { sent = want; epoch = now_epoch; }
};

class Worker {
 public:
  ~Worker() { stop(); }
  // epoch(): the live connection's number or 0. send(state): one request; its answer is ignored
  // (an older app's notYet or badRequest included). Both run on this worker only.
  void start(std::function<uint64_t()> epoch, std::function<void(int)> send) {
    thread_ = std::thread([this, epoch = std::move(epoch), send = std::move(send)] {
      Pending pending;
      while (!stopping_.load(std::memory_order_relaxed)) {
        const int want = want_.load(std::memory_order_acquire);
        uint64_t e = 0;
        try { e = epoch(); } catch (...) {}
        if (pending.due(want, e)) {
          try { send(want); } catch (...) {}   // no answer or a closed channel: the next epoch resends
          pending.done(want, e);
          continue;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        // 50 ms also folds a flicker (a one-frame scene step) into the state it settles on.
        wake_.wait_for(lock, std::chrono::milliseconds(50), [this] { return stopping_.load(); });
      }
    });
  }
  // Simulation thread: one lock-free store. No allocation, clock, pipe or callback there.
  void set(int state) { want_.store(state, std::memory_order_release); }
  int wanted() const { return want_.load(std::memory_order_acquire); }
  void stop() {
    stopping_.store(true);
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::atomic<int> want_{kNone};
  std::atomic<bool> stopping_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
};
}  // namespace yougame::game_state
