// Every ladder division, placements and unknown roster data through the real B3
// rank fill and the same FE encoder used by yougame_link.cpp. No sockets.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "yougame_ranked.h"
#include <cstdlib>
#include <iostream>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
namespace yougame {
static bool enabled = true, visible = true;
static int local_seat;
static ranked::json participants;
bool active() { return enabled; }
uint8_t rank_visibility() { return visible; }
uint8_t local_rank() { return ranked::roster_rank_byte(participants, local_seat); }
uint8_t opponent_rank() { return ranked::roster_rank_byte(participants, 1-local_seat); }
}
namespace Matchmaking { enum { RANKED, UNRANKED }; }
static struct { int mode = Matchmaking::RANKED; } g_last_search;
static uint8_t g_local_player_index;
static std::vector<uint8_t> b3() {
  std::vector<uint8_t> q;
#include "ranked_b3_fill.inc"
  return q;
}
using namespace yougame::ranked;
static json standing(const char* tier, int division, bool placed = true, int matches = 5) {
  return {{"tier", tier}, {"division", division}, {"placed", placed}, {"matches", matches},
          {"order", 17}, {"rating", 1500.7}, {"placementTotal", 5}};
}
static void check_opponent(const json& rank, int tier, int division, uint8_t byte, const std::string& label) {
  for (int seat : {0,1}) {
    yougame::local_seat = g_local_player_index = seat;
    // The roster is deliberately out of slot order; rating beside rank wins.
    yougame::participants = json::array({
      {{"slot", 1-seat}, {"rank", rank}, {"rating", 1555.6}},
      {{"slot", seat}, {"rank", standing("silver", 2)}}});
    const auto bytes = b3();
    CHECK(bytes.size() == 2 && bytes[1-seat] == byte && bytes[seat] == 5);
    const auto self = roster_rank(yougame::participants, seat);
    const auto opponent = roster_rank(yougame::participants, 1-seat);
    const auto wire = rank_record_response(self.record(), opponent.record(), {});
    MuYgRanks decoded{}; CHECK(mu_yg_ranks_decode(&decoded, wire.data(), wire.size()));
    CHECK(decoded.opponent.known && decoded.opponent.tier == tier && decoded.opponent.division == division);
    CHECK(bool(decoded.opponent.placed) == rank["placed"].get<bool>() && decoded.opponent.matches == rank["matches"]);
    CHECK(decoded.opponent.order == 17 && decoded.opponent.rating == 15556 && decoded.opponent.placement_total == 5);
    CHECK(std::string(decoded.opponent.label) == label);
    CHECK(decoded.self.tier == 2 && decoded.self.division == 2 && decoded.self.rating == 15007);
    yougame::visible = false;
    CHECK(b3()[seat] == 255 && b3()[1-seat] == byte);
    yougame::visible = true;
  }
}
int main() {
  const char* tiers[] = {"bronze", "silver", "gold", "platinum", "diamond"};
  const char* names[] = {"Bronze", "Silver", "Gold", "Platinum", "Diamond"};
  const char* divisions[] = {"", "I", "II", "III", "IV"};
  for (int t = 0; t < 5; ++t) for (int d = 1; d <= 4; ++d)
    check_opponent(standing(tiers[t], d), t+1, d,
      uint8_t(1+3*t+(d == 1 ? 2 : d == 2 ? 1 : 0)), std::string(names[t])+" "+divisions[d]);
  check_opponent(standing("challenger", 0), 6, 0, 19, "Challenger");
  for (int count : {0,3,4})
    check_opponent(standing("unranked", 0, false, count), 0, 0, 0, "Placements "+std::to_string(count)+"/5");
  for (const auto& roster : {json(), json::object(), json::array(),
       json::array({{{"slot", 0}}}),
       json::array({{{"slot", 0}, {"rank", {{"placed", false}}}}}),
       json::array({{{"slot", "0"}, {"rank", standing("gold", 1)}}})}) {
    yougame::participants = roster;
    for (int seat : {0,1}) {
      yougame::local_seat = g_local_player_index = seat;
      const auto opponent = roster_rank(roster, 1-seat);
      CHECK(!opponent.known() && !opponent.record().known);
      CHECK(b3() == std::vector<uint8_t>({255,255}));
      const auto wire = rank_record_response({}, opponent.record(), {});
      CHECK(std::all_of(wire.begin()+1, wire.end(), [](uint8_t v){ return v == 0; }));
    }
  }
  yougame::participants = json::array({{{"slot", 0}, {"rank", standing("gold", 1)}}});
  for (uint8_t invalid : {2,255}) { g_local_player_index = invalid; CHECK(b3() == std::vector<uint8_t>({255,255})); }
  g_local_player_index = 0;
  g_last_search.mode = Matchmaking::UNRANKED;
  CHECK(b3() == std::vector<uint8_t>({0,0}));
  g_last_search.mode = Matchmaking::RANKED; yougame::enabled = false;
  CHECK(b3() == std::vector<uint8_t>({0,0}));
  std::cout << "opponent rank: " << checks << " checks passed\n";
}
