// Arena spectator feed: a fighter's live recording, trimmed to what the built-in replay viewer
// plays, sent to a friend who watches the match as it happens.
//
// The source is the same event stream the Slippi spectator server gets (the recording's event
// sink). The Sender keeps each frame's start event and pre-frame events and lets a frame go only
// once the game says it is final, each frame once, in order: a frame the game simulates again after
// a rollback replaces the copy that was waiting. Items, stage events and the code list are not
// sent, and post-frame events only for the first frame (the viewer reads a starting Sheik from
// them). The Receiver writes what arrives as a recording that is still being written, the form
// the Source Port's viewer already plays (source_host.cpp h_replay_frame), and takes a frame from
// whichever fighter delivers it first.
//
// Neither class opens a socket or takes a lock. The Sender is called on the thread that records;
// the Receiver on the thread that hands it messages.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_replay_stream.h"
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace slippi::arena_feed {

constexpr uint8_t kVersion = 1;
// The recording's event sizes this build writes (native_replay_stream.cpp begin_slippi).
constexpr uint16_t kFrameStartSize = 12, kPreFrameSize = 66, kPostFrameSize = 84, kGameEndSize = 6;
constexpr int32_t kFirstFrame = -123;

enum Kind : uint8_t { kStart = 1, kFrame = 2, kEnd = 3 };

// A game's tag: a hash of its Game Start payload's settings and seed (the first 0x1A0 bytes), which
// both fighters' recordings share (the player names after them are each side's own). Never 0.
uint32_t game_tag(const uint8_t* start, size_t size);

// Who won a game, from its Game End payload (method, LRA-Start port, each port's placement) and,
// for doubles, each port's team from its Game Start: 1 the player on `my_port` (or their team)
// won, 0 they lost, -1 no result (a no contest, a tie, a payload too short).
int my_result(const uint8_t* game_end, size_t size, int my_port, const uint8_t port_team[4], bool doubles);
// Each port's team from a Game Start payload (0 for a short one).
void port_teams(const uint8_t* game_start, size_t size, uint8_t out[4]);

class Sender {
 public:
  // One feed message, whole. Called from event() and stop().
  using Out = std::function<void(const uint8_t* data, uint32_t size)>;
  void set_out(Out out) { out_ = std::move(out); }
  // Stamped on every message, so a late message of the last game is not taken for this one. 0 (the
  // default): each game's own game_tag.
  void set_game_tag(uint32_t tag) { fixed_tag_ = tag; tag_ = tag; }
  uint32_t tag() const { return tag_; }
  bool in_game() const { return started_; }
  // Every frame's post-frame events too (about twice the upload). Off: the first frame's only.
  void set_full_post(bool on) { full_post_ = on; }
  // One recording event as the sink gives it: the command byte, then its payload. The command
  // table event (0x35) comes first.
  void event(const uint8_t* data, uint32_t size);
  // The feed ends with no Game End (the fighter left the match or the room).
  void stop();

  struct Stats {
    uint64_t bytes = 0, messages = 0;
    uint32_t frames = 0;       // frames sent
    uint32_t superseded = 0;   // waiting frames replaced by a re-simulated copy
    uint32_t late = 0;         // re-simulated copies of frames already sent (must stay 0)
    size_t max_waiting = 0;    // most frames held back at once
  };
  const Stats& stats() const { return stats_; }

 private:
  void send(const std::vector<uint8_t>& message);
  void release(int32_t through);
  Out out_;
  uint32_t tag_ = 0, fixed_tag_ = 0;
  bool full_post_ = false;
  bool started_ = false;
  uint16_t pre_size_ = kPreFrameSize, post_size_ = kPostFrameSize;
  bool open_ = false;                 // between a frame start and its frame end
  int32_t open_frame_ = 0;
  std::vector<uint8_t> open_events_;
  std::map<int32_t, std::vector<uint8_t>> waiting_;   // complete frames not yet final
  bool any_sent_ = false;
  int32_t next_ = kFirstFrame;        // the next frame to send
  Stats stats_;
};

class Receiver {
 public:
  // The live recording goes to `path` (UTF-8). `codes` is this build's own code list for an online
  // recording: the feed does not carry it. Messages with another tag are ignored; tag 0 takes the
  // first start's.
  bool open(const std::string& path, std::vector<uint8_t> codes, uint32_t game_tag);
  // One feed message from fighter `source`. False when the message is malformed or does not fit
  // this build (nothing is written for it).
  bool message(int source, const uint8_t* data, uint32_t size);
  // Closes the file. A match that got no Game End is then a recording nobody writes any more, which
  // the viewer ends at its last frame.
  void close();

  bool started() const { return started_; }
  uint32_t tag() const { return tag_; }
  bool ended() const { return ended_; }
  int32_t next_frame() const { return next_; }   // the first frame not written yet
  const std::string& error() const { return error_; }
  struct Stats {
    uint32_t frames = 0;       // frames written
    uint32_t duplicates = 0;   // frames another fighter had already delivered
    uint32_t gaps = 0;         // frames past the next one (dropped; must stay 0)
    uint32_t from[4] = {};     // frames taken from each source
  };
  const Stats& stats() const { return stats_; }

 private:
  bool fail(const char* reason);
  std::ofstream file_;
  std::vector<uint8_t> codes_;
  uint32_t tag_ = 0;
  NativeReplayStream stream_;
  bool started_ = false, ended_ = false;
  int32_t next_ = kFirstFrame;
  std::map<uint16_t, uint8_t> character_;   // slot << 8 | follower -> the post-frame's character
  std::string error_;
  Stats stats_;
};

}  // namespace slippi::arena_feed
