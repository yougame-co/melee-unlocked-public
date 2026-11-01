// Blended Unranked: one Unranked search over YouGame's pool and Slippi's ("smart
// Unranked"). Launched by the YouGame app on the base listing with a usable Slippi login, the search
// goes to YouGame first when someone is already in its queue, otherwise to both at once; the first
// side to be given an opponent is the match and the other is cancelled.
// Pure: no sockets, threads or clock. Matchmaking::blendMatchmaking (slippi_net.cpp) drives it from
// two threads under one mutex and does what it says; the unit test drives it with a fake clock.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace yougame::blend {

enum class Side : uint8_t { YouGame, Slippi };
inline const char* name(Side side) { return side == Side::YouGame ? "YouGame" : "Slippi"; }

// YouGame's queue says on joining how many are searching there, this player included (the rooms
// Worker's "searching".waiting). Someone else there: YouGame alone this long, then Slippi too.
constexpr uint64_t kYouGameFirstMs = 15000;
// No word from the queue by then (a slow ticket, a queue that sent none): Slippi too.
constexpr uint64_t kQueueWordMs = 3000;

// The base listing's Unranked status line while it searches (the YouGame status the game shows over
// the character select). Remixes keep plain "searching" and show neither.
inline constexpr const char* kSearchBoth = "Searching Slippi + YouGame";
inline constexpr const char* kSearchYouGameOnly = "Searching YouGame players";

// The app's Slippi link line over the stdio bridge. Only the player's own connect code goes out, and
// only the characters a code can have (never a quote or a control byte into the JSON).
inline std::string slippi_link_line(bool linked, const std::string& tag) {
  if (!linked) return R"(yg {"slippi":{"linked":false}})";
  std::string clean;
  for (unsigned char c : tag)
    if (c > 0x20 && c < 0x7f && c != '"' && c != '\\') clean += char(c);
  return R"(yg {"slippi":{"linked":true,"tag":")" + clean + R"("}})";
}

inline std::string slippi_stop_reason(const std::string& error) {
  if (error.rfind("Slippi ", 0) == 0 && error.find(" is out. Update Melee Unlocked") != std::string::npos) return error;
  if (error.rfind("Invalid", 0) == 0) return "Slippi sent an invalid reply";
  if (error.rfind("Sign in", 0) == 0) return "Sign in to Slippi to search its players";
  return "Slippi isn't reachable right now";
}

class Coordinator {
 public:
  explicit Coordinator(uint64_t now) : start_(now) {}

  // YouGame's queue: a "searching" message. Only the first decides "YouGame first"; a later one that
  // says the others left (waiting 1) starts Slippi then, since nobody is left to match on YouGame.
  void searching(int waiting, uint64_t now) {
    if (over_for_new_work() || yougame_ != Run::Live) return;
    const bool first = !heard_;
    heard_ = true;
    if (slippi_ != Run::Off) {
      if (first) say("YouGame queue waiting " + std::to_string(waiting) + " -> both at once");
      return;
    }
    if (waiting >= 2) {
      if (first) {
        solo_until_ = now + kYouGameFirstMs;
        say("YouGame queue waiting " + std::to_string(waiting) + " -> YouGame first");
      }
      return;
    }
    say("YouGame queue waiting " + std::to_string(waiting) + " -> both at once");
    start_slippi(now);
  }

  // A side was given an opponent (YouGame: the queue named a room; Slippi: a ticket arrived). True:
  // this side is the match and the other is cancelled. False: decline it (the other side was first,
  // or the search is over); it is left at once, and casual Unranked costs nobody anything for it.
  bool matched(Side side, uint64_t now) {
    if (committed_ || stopped_ || run(side) != Run::Live) {
      say(std::string("declined a late ") + name(side) + " match");
      if (!committed_ || side != winner_) run(side) = Run::Done;   // the winner keeps connecting
      return false;
    }
    committed_ = true;
    winner_ = side;
    Run& other = run(side == Side::YouGame ? Side::Slippi : Side::YouGame);
    const bool cancelled = other == Run::Live;
    other = Run::Done;   // cancelled, or never to start
    start_pending_ = false;
    say(std::string("matched on ") + name(side) + " after " + std::to_string(now - start_) + " ms; " +
        (cancelled ? "cancelled the other" : "the other was not searching"));
    return true;
  }

  // A side's search ended without a match of its own (an error, a refused version, a lost
  // connection). Before anything committed, the other side goes on, and YouGame failing before
  // Slippi started starts Slippi now. The committed side failing is that side's own failure: the
  // caller does what it always did, and the cancelled side is never resumed.
  void failed(Side side, uint64_t now) {
    if (run(side) != Run::Live) return;   // cancelled, declined or already out
    run(side) = Run::Done;
    if (over_for_new_work()) return;
    if (side == Side::YouGame && slippi_ == Run::Off) start_slippi(now);
  }

  void tick(uint64_t now) {
    if (over_for_new_work() || slippi_ != Run::Off) return;
    if (!heard_ && now - start_ >= kQueueWordMs) start_slippi(now);
    else if (heard_ && solo_until_ && now >= solo_until_) start_slippi(now);
  }

  // The player left the search: both sides are cancelled and nothing commits after this.
  void stop() {
    stopped_ = true;
    start_pending_ = false;
    if (yougame_ == Run::Live) yougame_ = Run::Done;
    if (slippi_ == Run::Live) slippi_ = Run::Done;
  }

  // Slippi's search is due to start (once; never after a commit, a stop or its own failure).
  bool take_start_slippi() { return std::exchange(start_pending_, false) && slippi_ == Run::Live; }
  // Still searching (or, for the committed side, connecting): its work goes on.
  bool live(Side side) const { return (side == Side::YouGame ? yougame_ : slippi_) == Run::Live; }
  bool committed(Side* side = nullptr) const { if (committed_ && side) *side = winner_; return committed_; }
  // A Slippi stop leaves YouGame searching alone; commits and cancellation no longer search.
  bool yougame_only() const { return !over_for_new_work() && yougame_ == Run::Live && slippi_ == Run::Done; }
  // Nothing committed and neither side can still find an opponent: the search has failed.
  bool over() const { return !committed_ && !stopped_ && yougame_ == Run::Done && slippi_ == Run::Done; }
  // The log lines decided so far, oldest first ("yougame: unranked blend: ...").
  std::vector<std::string> take_log() { return std::exchange(log_, {}); }

 private:
  enum class Run : uint8_t { Off, Live, Done };
  Run& run(Side side) { return side == Side::YouGame ? yougame_ : slippi_; }
  bool over_for_new_work() const { return committed_ || stopped_; }
  void start_slippi(uint64_t now) {
    slippi_ = Run::Live;
    start_pending_ = true;
    say("Slippi search started after " + std::to_string(now - start_) + " ms");
  }
  void say(const std::string& line) { log_.push_back("yougame: unranked blend: " + line); }

  uint64_t start_;
  Run yougame_ = Run::Live, slippi_ = Run::Off;
  bool heard_ = false, committed_ = false, stopped_ = false, start_pending_ = false;
  Side winner_ = Side::YouGame;
  uint64_t solo_until_ = 0;
  std::vector<std::string> log_;
};

}  // namespace yougame::blend
