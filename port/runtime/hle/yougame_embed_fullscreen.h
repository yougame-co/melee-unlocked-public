// App-owned fullscreen state and request pacing; no window or clock calls.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <string_view>

namespace yougame::embed_fullscreen {
enum class Line { Other, Invalid, Accepted };

class State {
 public:
  static constexpr uint64_t kGapMs = 300;   // the app takes 4 a rolling second, by arrival
  static constexpr uint64_t kShowMs = 2000; // a choice shows this long while unanswered
  // Reader thread. Every answer is the app's state, a repeated one included.
  Line receive(std::string_view command) {
    if (command.substr(0, 10) != "fullscreen") return Line::Other;
    if (command != "fullscreen 0" && command != "fullscreen 1") return Line::Invalid;
    const auto previous = app_.load();
    app_.store(((previous & ~uint64_t{1}) + 2) | (command.back() == '1'));
    return Line::Accepted;
  }
  int display(bool embedded) const {
    const auto app = app_.load();
    return embedded && app ? int(app & 1) : -1;
  }
  // Render thread only. What the Display option shows: the player's choice until the app
  // answers (any embed line, a refusal included) or kShowMs pass, then the app's state.
  int shown(bool embedded, uint64_t now_ms) const {
    const auto app = app_.load();
    if (!embedded || !app) return -1;
    if (pending_ >= 0) return pending_;
    if (asked_ >= 0 && (app >> 1) == asked_rev_ && now_ms >= sent_at_ && now_ms - sent_at_ < kShowMs) return asked_;
    return int(app & 1);
  }
  // Render thread only. Return a request to emit, or -1. Times are monotonic ms.
  int request(bool embedded, bool on, uint64_t now_ms) {
    if (!sync(embedded)) return -1;
    pending_ = on;
    return flush(embedded, now_ms);
  }
  int flush(bool embedded, uint64_t now_ms) {
    if (!sync(embedded) || pending_ < 0) return -1;
    if (sent_ && (now_ms < sent_at_ || now_ms - sent_at_ < kGapMs)) return -1;
    const int request = pending_;
    pending_ = -1;
    sent_ = true;
    sent_at_ = now_ms;
    asked_ = request;
    asked_rev_ = seen_ >> 1;
    return request;
  }
 private:
  bool sync(bool embedded) {
    const auto app = app_.load();
    // The player's last unsent choice survives an answer to an earlier request; it is dropped
    // only when the app already shows it (a refusal answers 0, so a queued 0 goes too).
    if (!embedded || (app != seen_ && app != 0 && pending_ == int(app & 1))) pending_ = -1;
    seen_ = app;
    return embedded && app != 0;
  }
  std::atomic<uint64_t> app_{0}; // zero: no app answer; low bit: state; rest: revision
  uint64_t seen_ = 0, sent_at_ = 0, asked_rev_ = 0;
  int pending_ = -1, asked_ = -1;
  bool sent_ = false;
};

inline State state;
} // namespace yougame::embed_fullscreen
