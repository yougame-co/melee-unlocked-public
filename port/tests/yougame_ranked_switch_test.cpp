// GameSetup must not replace first-to-two or release the pair before C2 + result.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "yougame_ranked.h"
#include <cstdlib>
#include <iostream>
using namespace yougame::ranked;
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
static Set started(bool setup) {
  Set set; set.configure(0, {"a", "b"});
  if (setup) set.game_setup(); // first C0 is before the first gameplay frame
  set.start_game("game-1"); set.receive({{"t", "start"}, {"matchId", "set"}}); return set;
}
static void game(Set& set, const char* id, int winner) {
  set.start_game(id); Game result; result.winner = winner; set.report_game(result);
  CHECK(!set.decided());
  set.receive({{"t", "game-result"}, {"matchId", "set"}, {"id", id},
               {"ranking", winner == 0 ? json{"a", "b"} : json{"b", "a"}}});
}
int main() {
  for (int seat = 0; seat < 2; ++seat) for (bool c2_first : {false,true}) {
    auto set = started(true); set.configure(seat, {"a", "b"});
    game(set, "game-1", seat); CHECK(!set.decided() && !set.release_pair());
    game(set, "game-2", seat);
    CHECK(set.decided() && set.final_message()["winner"] == (seat ? "b" : "a"));
    CHECK(set.over() && !set.release_pair());
    if (c2_first) set.complete(0);
    else set.receive({{"t", "result"}, {"matchId", "set"}});
    CHECK(!set.release_pair());
    if (c2_first) set.receive({{"t", "result"}, {"matchId", "set"}});
    else set.complete(0);
    CHECK(set.release_pair());
    const auto final = set.final_message(); set.complete(1);
    CHECK(set.final_message() == final); // C2 never adds/changes the room-authoritative final
  }
  // A refused finish cannot hold GameSetup forever, even with healthy sockets.
  for (bool c2 : {false, true}) {
    auto held = started(true);
    game(held, "game-1", 0); game(held, "game-2", 0);
    held.due(100, true, 1); // arm the bound when the decided set is first pumped
    const auto winner = held.final_message();
    if (c2) held.complete(0);
    held.refused({{"action", "finish"}}, 101);
    CHECK(held.due(100 + kPairCleanupMs - 1, true, 1).empty());
    CHECK(!held.release_pair());
    CHECK(held.due(100 + kPairCleanupMs, true, 1).empty());
    CHECK(held.release_pair() && held.needs_drain() && !held.settled());
    CHECK(held.final_message() == winner && winner["winner"] == "a");
  }
  // A settled set missing C2 has the same bound. Run timers before settled's early return.
  auto missing_c2 = started(true);
  game(missing_c2, "game-1", 1); game(missing_c2, "game-2", 1);
  missing_c2.receive({{"t", "result"}, {"matchId", "set"}});
  const auto winner = missing_c2.final_message();
  missing_c2.due(200, true, 1);
  missing_c2.due(200 + kPairCleanupMs - 1, true, 1); CHECK(!missing_c2.release_pair());
  missing_c2.due(200 + kPairCleanupMs, true, 1);
  CHECK(missing_c2.release_pair() && !missing_c2.needs_drain());
  CHECK(missing_c2.final_message() == winner && winner["winner"] == "b");
  auto early = started(true); early.complete(0);
  game(early, "game-1", 0); CHECK(!early.decided());
  game(early, "game-2", 0); CHECK(early.decided() && !early.release_pair());
  auto legacy = started(false); game(legacy, "game-1", 0); game(legacy, "game-2", 0);
  CHECK(legacy.release_pair());
  auto abnormal = started(true); game(abnormal, "game-1", 0); abnormal.complete(1);
  CHECK(abnormal.final_message()["void"] == true && !abnormal.release_pair());
  abnormal.receive({{"t", "result"}, {"matchId", "set"}}); CHECK(abnormal.release_pair());
  auto dead = started(true); dead.room_closed(); CHECK(dead.release_pair());
  auto quit = started(true); quit.quit(); CHECK(quit.leaves_at_once() && quit.release_pair());
  auto lost = started(true); lost.lost(); CHECK(lost.release_pair());
  std::cout << "ranked host switches: " << checks << " checks passed\n";
}
