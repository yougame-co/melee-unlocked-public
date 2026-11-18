// Warm-up room (slippi_warmup.h): going to Training and back, staying away in any other mode, the
// opponent's states shown while away, the opponent leaving, both players warming up, the arena's turn
// call with its countdown, and the time limits that keep a lost scene change from leaving the room stuck.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_warmup.h"
#include <cstdio>
#include <functional>
#include <vector>

using namespace slippi::warmup;

namespace {
int g_failed = 0, g_checks = 0;
const char* g_test = "";
#define CHECK(x) do { ++g_checks; if (!(x)) { std::printf("  FAIL %s line %d: %s\n", g_test, __LINE__, #x); ++g_failed; } } while (0)

struct Case { const char* name; std::function<void()> body; };
std::vector<Case>& cases() { static std::vector<Case> c; return c; }
struct Add { Add(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Add add_##name(#name, name); static void name()

// A frame on the online character select, one in Training, one between them (a scene change).
Observe select_frame(uint64_t t, PeerState p = PeerState::Picking) {
  Observe o; o.now_ms = t; o.online_select = true; o.peers = 1; o.peer[0] = p; return o;
}
Observe training_frame(uint64_t t, PeerState p = PeerState::Picking) {
  Observe o; o.now_ms = t; o.in_training = true; o.peers = 1; o.peer[0] = p; return o;
}
Observe between_frame(uint64_t t, PeerState p = PeerState::Picking) {
  Observe o; o.now_ms = t; o.peers = 1; o.peer[0] = p; return o;
}
// Runs frames 16 ms apart from t0 to t1 with `make`, returning the first action that is not None.
Action run(Room& r, uint64_t t0, uint64_t t1, const std::function<Observe(uint64_t)>& make, uint64_t* at = nullptr) {
  for (uint64_t t = t0; t <= t1; t += 16) {
    const Action a = r.tick(make(t));
    if (a != Action::None) { if (at) *at = t; return a; }
  }
  return Action::None;
}
// Into Training: enter at t, a scene change, then Training from t+300.
void into_training(Room& r, uint64_t t, Kind kind = Kind::Pair) {
  CHECK(r.enter(kind, t));
  CHECK(r.stage() == Stage::Going);
  CHECK(r.tick(select_frame(t + 16)) == Action::None);   // the select still answers for a few frames
  CHECK(r.tick(between_frame(t + 100)) == Action::None);
  CHECK(r.tick(training_frame(t + 300)) == Action::None);
  CHECK(r.stage() == Stage::Away);
}
}  // namespace

TEST(enter_and_return) {
  Room r;
  CHECK(!r.active());
  CHECK(!r.request_return(0));   // nothing to go back from
  into_training(r, 1000);
  CHECK(!r.enter(Kind::Pair, 1400));   // one warm-up at a time
  CHECK(run(r, 1400, 60000, [](uint64_t t) { return training_frame(t); }) == Action::None);
  CHECK(r.request_return(60000));
  CHECK(r.stage() == Stage::Returning);
  CHECK(r.tick(between_frame(60100)) == Action::None);
  CHECK(r.tick(select_frame(60300)) == Action::Arrived);
  CHECK(!r.active());
  CHECK(r.banner() == Banner::None);
}

TEST(return_while_going) {
  // Changing one's mind before Training even came up still goes back.
  Room r;
  CHECK(r.enter(Kind::Pair, 0));
  CHECK(r.request_return(50));
  CHECK(r.tick(training_frame(200)) == Action::None);   // the game got there anyway
  CHECK(r.tick(select_frame(900)) == Action::Arrived);
}

TEST(long_stay_twelve_minutes) {
  Room r;
  into_training(r, 0);
  CHECK(run(r, 400, 12 * 60 * 1000, [](uint64_t t) { return training_frame(t); }) == Action::None);
  CHECK(r.stage() == Stage::Away);
  CHECK(r.request_return(12 * 60 * 1000 + 16));
  CHECK(r.tick(select_frame(12 * 60 * 1000 + 400)) == Action::Arrived);
}

TEST(training_select_counts_as_training) {
  // Training's own character and stage selects are the same major scene: the player may change
  // character there and start again without the room ending.
  Room r;
  into_training(r, 0);
  CHECK(run(r, 400, 30000, [](uint64_t t) { return training_frame(t); }) == Action::None);
  CHECK(r.stage() == Stage::Away);
}

TEST(opponent_ready_goes_back_at_once) {
  // The opponent locks in while the player trains: back at once (the coordinator then presses
  // Start on the select), the same frame, no countdown.
  Room r;
  into_training(r, 0);
  CHECK(r.tick(training_frame(5000, PeerState::Picking)) == Action::None);
  CHECK(r.banner() == Banner::None);
  CHECK(r.tick(training_frame(6000, PeerState::Ready)) == Action::Ready);
  CHECK(r.stage() == Stage::Returning);
  CHECK(r.banner() == Banner::PeerReady);
  CHECK(r.banner_since_ms() == 6000);
  CHECK(r.banner_peer() == 0);
  CHECK(r.peer(0) == PeerState::Ready);
  CHECK(r.tick(training_frame(6016, PeerState::Ready)) == Action::None);   // once
  CHECK(r.tick(select_frame(6400, PeerState::Ready)) == Action::Arrived);
}

TEST(opponent_ready_stays_when_turned_off) {
  Room r;
  r.set_back_when_ready(false);
  into_training(r, 0);
  CHECK(r.tick(training_frame(6000, PeerState::Ready)) == Action::None);
  CHECK(r.banner() == Banner::PeerReady);
  CHECK(r.stage() == Stage::Away);
  CHECK(run(r, 6016, 30000, [](uint64_t t) { return training_frame(t, PeerState::Ready); }) == Action::None);
}

TEST(teams_back_when_the_last_locks_in) {
  Room r;
  CHECK(r.enter(Kind::Pair, 0));
  Observe o = training_frame(300);
  o.peers = 3;
  CHECK(r.tick(o) == Action::None);
  o.now_ms = 2000; o.peer[0] = PeerState::Ready;
  CHECK(r.tick(o) == Action::None);
  o.now_ms = 3000; o.peer[2] = PeerState::Ready;
  CHECK(r.tick(o) == Action::None);
  o.now_ms = 4000; o.peer[1] = PeerState::Ready;
  CHECK(r.tick(o) == Action::Ready);
}

TEST(teams_one_warming_is_not_ready) {
  Room r;
  CHECK(r.enter(Kind::Pair, 0));
  Observe o = training_frame(300);
  o.peers = 3;
  o.peer[0] = o.peer[1] = PeerState::Ready;
  o.peer[2] = PeerState::WarmingUp;
  CHECK(r.tick(o) == Action::None);
  CHECK(run(r, 316, 20000, [&](uint64_t t) { Observe x = o; x.now_ms = t; return x; }) == Action::None);
}

TEST(no_banner_for_state_seen_at_entry) {
  // The opponent was already ready when the player left: that is no news.
  Room r;
  CHECK(r.enter(Kind::Pair, 0));
  CHECK(r.tick(select_frame(16, PeerState::Ready)) == Action::None);
  CHECK(r.tick(training_frame(300, PeerState::Ready)) == Action::None);
  CHECK(r.banner() == Banner::None);
  CHECK(r.peer(0) == PeerState::Ready);
}

TEST(opponent_leaves_during_warmup) {
  Room r;
  into_training(r, 0);
  Observe gone = training_frame(8000);
  gone.link_up = false;
  CHECK(r.tick(gone) == Action::None);   // the player stays in Training; the banner says so
  CHECK(r.banner() == Banner::PeerLeft);
  CHECK(r.peer(0) == PeerState::Left);
  CHECK(r.anyone_left());
  // The link coming back up for a frame does not bring them back.
  CHECK(r.tick(training_frame(8100, PeerState::Ready)) == Action::None);
  CHECK(r.peer(0) == PeerState::Left);
  CHECK(r.banner() == Banner::PeerLeft);
  // Going back still goes to the select (the game shows the normal disconnected screen there).
  CHECK(r.request_return(9000));
  CHECK(r.tick(select_frame(9400)) == Action::Arrived);
}

TEST(teams_one_of_three_leaves) {
  Room r;
  CHECK(r.enter(Kind::Pair, 0));
  Observe o = training_frame(300);
  o.peers = 3;
  CHECK(r.tick(o) == Action::None);
  CHECK(r.peers() == 3);
  o.now_ms = 4000;
  o.peers = 2;   // the third dropped
  CHECK(r.tick(o) == Action::None);
  CHECK(r.peer(0) == PeerState::Picking);
  CHECK(r.peer(1) == PeerState::Picking);
  CHECK(r.peer(2) == PeerState::Left);
  CHECK(r.banner() == Banner::PeerLeft);
  CHECK(r.banner_peer() == 2);
}

TEST(both_warm_up) {
  // The opponent goes to warm up too, and later comes back to the select.
  Room r;
  into_training(r, 0);
  CHECK(r.tick(training_frame(3000, PeerState::WarmingUp)) == Action::None);
  CHECK(r.banner() == Banner::PeerWarming);
  CHECK(r.peer(0) == PeerState::WarmingUp);
  CHECK(r.tick(training_frame(20000, PeerState::Picking)) == Action::None);
  CHECK(r.banner() == Banner::PeerBack);
  // Whoever goes back first waits on the select as usual.
  CHECK(r.request_return(21000));
  CHECK(r.tick(select_frame(21400)) == Action::Arrived);
}

TEST(both_warm_up_mirror) {
  // The other side of the same pair: it sees this player warming up, then ready.
  Room r;
  into_training(r, 0);
  CHECK(r.tick(training_frame(2000, PeerState::WarmingUp)) == Action::None);
  CHECK(r.tick(training_frame(2500, PeerState::Ready)) == Action::Ready);   // came back and locked in
  CHECK(r.banner() == Banner::PeerReady);
}

TEST(going_never_arrives) {
  Room r;
  CHECK(r.enter(Kind::Pair, 0));
  uint64_t at = 0;
  CHECK(run(r, 16, 60000, [](uint64_t t) { return between_frame(t); }, &at) == Action::GiveUp);
  CHECK(at > Room::kGoingTimeoutMs);
  CHECK(!r.active());
}

TEST(return_never_arrives) {
  Room r;
  into_training(r, 0);
  CHECK(r.request_return(1000));
  uint64_t at = 0;
  CHECK(run(r, 1016, 60000, [](uint64_t t) { return between_frame(t); }, &at) == Action::GiveUp);
  CHECK(at > 1000 + Room::kReturnTimeoutMs);
  CHECK(!r.active());
}

TEST(player_goes_to_other_modes) {
  // Out of Training to the menus and any other mode (VS, Classic...): the room keeps the player, for as
  // long as they like, with the opponent's news still coming.
  Room r;
  into_training(r, 0);
  CHECK(run(r, 5000, 5000 + 15 * 60 * 1000, [](uint64_t t) { return between_frame(t); }) == Action::None);
  CHECK(r.active());
  CHECK(r.stage() == Stage::Away);
  CHECK(r.tick(between_frame(1000000, PeerState::WarmingUp)) == Action::None);
  CHECK(r.banner() == Banner::PeerWarming);
  CHECK(r.tick(training_frame(1000100)) == Action::None);   // and back into Training: still the same stay
  CHECK(r.stage() == Stage::Away);
}

TEST(online_menus_lead_back) {
  // The player picks Online from the menus: not a new online game, the way back to the room.
  Room r;
  into_training(r, 0, Kind::Arena);
  CHECK(r.tick(between_frame(5000)) == Action::None);
  Observe o; o.now_ms = 6000; o.online_menu = true;
  CHECK(r.tick(o) == Action::GoBack);
  CHECK(r.stage() == Stage::Returning);
  Observe s; s.now_ms = 6500; s.online_select = true;
  CHECK(r.tick(s) == Action::Arrived);
  CHECK(!r.active());
}

TEST(opponent_ready_elsewhere_counts_down) {
  // In a match of another mode when the opponent locks in: the countdown, then back to play.
  Room r;
  r.set_countdown_ms(3000);
  into_training(r, 0);
  CHECK(r.tick(between_frame(5000, PeerState::Picking)) == Action::None);
  CHECK(r.tick(between_frame(6000, PeerState::Ready)) == Action::None);
  CHECK(r.banner() == Banner::PeerReady);
  CHECK(r.ready_waiting());
  CHECK(r.countdown_left_ms(6000) == 3000);
  uint64_t at = 0;
  CHECK(run(r, 6016, 20000, [](uint64_t t) { return between_frame(t, PeerState::Ready); }, &at) == Action::Ready);
  CHECK(at >= 9000 && at < 9020);
  CHECK(r.stage() == Stage::Returning);
  CHECK(!r.ready_waiting());
}

TEST(opponent_ready_elsewhere_instant) {
  Room r;
  r.set_countdown_ms(0);
  into_training(r, 0);
  CHECK(r.tick(between_frame(6000, PeerState::Ready)) == Action::Ready);
}

TEST(opponent_unlocks_during_the_countdown) {
  Room r;
  r.set_countdown_ms(3000);
  into_training(r, 0);
  CHECK(r.tick(between_frame(6000, PeerState::Ready)) == Action::None);
  CHECK(r.tick(between_frame(7000, PeerState::Picking)) == Action::None);
  CHECK(!r.ready_waiting());
  CHECK(r.countdown_left_ms(7000) == -1);
  CHECK(run(r, 7016, 20000, [](uint64_t t) { return between_frame(t, PeerState::Picking); }) == Action::None);
}

TEST(press_during_ready_countdown) {
  Room r;
  r.set_countdown_ms(3000);
  into_training(r, 0);
  CHECK(r.tick(between_frame(6000, PeerState::Ready)) == Action::None);
  CHECK(r.request_return(6500));
  CHECK(r.stage() == Stage::Returning);
  CHECK(!r.ready_waiting());
}

TEST(short_scene_gap_is_not_leaving) {
  // Restarting Training passes through a frame or two of no scene: not a departure.
  Room r;
  into_training(r, 0);
  CHECK(r.tick(between_frame(5000)) == Action::None);
  CHECK(r.tick(between_frame(5500)) == Action::None);
  CHECK(r.tick(training_frame(5600)) == Action::None);
  CHECK(r.tick(between_frame(9000)) == Action::None);
  CHECK(r.tick(training_frame(9100)) == Action::None);
  CHECK(r.stage() == Stage::Away);
}

TEST(arrived_by_other_way) {
  Room r;
  into_training(r, 0);
  CHECK(r.tick(select_frame(7000)) == Action::Arrived);
  CHECK(!r.active());
}

TEST(arena_turn_countdown_three_seconds) {
  Room r;
  r.set_countdown_ms(3000);
  into_training(r, 0, Kind::Arena);
  Observe o; o.in_training = true;
  o.now_ms = 10000;
  CHECK(r.tick(o) == Action::None);
  CHECK(r.countdown_left_ms(10000) == -1);
  o.turn = true;
  CHECK(r.tick(o) == Action::None);
  CHECK(r.banner() == Banner::YourTurn);
  CHECK(r.countdown_left_ms(10000) == 3000);
  CHECK(r.countdown_left_ms(11000) == 2000);
  uint64_t at = 0;
  CHECK(run(r, 10016, 20000, [](uint64_t t) { Observe x; x.now_ms = t; x.in_training = true; x.turn = true; return x; }, &at) == Action::GoBack);
  CHECK(at >= 13000 && at < 13020);
  CHECK(r.stage() == Stage::Returning);
  Observe s; s.now_ms = at + 500; s.online_select = true; s.turn = true;
  CHECK(r.tick(s) == Action::Arrived);
}

TEST(arena_turn_instant) {
  Room r;
  r.set_countdown_ms(0);
  into_training(r, 0, Kind::Arena);
  Observe o; o.in_training = true; o.now_ms = 10000; o.turn = true;
  CHECK(r.tick(o) == Action::GoBack);
  CHECK(r.stage() == Stage::Returning);
}

TEST(arena_press_during_countdown) {
  Room r;
  r.set_countdown_ms(3000);
  into_training(r, 0, Kind::Arena);
  Observe o; o.in_training = true; o.now_ms = 10000; o.turn = true;
  CHECK(r.tick(o) == Action::None);
  CHECK(r.request_return(10500));   // the player pressed: at once
  CHECK(r.stage() == Stage::Returning);
  CHECK(r.countdown_left_ms(10600) == -1);
}

TEST(arena_turn_taken_back) {
  // The turn is called, then the room changes its mind before the countdown ends (the player ahead
  // came back): the countdown stops and the player stays.
  Room r;
  r.set_countdown_ms(3000);
  into_training(r, 0, Kind::Arena);
  Observe o; o.in_training = true; o.now_ms = 10000; o.turn = true;
  CHECK(r.tick(o) == Action::None);
  o.now_ms = 11000; o.turn = false;
  CHECK(r.tick(o) == Action::None);
  CHECK(r.countdown_left_ms(11000) == -1);
  CHECK(r.banner() == Banner::None);
  CHECK(run(r, 11016, 30000, [](uint64_t t) { Observe x; x.now_ms = t; x.in_training = true; return x; }) == Action::None);
  CHECK(r.stage() == Stage::Away);
}

TEST(arena_turn_called_on_the_way) {
  // The room called the turn while the game was still on its way into Training.
  Room r;
  r.set_countdown_ms(3000);
  CHECK(r.enter(Kind::Arena, 0));
  Observe o; o.now_ms = 100; o.turn = true;
  CHECK(r.tick(o) == Action::None);
  o.now_ms = 400; o.in_training = true;
  CHECK(r.tick(o) == Action::None);
  CHECK(r.banner() == Banner::YourTurn);
  CHECK(r.countdown_left_ms(400) == 3000);
  // With the instant setting it turns straight round.
  Room q;
  q.set_countdown_ms(0);
  CHECK(q.enter(Kind::Arena, 0));
  CHECK(q.tick(o) == Action::GoBack);
}

TEST(countdown_setting_survives_entries) {
  Room r;
  r.set_countdown_ms(0);
  into_training(r, 0, Kind::Arena);
  CHECK(r.request_return(1000));
  Observe s; s.now_ms = 1400; s.online_select = true;
  CHECK(r.tick(s) == Action::Arrived);
  // The next warm-up keeps the setting.
  into_training(r, 5000, Kind::Arena);
  Observe o; o.in_training = true; o.now_ms = 9000; o.turn = true;
  CHECK(r.tick(o) == Action::GoBack);
}

TEST(arena_ignores_peers) {
  // An arena warm-up has no pair to watch: whatever the link fields say, no banners.
  Room r;
  into_training(r, 0, Kind::Arena);
  Observe o = training_frame(5000, PeerState::Ready);
  o.link_up = false;
  CHECK(r.tick(o) == Action::None);
  CHECK(r.banner() == Banner::None);
}

TEST(names) {
  CHECK(stage_name(Stage::Away)[0] == 'a');
  CHECK(peer_state_name(PeerState::WarmingUp)[0] == 'w');
}

int main() {
  for (const Case& c : cases()) {
    g_test = c.name;
    const int before = g_failed;
    c.body();
    std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", c.name);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
