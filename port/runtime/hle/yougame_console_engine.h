// Session input ownership. Match prediction/replay belongs to mu_online.c.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yougame_console.h"
#include "yougame_console_rollback_default.h"
#include <array>
#include <functional>
#include <filesystem>
#include <map>

namespace yougame::console {
using Pads = std::array<MuPadStatus, kMaxPorts>;
struct EngineHooks {
  std::function<uint64_t()> now_ms; // monotonic wall time, ONLY for waiting/diagnostics
  std::function<void()> responsive; // window/app events only; never advance the game
  std::function<void(const std::string&)> log;
  std::function<void(const char*)> notice;
};

// Independently instantiable so two engines can be exercised without a host or a network.
class Engine {
public:
  bool begin(std::unique_ptr<SessionLink>, const SessionConfig&, EngineHooks = {},
             bool rollback = kConsoleRollbackDefault);
  bool active() const { return active_; }
  const SessionConfig& config() const { return config_; }
  uint32_t frame() const { return frame_; }
  // Console-only and off by default. Set before match entry.
  void enable_rollback(bool enabled) {
    if (!match_) {
      service_ticks_ = ticks_; service_next_retrace_ = next_retrace_; service_retraces_ = retraces_;
      rollback_enabled_ = enabled;
    }
  }
  bool rollback_enabled() const { return active_ && rollback_enabled_; }
  bool match_active() const { return active_ && match_; }
  bool menus_lockstep() const { return active_ && !match_; }
  bool needs_sample() const {
    return active_ && !local_.count((started_ ? frame_ + 1 : 0) + config_.input_delay);
  }
  uint32_t match_epoch() const { return match_epoch_; }
  uint32_t match_origin() const { return match_origin_; }
  bool begin_match(); // next body is b=0; never resets the continuous session timeline
  void end_match();   // lifecycle cleanup; normal exits use commit_match
  bool confirm(uint32_t body); // wait for causal inputs, without admitting/sampling another body
  // Exclusive body frontier, reported ONLY by the predictor/runner after comparison/replay.
  bool validated(uint32_t boundary);
  bool boundary(uint32_t body, const MuStatePod&); // after corrections, before pad injection
  bool finish_body(uint32_t body);
  bool commit_match(uint32_t body, const MuStatePod&); // terminal body has finished and been corrected
  void capture_clock(uint32_t body);
  bool restore_clock(uint32_t body);
  bool body_active() const { return match_active() && body_clock_; }
  struct RollbackStats { uint64_t loads = 0, replayed = 0, commits = 0; };
  const RollbackStats& rollback_stats() const { return rollback_stats_; }
  // Adapter to the existing native RXB predictor. previous excludes the not-yet-run body,
  // keeping its seven historical entries in bounds even at a depth-seven correction.
  bool inputs(uint32_t body, bool previous, MuConsoleInputs&);
  bool replay(uint32_t body); // history only: no sampling, sending, waiting or delay
  // Once per newly admitted body, BEFORE its pad queue is consumed; replay uses replay(). State is the boundary
  // before this frame, so frame 60's digest includes the completed bodies 0..59.
  bool advance(const Pads& hardware, const MuStatePod& state);
  // Repeated PADRead calls never sample, send, wait, or advance the frame.
  // Before begin this leaves every byte of hardware alone; after end only our virtual port lives.
  void read(const Pads& hardware, Pads& out) const;
  // Game-call-driven time only. Reads/polls make busy waits progress; a requested VI
  // adds exactly one 60 Hz period. Neither operation advances a lockstep body.
  static constexpr uint64_t frame_ticks = MU_TB_HZ / 60;
  static constexpr uint64_t poll_ticks = 2048;
  uint64_t ticks(uint64_t ordinary);
  uint64_t clock_ticks() const { return ticks_; } // alarm delivery must not read/step time
  uint64_t alarm_ticks() const { return rollback_enabled_ ? service_ticks_ : ticks_; }
  void poll();
  void retrace();
  void logical_retrace();
  void service_retrace();
  void service_clock(bool entering) { if (entering) ++service_depth_; else if (service_depth_) --service_depth_; }
  bool servicing() const { return rollback_enabled_ && service_depth_ != 0; }
  bool retrace_due() const { return active_ && ticks_ >= next_retrace_; }
  bool service_retrace_due() const { return active_ && alarm_ticks() >= service_next_retrace_; }
  uint32_t retrace_count() const { return servicing() ? service_retraces_ : retraces_; }
  uint64_t boot_ticks(uint64_t ordinary) const;
  void end(const std::string& reason, bool desync = false);
  static uint64_t state_digest(const MuStatePod&);
  static bool valid(const SessionConfig&);
private:
  uint64_t liveness_clock() const;
  void relink_notice();
  bool relink_shown_ = false;
  uint64_t relink_notice_at_ = 0;
  bool compare_digests();
  void candidate(uint32_t boundary, const MuStatePod&);
  bool publish(uint32_t corrected_boundary);
  int64_t confirmed_through() const;
  void receive(uint32_t through);
  bool compose(uint32_t frame, bool predict);
  void retain_history();
  void log(const std::string& text) const;
  SessionConfig config_;
  EngineHooks hooks_;
  std::unique_ptr<SessionLink> link_;
  bool active_ = false, started_ = false, ended_ = false;
  uint32_t frame_ = 0;
  uint32_t admitted_through_ = 0; // forward frontier, never rewound by replay()
  uint64_t ticks_ = 0, next_retrace_ = frame_ticks;
  uint32_t retraces_ = 0;
  // Hardware schedules never rewind. Only the game-visible clock belongs to checkpoints.
  uint64_t service_ticks_ = 0;
  uint64_t service_next_retrace_ = frame_ticks;
  uint32_t service_retraces_ = 0, service_depth_ = 0;
  struct Clock { uint64_t ticks, next_retrace; uint32_t retraces; };
  Clock body_start_{};
  std::map<uint32_t, Clock> clocks_;
  bool body_clock_ = false;
  RollbackStats rollback_stats_;
  Pads pads_{};
  std::map<uint32_t, MuPadStatus> local_;
  std::array<std::map<uint32_t, MuPadStatus>, kMaxPorts> remote_;
  std::array<int64_t, kMaxPorts> confirmed_{{-1, -1, -1, -1}};
  bool rollback_enabled_ = false, match_ = false;
  uint32_t match_origin_ = 0, match_epoch_ = 0;
  struct Check { uint64_t value; unsigned pending; uint64_t since; };
  std::map<uint32_t, Check> checks_;
  std::map<uint32_t, uint64_t> candidates_;
  int64_t published_through_ = -1;
  uint32_t validated_boundary_ = 0; // session boundary; receive() never advances it
  uint64_t waited_ = 0, max_wait_ = 0;
  // Per 600 frames: wall time (is the session at 60 fps?), and waits longer than one frame, by port.
  uint64_t window_began_ = 0, long_waits_ = 0;
  int long_port_ = 0;
  uint64_t menu_window_ = 0, menu_stalls_ = 0, menu_longest_ = 0;
  bool menu_window_active_ = false;
};

Engine& console_engine(); // simulation-thread owner
bool console_active();   // pure, thread-safe presence query
bool prepare_card(const std::filesystem::path& data, const SessionConfig&,
                  std::filesystem::path& card);
// Host entry point, called before source_port::run / the first simulation frame. Requires
// YOUGAME_DATA and a fresh console/<session_id>/card directory. Never calls start_session.
bool console_begin(std::unique_ptr<SessionLink>, const SessionConfig&);
} // namespace yougame::console
