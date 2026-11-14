// squadLineup (agent-test hook, Squad Strike builds): the key's validation and what the host
// hands the game for MU_SQUAD_OP_AGENT_LINEUP. Built by squad_online_wiring_test.py.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "yougame_agent_test.h"
#include <cstdio>
#include <cstdlib>

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

using nlohmann::json;
using yougame::agent_test::Engine;
using yougame::agent_test::map_keys;

static bool ignored(const yougame::agent_test::Settings& s) {
  return !s.squad_lineup_set && s.log.find("agent-test: squadLineup invalid value; ignored") != std::string::npos;
}

int main() {
  using Lineup = std::array<uint8_t, yougame::agent_test::kSquadLineupBytes>;
  static_assert(yougame::agent_test::kSquadLineupBytes == 8, "four kind/colour pairs");
  const auto good = map_keys(json{{"squadLineup", json::array({json::array({2, 0}), json::array({9, 1}),
                                                              json::array({20, 3}), json::array({14, 2})})}},
                             Engine::Source, {}, false);
  CHECK(good.squad_lineup_set);
  CHECK((good.squad_lineup == Lineup{2, 0, 9, 1, 20, 3, 14, 2}));
  CHECK(good.log.find("applied default keys: squadLineup") != std::string::npos);
  CHECK(good.arguments.empty() && good.environment.empty());
  // The last colour each fighter has (Falcon 6, Kirby 6, Yoshi 6, Ganondorf 5).
  CHECK(map_keys(json{{"squadLineup", json::parse("[[0,5],[4,5],[17,5],[25,4]]")}}, Engine::Source, {}, false).squad_lineup_set);
  // Every shape mu_squad_side_valid or the CSS would refuse is ignored, with one log line: three fighters
  // (the version 2 lineup) and five included.
  for (const char* bad : {"[[2,0],[9,1],[20,3]]", "[[2,0],[9,1]]", "[[2,0],[9,1],[20,3],[14,2],[1,0]]", "[]", "true",
                          "\"2:0,9:1,20:3,14:2\"",
                          "[[2,0],[9,1],[20,3],[14]]", "[[2,0],[9,1],[20,3],[14,2,0]]", "[[2,0],[9,1],[20,3],[26,0]]",
                          "[[2,0],[9,1],[20,3],[-1,0]]", "[[2,4],[9,1],[20,3],[14,2]]", "[[2,0],[9,1],[20,4],[14,2]]",
                          "[[2,0],[2,1],[20,3],[14,2]]", "[[2,0],[9,1],[20,3],[2,1]]", "[[2,0],[9,1],[20,6],[14,2]]",
                          "[[2.5,0],[9,1],[20,3],[14,2]]", "[[\"2\",0],[9,1],[20,3],[14,2]]",
                          "[[2,0],[9,1],[20,3],{\"kind\":14}]", "[[2,0],[19,0],[20,3],[14,2]]"}) {
    const auto s = map_keys(json{{"squadLineup", json::parse(bad)}}, Engine::Source, {}, false);
    if (!ignored(s)) std::fprintf(stderr, "accepted %s\n", bad);
    CHECK(ignored(s));
    CHECK((s.squad_lineup == Lineup{}));
  }
  const auto stat = map_keys(json{{"squadLineup", json::parse("[[2,0],[9,1],[20,3],[14,2]]")}}, Engine::Static, {}, false);
  CHECK(!stat.squad_lineup_set);
  CHECK(stat.log.find("squadLineup not supported on the Static Recomp; ignored") != std::string::npos);

  // The packed atomic the host's command handler answers from: nothing until a lineup is stored.
  uint8_t bytes[8] = {};
  yougame::agent_test::squad_lineup.store(0);
  CHECK(!yougame::agent_test::squad_lineup_bytes(bytes));
  yougame::agent_test::squad_lineup.store(yougame::agent_test::squad_lineup_pack(good.squad_lineup));
  CHECK(yougame::agent_test::squad_lineup_bytes(bytes));
  CHECK(bytes[0] == 2 && bytes[1] == 0 && bytes[2] == 9 && bytes[3] == 1 && bytes[4] == 20 && bytes[5] == 3 &&
        bytes[6] == 14 && bytes[7] == 2);
  // Every kind and colour round-trips through the packed byte, in every slot.
  for (uint8_t kind = 0; kind < 26; ++kind) for (uint8_t color = 0; color < 6; ++color) {
    Lineup lineup{0, 0, 1, 0, 2, 0, 3, 0};
    const size_t slot = (kind + color) % 4;
    lineup[2 * slot] = kind; lineup[2 * slot + 1] = color;
    yougame::agent_test::squad_lineup.store(yougame::agent_test::squad_lineup_pack(lineup));
    CHECK(yougame::agent_test::squad_lineup_bytes(bytes));
    for (size_t i = 0; i < 8; ++i) CHECK(bytes[i] == lineup[i]);
  }
  yougame::agent_test::squad_lineup.store(yougame::agent_test::squad_lineup_pack(Lineup{}));
  CHECK(yougame::agent_test::squad_lineup_bytes(bytes) && bytes[0] == 0 && bytes[7] == 0); // kind 0 is a lineup
  // load() of an inactive bridge clears it, so a later session never inherits a lineup.
  yougame::agent_test::load(false, nullptr);
  CHECK(!yougame::agent_test::squad_lineup_bytes(bytes));
  // squadRefuse: a boolean on the Source engine only; anything else is ignored with one log line.
  const auto refuse = map_keys(json{{"squadRefuse", true}}, Engine::Source, {}, false);
  CHECK(refuse.squad_refuse && refuse.log.find("applied default keys: squadRefuse") != std::string::npos);
  CHECK(!map_keys(json{{"squadRefuse", false}}, Engine::Source, {}, false).squad_refuse);
  for (const char* bad : {"1", "\"true\"", "[]", "null"}) {
    const auto s = map_keys(json{{"squadRefuse", json::parse(bad)}}, Engine::Source, {}, false);
    CHECK(!s.squad_refuse && s.log.find("agent-test: squadRefuse needs true or false; ignored") != std::string::npos);
  }
  const auto refuse_static = map_keys(json{{"squadRefuse", true}}, Engine::Static, {}, false);
  CHECK(!refuse_static.squad_refuse && refuse_static.log.find("squadRefuse not supported on the Static Recomp") != std::string::npos);
  CHECK(!map_keys(json::object(), Engine::Source, {}, false).squad_refuse);
  // load() of an inactive bridge clears the flag the setup check reads, like the lineup.
  yougame::net_test::squad_refuse.store(true);
  yougame::agent_test::load(false, nullptr);
  CHECK(!yougame::net_test::squad_refuse.load());
  std::printf("squad agent test: %d checks passed\n", checks);
  return 0;
}
