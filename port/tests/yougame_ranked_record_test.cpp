// Fixed rank/result wire and authoritative room/cache transitions, without sockets.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include "yougame_ranked.h"
#include "native_slippi_bridge.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using namespace yougame::ranked;
static int checks;
static json standing(const char* tier = "gold", int division = 4, bool placed = true, int matches = 5) {
  return {{"tier", tier}, {"division", division}, {"placed", placed}, {"matches", matches},
          {"order", 9}, {"rating", 1234.5}, {"placementTotal", 5}, {"label", "Gold IV"}};
}
int main() {
  RankCache local, remote;
  CHECK(local.standing(standing(), json()));
  auto challenger = standing("challenger", 0); challenger["order"] = 21; challenger["label"] = "Challenger";
  CHECK(remote.standing(challenger, -12.3));
  auto before = standing(); before["label"] = "Gold II"; before["division"] = 2; before["order"] = 11;
  before["opaqueFutureField"] = "kept";
  json event = {{"t", "result"}, {"queue", "ranked"}, {"matchId", "set"}, {"ratingDisposition", {{"rated", true}}},
    {"ratings", {{"me", {{"before", 1240.6}, {"after", 1234.5}, {"matches", 6}, {"rankBefore", before}, {"rank", standing()}}}}}};
  ResultsPanel panel;
  CHECK(panel.record().status == MU_YG_RESULT_NONE);
  panel.pending("set"); CHECK(panel.record().status == MU_YG_RESULT_RECORDING);
  auto unrelated = event; unrelated["matchId"] = "old";
  CHECK(!panel.receive(unrelated, "me") && panel.record().status == MU_YG_RESULT_RECORDING);
  CHECK(panel.receive(event, "me") && panel.record().status == MU_YG_RESULT_RECORDED);
  CHECK(panel.record().before == 12406 && panel.record().after == 12345);
  CHECK(panel.record().rank_before.order == 11 && panel.record().rank_after.order == 9);
  CHECK(local.result(event, "me") && local.rank_before() == before);
  CHECK(local.rank_object()["division"] == 4);
  CHECK(local.response().size() == 16 && local.response()[1] == 1);
  CHECK(int8_t(local.response()[15]) == -1); // E3 signed rank delta, not an enum.
  for (int seat = 0; seat < 2; ++seat) {
    const json roster = json::array({{{"slot", seat}, {"rank", standing()}},
                                   {{"slot", 1-seat}, {"rating", -12.3}, {"rank", challenger}}});
    auto self = roster_rank(roster, seat), opp = roster_rank(roster, 1-seat);
    const auto bytes = rank_record_response(self.record(), opp.record(), panel.record());
    CHECK(bytes.size() == 159 && bytes[0] == 1);
    CHECK(bytes[1] == 1 && bytes[2] == 1 && bytes[3] == 3 && bytes[4] == 4);
    CHECK(bytes[5] == 0 && bytes[6] == 9 && bytes[7] == 0 && bytes[8] == 0 && bytes[9] == 0x30 && bytes[10] == 0x39);
    CHECK(bytes[11] == 0 && bytes[12] == 5 && bytes[13] == 5);
    CHECK(std::string((const char*)bytes.data()+14) == "Gold IV" && bytes[37] == 0);
    CHECK(bytes[40] == 6 && bytes[41] == 0 && bytes[43] == 21);
    CHECK(bytes[44] == 255 && bytes[45] == 255 && bytes[46] == 255 && bytes[47] == 133); // -123
    CHECK(bytes[75] == 2 && bytes[76] == 0 && bytes[79] == 0x30 && bytes[80] == 0x76);
    MuYgRanks decoded{}; CHECK(mu_yg_ranks_decode(&decoded, bytes.data(), bytes.size()));
    CHECK(decoded.self.order == 9 && decoded.opponent.tier == 6 && decoded.opponent.rating == -123);
    CHECK(decoded.result.rank_before.division == 2 && decoded.result.rank_after.division == 4);
    CHECK(!mu_yg_ranks_decode(&decoded, bytes.data(), bytes.size()-1) && !decoded.self.known);
    auto bad = bytes; bad[0] = 2; CHECK(!mu_yg_ranks_decode(&decoded, bad.data(), bad.size()));
  }
  // Real producers of every status, then their fixed offsets on the wire.
  for (int status = 0; status <= 4; ++status) {
    ResultsPanel state;
    if (status) state.pending("set");
    if (status == 2) CHECK(state.receive(event, "me"));
    if (status == 3) {
      auto unrated = event; unrated["ratingDisposition"] = {{"rated", false}, {"reason", "pack"}};
      CHECK(state.receive(unrated, "me") && state.record().reason == MU_YG_REASON_PACK);
      CHECK(state.record().before == 0 && !state.record().rank_after.known);
    }
    if (status == 4) { state.result_error(); CHECK(state.rating_text() == "Result unavailable"); }
    const auto bytes = rank_record_response(local.record(), remote.record(), state.record());
    MuYgRanks decoded{}; CHECK(mu_yg_ranks_decode(&decoded, bytes.data(), bytes.size()));
    CHECK(bytes[75] == status && decoded.result.status == status);
  }
  for (const auto& reason : {"void", "other"}) {
    ResultsPanel state; state.pending("set"); auto e = event;
    e["ratingDisposition"] = {{"rated", false}, {"reason", reason}};
    CHECK(state.receive(e, "me"));
    CHECK(state.record().reason == (std::string(reason) == "void" ? MU_YG_REASON_VOID : MU_YG_REASON_OTHER));
  }
  ResultsPanel missing; missing.pending("set"); missing.recovered_standing("set", RankCache(), true);
  CHECK(missing.record().status == MU_YG_RESULT_ERROR && missing.record().reason == MU_YG_REASON_UNAVAILABLE);
  CHECK(missing.receive(event, "me") && missing.record().status == MU_YG_RESULT_RECORDED);
  auto placements = standing("unranked", 0, false, 0); placements.erase("label");
  CHECK(local.standing(placements, 1000)); CHECK(local.record().known && !local.record().placed && local.record().matches == 0);
  CHECK(std::string(local.record().label) == "Placements 0/5");
  placements["matches"] = 100000; CHECK(local.standing(placements, 1000) && local.record().matches == 65535);
  local.clear(); const auto unknown = rank_record_response(local.record(), {}, {});
  CHECK(std::all_of(unknown.begin()+1, unknown.end(), [](uint8_t v){ return v == 0; }));
  for (int32_t value : {INT32_MIN, -1, 0, 1, INT32_MAX}) {
    uint8_t bytes[4]; mu_yg_put32(bytes, value); CHECK(mu_yg_get32(bytes) == value);
  }
  using namespace slippi::online;
  CHECK(CMD_YOUGAME_DATA == 0xFE && MU_YG_SUB_RANK == 0 && MU_YG_SUB_POLICY == 1);
  for (uint8_t sub : {MU_YG_SUB_RANK, MU_YG_SUB_POLICY}) {
    CHECK(command_payload_valid(CMD_YOUGAME_DATA, &sub, 1));
    CHECK(!command_payload_valid(CMD_YOUGAME_DATA, nullptr, 0));
    CHECK(!command_payload_valid(CMD_YOUGAME_DATA, &sub, 2));
    CHECK(!command_payload_valid(CMD_YOUGAME_DATA, nullptr, 1));
  }
  std::cout << "rank record: " << checks << " checks passed\n";
}
