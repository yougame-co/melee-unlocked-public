// Actual game control/rank readers against the host's production interceptor.
// GPL-3.0-or-later.
#include "yougame_mode_policy.h"
#include "yougame_ranked_cpu.h"
#include "yougame_ranked_rules.h"
#include "yougame_compatibility.h"
#include "native_slippi_bridge.h"
#include <cstdio>
#include <cstdlib>
extern "C" {
#include <mu_ranked.h>
#include <mu_ranked_setup_model.h>
#include <cstring>
}
#define MU_RANKED_REVEAL_TEST
#include "../../sourceport/game/shim/mu_ranked_reveal.c"
static unsigned checks, backend_calls;
static bool active = true;
static unsigned interceptor_calls, read_calls;
static bool reads_available = true;
static int allowed_reply_case = -1;
namespace slippi::online {
// A real-handler seam, deliberately unlike the CPU fixture. Poison the room
// result/opponent to prove practice keeps only the player's real standing.
bool handle(uint8_t cmd, const uint8_t* p, uint32_t n, std::vector<uint8_t>& out) {
  if (cmd != 0xB9 && cmd != 0xE3 &&
      !(cmd == MU_YG_COMMAND && n == 1 && p[0] == MU_YG_SUB_RANK)) return false;
  ++read_calls;
  if (!reads_available) return false;
  if (cmd == 0xB9) {
    out.assign(42, 0); out[0]=1;
    std::memcpy(out.data()+1, "Player name", 11); return true;
  }
  if (cmd == 0xE3) { out.assign(16,0); out[1]=1; return true; }
  if (cmd != MU_YG_COMMAND || n != 1 || p[0] != MU_YG_SUB_RANK) std::abort();
  MuYgRanks ranks{};
  ranks.self.known=ranks.self.placed=1; ranks.self.tier=4; ranks.self.division=4;
  ranks.self.rating=16234; std::strcpy(ranks.self.label,"Platinum IV");
  ranks.opponent=ranks.self; ranks.result.status=MU_YG_RESULT_RECORDED;
  ranks.result.before=15000; ranks.result.after=15250;
  ranks.result.rank_before=ranks.result.rank_after=ranks.self;
  out.resize(MU_YG_RANK_RESPONSE_SIZE); mu_yg_ranks_encode(out.data(), &ranks); return true;
}
}
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
static int bridge(unsigned cmd, const void* data, unsigned size, uint8_t* reply, unsigned capacity, unsigned* got) {
  return slippi::online::native_command(cmd, static_cast<const uint8_t*>(data), size,
    reply, capacity, got, 4096, true,
    [](uint8_t cmd, const uint8_t* data, uint32_t size, std::vector<uint8_t>& out) {
  ++interceptor_calls;
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
  if (yougame::ranked_cpu::intercept(cmd, data, size, out, active)) return true;
#else
  (void)cmd; (void)data; (void)size; (void)out;
#endif
  ++backend_calls;
  return slippi::online::handle(cmd,data,size,out);
    });
}
extern "C" int mu_slippi_cmd(unsigned cmd, const void* data, unsigned size, void* reply, unsigned capacity, unsigned* got) {
  // Exercise old/missing/malformed host replies at the actual game reader.
  if (cmd == MU_YG_COMMAND && size == 1 &&
      *static_cast<const uint8_t*>(data) == MU_YG_SUB_PRACTICE_ALLOWED && allowed_reply_case >= 0) {
    const uint8_t values[] = {0, 1, 2, 255, 1, 1, 1};
    if (reply && capacity) *static_cast<uint8_t*>(reply) = values[allowed_reply_case];
    if (got) *got = allowed_reply_case == 4 ? 0 : allowed_reply_case == 5 ? 2 : 1;
    return allowed_reply_case == 6 ? slippi::online::NATIVE_COMMAND_UNKNOWN : 0;
  }
  // The native ABI always uses its full response buffer, then the game wrapper
  // copies the bounded response to the caller's smaller output.
  uint8_t response[4096]; unsigned received=0;
  const int result=bridge(cmd,data,size,response,sizeof response,&received);
  if (got) *got=received;
  if (reply && received<=capacity) std::memcpy(reply,response,received);
  return result;
}
int main() {
  using namespace yougame;
  const auto rules = ranked_rules::vanilla();
  uint8_t response[4096]{}; unsigned received=99;
  const uint8_t practice[]={MU_YG_SUB_PRACTICE,MU_YG_PRACTICE_START};
  // Malformed FE commands must fail validation before the interceptor.
  const unsigned before=interceptor_calls;
  for(uint8_t sub : {MU_YG_SUB_RANK,MU_YG_SUB_POLICY,MU_YG_SUB_PRACTICE_STATUS,MU_YG_SUB_PRACTICE_ALLOWED,99u}) {
    const uint8_t request[]={sub,1};
    CHECK(bridge(MU_YG_COMMAND,request,2,response,sizeof response,&received)==slippi::online::NATIVE_COMMAND_BOUNDS);
    CHECK(received==0);
  }
  CHECK(bridge(MU_YG_COMMAND,nullptr,2,response,sizeof response,&received)==slippi::online::NATIVE_COMMAND_BOUNDS);
  CHECK(bridge(MU_YG_COMMAND,practice,0,response,sizeof response,&received)==slippi::online::NATIVE_COMMAND_BOUNDS);
  CHECK(bridge(MU_YG_COMMAND,practice,3,response,sizeof response,&received)==slippi::online::NATIVE_COMMAND_BOUNDS);
  CHECK(interceptor_calls==before);
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
  CHECK(mode_policy::yougame_ranked_offered);
  const auto digest = ranked_rules::branch_digest(rules.digest);
  CHECK(digest != rules.digest && digest.size() == 64);
  CHECK(compatibility_key("0.9.81", false, "", false, digest) != compatibility_key("0.9.81", false, "", false, rules.digest));
  CHECK(ranked_rules::peer_verdict(digest, true, rules.digest, false) == -1);
  CHECK(ranked_rules::peer_verdict(digest, true, digest, false) == 1);
  CHECK(!mu_ranked_practice_allowed());
  CHECK(!mu_ranked_practice());
  mu_ranked_set_reset(); CHECK(mu_ranked_set()->max_games == 3);
  // An ordinary Ranked search declines interception, both before/after practice.
  std::vector<uint8_t> out;
  CHECK(!ranked_cpu::intercept(0xB4, nullptr, 0, out, true));
  CHECK(!ranked_cpu::practice_allowed);
  const auto initial_info = ranked_cpu::info;
  const uint8_t allowed = MU_YG_SUB_PRACTICE_ALLOWED;
  CHECK(bridge(MU_YG_COMMAND, &allowed, 1, response, sizeof response, &received) == 0);
  CHECK(received == 1 && response[0] == 0);
  CHECK(bridge(MU_YG_COMMAND, practice, 2, response, sizeof response, &received) == 0);
  CHECK(received == 1 && response[0] == 0);
  CHECK(!ranked_cpu::enabled && !ranked_cpu::practice && !ranked_cpu::initialized);
  CHECK(ranked_cpu::connected && !ranked_cpu::ready && !ranked_cpu::practice_result.status);
  CHECK(ranked_cpu::info == initial_info && !mu_ranked_practice());
  for (allowed_reply_case = 0; allowed_reply_case < 7; ++allowed_reply_case)
    CHECK(mu_ranked_practice_allowed() == (allowed_reply_case == 1));
  allowed_reply_case = -1;
  ranked_cpu::practice_allowed = true;
  CHECK(bridge(MU_YG_COMMAND, &allowed, 1, response, sizeof response, &received) == 0);
  CHECK(received == 1 && response[0] == 1 && mu_ranked_practice_allowed());
  active = false;
  CHECK(!mu_ranked_practice_allowed()); // missing/inactive host fails closed
  CHECK(!mu_ranked_practice_control(MU_YG_PRACTICE_START));
  CHECK(!ranked_cpu::enabled);
  active = true;
  for (unsigned outcome : {MU_YG_PRACTICE_WIN, MU_YG_PRACTICE_LOSS}) {
    CHECK(mu_ranked_practice_control(MU_YG_PRACTICE_START));
    const auto before_calls = backend_calls;
    CHECK(mu_ranked_practice());
    // Practice plays Ranked's best of three (10-31), so the same winner takes it in two games.
    mu_ranked_set_reset(); CHECK(mu_ranked_set()->max_games == 3);
    const unsigned winner = outcome == MU_YG_PRACTICE_WIN ? 0 : 1;
    CHECK(mu_ranked_setup_record_game(mu_ranked_set_mut(), winner, 2, 31, 0) == MU_SETUP_NEXT_GAME);
    CHECK(mu_ranked_setup_record_game(mu_ranked_set_mut(), winner, 2, 32, 0) == MU_SETUP_SET_OVER);
    MuRankedPolicy policy{}; CHECK(mu_ranked_read_policy(&policy));
    CHECK(policy.available && policy.starters[0] == rules.policy.starters[0]);
    MuYgRanks ranks{}; CHECK(mu_ranked_read_ranks(&ranks));
    CHECK(std::string(ranks.self.label) == "Platinum IV" && ranks.self.rating == 16234);
    CHECK(!ranks.opponent.known && !ranks.opponent.rating);
    CHECK(bridge(0xB9,nullptr,0,response,sizeof response,&received)==0);
    CHECK(std::string((char*)response+1)=="Player name");
    CHECK(bridge(0xB3,nullptr,0,response,sizeof response,&received)==0);
    CHECK(std::string((char*)response+15)=="Player name");
    CHECK(std::string((char*)response+46)=="Player name");
    CHECK(std::string((char*)response+77)=="CPU");
    CHECK(std::string((char*)response+170)=="CPU");
    CHECK(ranks.result.status == MU_YG_RESULT_NONE);
    // Match/report commands stay local; identity and standing status can pass through.
    for (unsigned cmd = 0xB0; cmd <= 0xC4; ++cmd) {
      const uint8_t payload[14]{};
      CHECK(ranked_cpu::intercept(cmd, payload, 14, out, true) == (cmd != 0xB9));
    }
    for (unsigned cmd = 0xE0; cmd <= 0xE9; ++cmd) CHECK(ranked_cpu::intercept(cmd, nullptr, 0, out, true) == (cmd != 0xE3));
    const uint8_t unknown = 99;
    CHECK(ranked_cpu::intercept(MU_YG_COMMAND, &unknown, 1, out, true));
    CHECK(ranked_cpu::intercept(0xFA, nullptr, 0, out, true));
    CHECK(out[0] == 5 && std::string((char*)out.data()+1) == "Practice - not rated");
    CHECK(mu_ranked_practice_control(outcome));
    CHECK(mu_ranked_read_ranks(&ranks));
    MuReveal reveal{}; reveal_accept(&reveal, &ranks.result);
    CHECK(reveal.active && reveal.result.status == MU_YG_RESULT_UNRATED);
    CHECK(reveal_direction(&reveal.result) == 0);
    CHECK(!reveal.result.before && !reveal.result.after);
    CHECK(!reveal.result.rank_before.known && !reveal.result.rank_after.known);
    CHECK(std::string(ranks.self.label) == "Platinum IV" && ranks.self.rating == 16234);
    reveal.frame = REVEAL_GLOW_END;
    CHECK(!reveal_pose(&reveal).rating && !reveal_pose(&reveal).changed);
    CHECK(!mu_ranked_practice_control(outcome)); // cannot settle twice
    CHECK(backend_calls == before_calls + 1); // only B9 was forwarded by the interceptor
    // Disconnect retains the practice result until exit/search; real rank stays readable.
    CHECK(ranked_cpu::intercept(0xBA, nullptr, 0, out, true));
    CHECK(mu_ranked_practice() && mu_ranked_read_ranks(&ranks));
    CHECK(mu_ranked_practice_control(MU_YG_PRACTICE_STOP));
    CHECK(!mu_ranked_practice() && !ranked_cpu::enabled);
    mu_ranked_set_reset(); CHECK(mu_ranked_set()->max_games == 3);
    CHECK(!ranked_cpu::intercept(0xB4, nullptr, 0, out, true));
  }
  CHECK(mu_ranked_practice_control(MU_YG_PRACTICE_START));
  MuYgRanks ranks{}; CHECK(mu_ranked_read_ranks(&ranks));
  CHECK(ranks.result.status == MU_YG_RESULT_NONE && ranks.self.rating == 16234);
  reads_available=false;
  CHECK(mu_ranked_read_ranks(&ranks));
  CHECK(!ranks.self.known && !ranks.self.rating && !ranks.opponent.known);
  CHECK(ranked_cpu::intercept(0xB3,nullptr,0,out,true));
  CHECK(!out[15] && !out[46] && std::string((char*)out.data()+77)=="CPU");
  CHECK(read_calls>0);
  reads_available=true;
  CHECK(mu_ranked_practice_control(MU_YG_PRACTICE_STOP));
  ranked_cpu::practice_allowed = false;
  CHECK(!mu_ranked_practice_allowed());
#else
  CHECK(!mode_policy::yougame_ranked_offered);
  CHECK(ranked_rules::branch_digest(rules.digest) == rules.digest);
  CHECK(!mu_ranked_practice_allowed());
  CHECK(!mu_ranked_practice());
  mu_ranked_set_reset(); CHECK(mu_ranked_set()->max_games == 3);
  for (unsigned action = 0; action <= MU_YG_PRACTICE_LOSS; ++action) CHECK(!mu_ranked_practice_control(action));
  CHECK(backend_calls == 0); // OFF controls don't even call the bridge
  CHECK(bridge(MU_YG_COMMAND,practice,2,response,sizeof response,&received)==slippi::online::NATIVE_COMMAND_BOUNDS);
  CHECK(received==0 && interceptor_calls==before && !mu_ranked_practice());
#endif
  std::printf("Ranked branch controls: %u checks passed\n", checks);
}
