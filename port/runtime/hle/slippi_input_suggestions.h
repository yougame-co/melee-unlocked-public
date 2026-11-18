// Slippi 3.7.0 input suggestions, the receiving side and the shared rules. A player's newest controller
// state is sent between frames as a suggestion stamped with the pad frame it first applies to; a client
// that has to predict that player's input uses the newest applicable suggestion instead of repeating
// the last input it received. Suggestions only ever feed predictions: the game records every prediction
// and rolls back when the real input differs. The rules here are Slippi's own, so this build and Slippi
// Dolphin judge and use each other's suggestions the same way. Header only, no host services: the unit
// test includes it alone. The sending side is the early input sampler (host/early_input.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace slippi::suggestions {

constexpr int kPadBytes = 8;            // buttons (big endian), stick x/y, C-stick x/y, L, R
constexpr int kReportBytes = 12;        // the game's whole pad report
constexpr int kPredictedFrames = 7;     // frames after a player's latest received frame the game can ask for
// The game zeroes every input polled before this frame less the delay, so nothing earlier is suggested.
constexpr int32_t kStartSyncFrame = 78;
constexpr uint8_t kTriggerDeadzone = 42;        // at or below: released, to the game's rollback comparison
constexpr int32_t kMaxSuggestionLeadFrames = 120;   // further past the latest real input: garbage
constexpr size_t kMaxStoredSuggestions = 512;
// A player whose suggestions do not match their real inputs only makes the game look worse. Each
// suggestion is checked once, against the real input for the frame it targets; a mistake the player's
// previous input did not make adds its cost to a bucket that drains one point every 200 frames. Over 15
// points, that player's suggestions are ignored for the rest of the game.
constexpr uint32_t kIgnoreAfterPoints = 15;
constexpr uint32_t kFramesPerDrainedPoint = 200;
constexpr uint32_t kWrongButtonCost = 3;
constexpr uint32_t kFartherAnalogCost = 1;

// The digital buttons `predicted` has wrong compared with `real` (the ones the game's rollback
// comparison looks at), as a bit mask.
inline uint16_t wrong_buttons(const uint8_t predicted[kPadBytes], const uint8_t real[kPadBytes]) {
  return (uint16_t)((((predicted[0] ^ real[0]) & 0x1F) << 8) | ((predicted[1] ^ real[1]) & 0x7F));
}
// How far `predicted`'s sticks and triggers are from `real`'s: the largest difference on either axis of
// each stick plus the largest on either trigger; two triggers inside the deadzone count as equal.
inline uint32_t analog_error(const uint8_t predicted[kPadBytes], const uint8_t real[kPadBytes]) {
  auto axis = [&](int i) { return (uint32_t)std::abs((int)(int8_t)predicted[i] - (int)(int8_t)real[i]); };
  auto trigger = [&](int i) {
    const uint8_t a = predicted[i], b = real[i];
    return a <= kTriggerDeadzone && b <= kTriggerDeadzone ? 0u : (uint32_t)(a > b ? a - b : b - a);
  };
  return std::max(axis(2), axis(3)) + std::max(axis(4), axis(5)) + std::max(trigger(6), trigger(7));
}
// Whether `next` is worth suggesting after `prev` was sent: only changes that can start a new action.
// Pressing or releasing A, B, X, Y, L, R, Z or Start; an analog trigger crossing the light shield line
// either way; either stick leaving the deadzone on either axis; the control stick reaching dash, tap
// jump or crouch range; the C-stick reaching a smash. Moving back out of a range is not sent: the real
// input brings that.
inline bool is_meaningful_change(const uint8_t prev[kPadBytes], const uint8_t next[kPadBytes]) {
  if (((prev[0] ^ next[0]) & 0x1F) != 0 || ((prev[1] ^ next[1]) & 0x70) != 0) return true;
  for (int i = 6; i < 8; ++i)
    if ((prev[i] > kTriggerDeadzone) != (next[i] > kTriggerDeadzone)) return true;
  // The sticks as the game sees them, in units of 1/80, each pulled back inside the unit circle.
  auto sticks = [](const uint8_t pad[kPadBytes], float out[4]) {
    auto clamp = [](uint8_t bx, uint8_t by, float& x, float& y) {
      x = (float)(int8_t)bx; y = (float)(int8_t)by;
      const float scale = std::min(80.0f / std::hypot(x, y), 1.0f);
      x *= scale; y *= scale;
    };
    clamp(pad[2], pad[3], out[0], out[1]);
    clamp(pad[4], pad[5], out[2], out[3]);
  };
  float before[4], after[4];
  sticks(prev, before); sticks(next, after);
  struct Range { int axis; bool (*inside)(float); };
  static const Range ranges[] = {
      {0, +[](float v) { return std::fabs(v) >= 23.0f; }},   // 0.2875, past the deadzone
      {1, +[](float v) { return std::fabs(v) >= 23.0f; }},
      {0, +[](float x) { return x >= 64.0f; }},    // dash right, 0.8
      {0, +[](float x) { return x <= -64.0f; }},   // dash left
      {1, +[](float y) { return y >= 53.0f; }},    // tap jump, 0.6625
      {1, +[](float y) { return y <= -56.0f; }},   // crouch, -0.7
      {2, +[](float v) { return std::fabs(v) >= 23.0f; }},
      {3, +[](float v) { return std::fabs(v) >= 23.0f; }},
      {2, +[](float x) { return x >= 64.0f; }},    // side smashes
      {2, +[](float x) { return x <= -64.0f; }},
      {3, +[](float y) { return y >= 53.0f; }},    // up and down smash
      {3, +[](float y) { return y <= -53.0f; }},
  };
  for (const Range& r : ranges)
    if (!r.inside(before[r.axis]) && r.inside(after[r.axis])) return true;
  return false;
}

// One remote player's suggestions: builds the predictions handed to the game from them and stops using
// them if they keep disagreeing with the player's real inputs.
class Remote {
 public:
  void on_suggestion(int32_t target_frame, uint32_t seq, const uint8_t pad[kPadBytes]) {
    const int32_t latest = has_real_ ? latest_frame_ : 0;
    if (ignored_ || target_frame <= latest || target_frame > latest + kMaxSuggestionLeadFrames) return;
    if (suggestions_.size() >= kMaxStoredSuggestions) suggestions_.pop_front();
    Suggestion s; s.target_frame = target_frame; s.seq = seq; std::memcpy(s.pad, pad, kPadBytes);
    suggestions_.push_back(s);
  }
  // The input the game should use for each of the frames after `latest_frame`: the newest applicable
  // suggestion, or `latest` (the latest received input) when there is none.
  void fill_predictions(int32_t latest_frame, const uint8_t latest[kReportBytes], uint8_t out[kPredictedFrames][kReportBytes]) const {
    for (int k = 0; k < kPredictedFrames; ++k) {
      std::memcpy(out[k], latest, kReportBytes);
      if (const Suggestion* s = best(latest_frame, latest_frame + 1 + k)) {
        std::memset(out[k], 0, kReportBytes);   // suggestions only have the inputs; the rest of the report is zero
        std::memcpy(out[k], s->pad, kPadBytes);
      }
    }
  }
  // True when this input is the one that got the player's suggestions ignored.
  bool on_real_input(int32_t frame, const uint8_t pad[kPadBytes]) {
    const bool had = has_real_;
    const int32_t previous_frame = latest_frame_;
    uint8_t previous_pad[kPadBytes];
    std::memcpy(previous_pad, latest_pad_, kPadBytes);
    if (had && frame <= previous_frame) return false;
    has_real_ = true; latest_frame_ = frame; std::memcpy(latest_pad_, pad, kPadBytes);
    // The suggestion in effect for this frame against what the player actually did.
    if (worse_bucket_ > 0) --worse_bucket_;
    if (const Suggestion* s = best(frame - 1, frame)) {
      if (had && previous_frame == frame - 1) {
        const uint32_t cost = worse_cost(previous_pad, s->pad, pad);
        worse_bucket_ += cost * kFramesPerDrainedPoint;
        if (cost) {
          mistake_.frame = frame; mistake_.cost = cost; mistake_.seq = s->seq;
          std::memcpy(mistake_.suggested, s->pad, kPadBytes); std::memcpy(mistake_.previous, previous_pad, kPadBytes); std::memcpy(mistake_.real, pad, kPadBytes);
          ++mistakes_;
        }
      }
    }
    // Nothing at or before the latest real input is needed for predictions any more.
    suggestions_.erase(std::remove_if(suggestions_.begin(), suggestions_.end(), [&](const Suggestion& s) { return s.target_frame <= frame; }),
                       suggestions_.end());
    if (ignored_ || worse_bucket_ <= kIgnoreAfterPoints * kFramesPerDrainedPoint) return false;
    ignored_ = true;
    suggestions_.clear();
    return true;
  }
  bool ignored() const { return ignored_; }
  // The last suggestion that cost points, and how many did (for a test trace).
  struct Mistake { int32_t frame = 0; uint32_t cost = 0, seq = 0; uint8_t suggested[kPadBytes] = {}, previous[kPadBytes] = {}, real[kPadBytes] = {}; };
  const Mistake& mistake() const { return mistake_; }
  uint64_t mistakes() const { return mistakes_; }
  size_t stored() const { return suggestions_.size(); }

 private:
  struct Suggestion { int32_t target_frame = 0; uint32_t seq = 0; uint8_t pad[kPadBytes] = {}; };
  // The newest suggestion that applies to `frame` when the latest real input is for `latest_frame`: a
  // suggestion for frame T describes the input from T onward, so it applies when T <= frame, and it is
  // only newer than the real inputs when T > latest_frame.
  const Suggestion* best(int32_t latest_frame, int32_t frame) const {
    const Suggestion* found = nullptr;
    for (const Suggestion& s : suggestions_)
      if (s.target_frame > latest_frame && s.target_frame <= frame && (!found || s.seq >= found->seq)) found = &s;
    return found;
  }
  // What a suggestion costs for being further from the real input than the player's previous input was.
  static uint32_t worse_cost(const uint8_t previous[kPadBytes], const uint8_t suggested[kPadBytes], const uint8_t real[kPadBytes]) {
    const uint16_t previous_wrong = wrong_buttons(previous, real), suggested_wrong = wrong_buttons(suggested, real);
    if (suggested_wrong & ~previous_wrong) return kWrongButtonCost;
    if (suggested_wrong == previous_wrong && analog_error(suggested, real) > analog_error(previous, real)) return kFartherAnalogCost;
    return 0;
  }
  std::deque<Suggestion> suggestions_;
  bool has_real_ = false;
  int32_t latest_frame_ = 0;
  uint8_t latest_pad_[kPadBytes] = {};
  uint32_t worse_bucket_ = 0;   // in frames' worth of draining
  Mistake mistake_;
  uint64_t mistakes_ = 0;
  bool ignored_ = false;
};

// The game in progress. One mutex: it is reached from the game thread (frames, predictions) and the
// network thread (inputs and suggestions received from peers).
struct Game {
  bool enabled = false;   // use the opponents' suggestions for predictions
  std::vector<Remote> remotes;
  uint64_t received = 0;          // suggestions that arrived from the other players
  uint64_t predicted_frames = 0;  // replies in which a suggestion stood in for the latest input
};
// For the end-of-game log line.
struct Stats { bool enabled = false; uint64_t received = 0, predicted_frames = 0; int ignored_players = 0; };
inline std::mutex& game_mutex() { static std::mutex m; return m; }
inline Game*& game_slot() { static Game* g = nullptr; return g; }

// Starts tracking a new game, replacing the previous one.
inline void start_game(bool enabled, int remote_player_count) {
  std::lock_guard<std::mutex> lock(game_mutex());
  delete game_slot();
  Game* g = new Game();
  g->enabled = enabled;
  g->remotes.resize((size_t)std::clamp(remote_player_count, 0, 3));
  game_slot() = g;
}
inline void end_game() {
  std::lock_guard<std::mutex> lock(game_mutex());
  delete game_slot();
  game_slot() = nullptr;
}
inline void on_remote_suggestion(size_t remote_idx, int32_t target_frame, uint32_t seq, const uint8_t pad[kPadBytes]) {
  std::lock_guard<std::mutex> lock(game_mutex());
  Game* g = game_slot();
  if (g && remote_idx < g->remotes.size()) { ++g->received; g->remotes[remote_idx].on_suggestion(target_frame, seq, pad); }
}
// True when this input got the player's suggestions ignored for the rest of the game.
inline bool on_remote_input(size_t remote_idx, int32_t frame, const uint8_t pad[kPadBytes]) {
  std::lock_guard<std::mutex> lock(game_mutex());
  Game* g = game_slot();
  return g && remote_idx < g->remotes.size() && g->remotes[remote_idx].on_real_input(frame, pad);
}
// Forgets a remote player's suggestions and inputs, for when they start a new game.
inline void reset_remote(size_t remote_idx) {
  std::lock_guard<std::mutex> lock(game_mutex());
  Game* g = game_slot();
  if (g && remote_idx < g->remotes.size()) g->remotes[remote_idx] = Remote();
}
// The inputs the game should use for each frame after `latest_frame` if it has to predict them. `latest`
// is the latest input received from the player: what every slot holds when no suggestion applies.
inline void fill_predictions(size_t remote_idx, int32_t latest_frame, bool is_disconnected, const uint8_t latest[kReportBytes],
                             uint8_t out[kPredictedFrames][kReportBytes]) {
  for (int k = 0; k < kPredictedFrames; ++k) std::memcpy(out[k], latest, kReportBytes);
  std::lock_guard<std::mutex> lock(game_mutex());
  Game* g = game_slot();
  if (!g || !g->enabled || is_disconnected || remote_idx >= g->remotes.size()) return;
  g->remotes[remote_idx].fill_predictions(latest_frame, latest, out);
  if (std::memcmp(out[0], latest, kPadBytes) != 0 || std::memcmp(out[kPredictedFrames - 1], latest, kPadBytes) != 0) ++g->predicted_frames;
}
// Test trace: the count of costly suggestions from a player and the last one.
inline uint64_t mistakes(size_t remote_idx, Remote::Mistake* last) {
  std::lock_guard<std::mutex> lock(game_mutex());
  const Game* g = game_slot();
  if (!g || remote_idx >= g->remotes.size()) return 0;
  if (last) *last = g->remotes[remote_idx].mistake();
  return g->remotes[remote_idx].mistakes();
}
inline Stats stats() {
  std::lock_guard<std::mutex> lock(game_mutex());
  Stats s;
  if (const Game* g = game_slot()) {
    s.enabled = g->enabled; s.received = g->received; s.predicted_frames = g->predicted_frames;
    for (const Remote& r : g->remotes) if (r.ignored()) ++s.ignored_players;
  }
  return s;
}

}  // namespace slippi::suggestions
