// Hook gating, fake ladder/wire and a full CPU set using both model seats.
// GPL-3.0-or-later.
#include "yougame_agent_test.h"
#include "yougame_ranked_cpu.h"
#include "mu_ranked_setup_model.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
// The agent fixture must never ask the real identity/rank handler.
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
namespace slippi::online {
bool handle(uint8_t, const uint8_t*, uint32_t, std::vector<uint8_t>&) { std::abort(); }
}
#endif
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
int main() {
  using namespace yougame;
#ifdef YOUGAME_AGENT_TEST_HOOK
  auto settings=agent_test::map_keys({{"online","ranked"},{"rankedCpu",true}},agent_test::Engine::Source);
  CHECK(settings.ranked_cpu); CHECK(settings.arguments==std::vector<std::string>({"--online-test","ranked"}));
  CHECK(!agent_test::map_keys({{"rankedCpu","yes"}},agent_test::Engine::Source).ranked_cpu);
  CHECK(!agent_test::map_keys({{"rankedCpu",true}},agent_test::Engine::Static).ranked_cpu);
#endif
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
  ranked_cpu::enabled=true; std::vector<uint8_t> reply;
  const uint8_t sub_policy=MU_YG_SUB_POLICY, sub_rank=MU_YG_SUB_RANK, sub_unknown=9;
  CHECK(ranked_cpu::handle(MU_YG_COMMAND,&sub_policy,1,reply));
  MuRankedPolicy policy; CHECK(mu_yg_policy_decode(&policy,reply.data(),reply.size()) && policy.available);
  CHECK(!ranked_cpu::handle(MU_YG_COMMAND,&sub_unknown,1,reply));
  CHECK(ranked_cpu::handle(MU_YG_COMMAND,&sub_rank,1,reply));
  MuYgRanks ranks; CHECK(mu_yg_ranks_decode(&ranks,reply.data(),reply.size()));
  CHECK(ranks.self.known && ranks.opponent.known && ranks.opponent.rating==18000);
  CHECK(ranked_cpu::handle(0xFA,nullptr,0,reply));
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
  CHECK(reply[0]==5);
#else
  CHECK(reply[0]==4); // hidden in hook builds
#endif
  CHECK(ranked_cpu::handle(0xB9,nullptr,0,reply));
  CHECK(!std::strcmp((char*)reply.data()+1,"Local test"));
  CHECK(ranked_cpu::handle(0xB3,nullptr,0,reply)); CHECK(reply.size()==962);
  CHECK(!std::strcmp((char*)reply.data()+15,"Local test"));
  CHECK(!std::strcmp((char*)reply.data()+46,"Local test"));
  CHECK(std::string(ranks.self.label)=="Gold II (test)" && ranks.self.rating==15000);
  CHECK(!std::strcmp((char*)reply.data()+910,"agent-ranked-cpu"));
  MuRankedSet sets[2]{};
  for(auto& s:sets) {s.max_games=3;s.cur_game=1;s.prev_winner=255;s.last_stage_win[0]=s.last_stage_win[1]=65535;}
  MuRankedSetupResult previous{}; previous.stage=31; previous.fighters[0]={2,0}; previous.fighters[1]={20,0};
  for(unsigned game=1;game<=3;++game) {
    MuRankedSetupModel model[2];
    for(unsigned seat=0;seat<2;++seat) CHECK(mu_ranked_setup_model_init(&model[seat],&policy,&sets[seat],seat,&previous));
    while(const auto* step=mu_ranked_setup_current(&model[0])) {
      unsigned player=step->player; uint8_t packet[5], response[6]={1,0,0,0,0,0};
      for(unsigned f=0;f<30;++f) CHECK(mu_ranked_setup_tick(&model[player])==MU_SETUP_WAIT);
      CHECK(mu_ranked_setup_packet(&model[player],nullptr,1,packet));
      CHECK(mu_ranked_setup_apply(&model[player],player,packet));
      std::memcpy(response+2,packet+1,4); CHECK(mu_ranked_setup_remote(&model[!player],response));
    }
    CHECK(!std::memcmp(&model[0].result,&model[1].result,sizeof previous)); previous=model[0].result;
    uint8_t overwrite[14]={0}; mu_yg_put16(overwrite,previous.stage);
    for(unsigned i=0;i<2;++i) {overwrite[2+3*i]=1;overwrite[3+3*i]=previous.fighters[i].character;overwrite[4+3*i]=previous.fighters[i].color;}
    CHECK(ranked_cpu::handle(0xBF,overwrite,14,reply));
    uint8_t merge[9]={0};merge[3]=merge[6]=2;
    CHECK(ranked_cpu::handle(0xB5,merge,9,reply)); CHECK(ranked_cpu::handle(0xB3,nullptr,0,reply));
    CHECK(reply[1] && reply[2] && mu_yg_get16(reply.data()+598+0xE)==previous.stage);
    CHECK(reply[598+0x61]==0 && reply[598+0x85]==1); // human/CPU, ordinary local VS
    for(auto& s:sets) CHECK(mu_ranked_setup_record_game(&s,game==2?1:0,2,previous.stage,0)==(game==3?MU_SETUP_SET_OVER:MU_SETUP_NEXT_GAME));
  }
  uint8_t normal=0; CHECK(ranked_cpu::handle(0xC2,&normal,1,reply));
  CHECK(ranked_cpu::handle(0xB3,nullptr,0,reply)); CHECK(reply[0]==0 && !reply[1]);
  ranked_cpu::enabled=false; CHECK(!ranked_cpu::handle(0xB3,nullptr,0,reply));
#else
  auto settings=agent_test::map_keys({{"online","ranked"}},agent_test::Engine::Source);
  CHECK(settings.arguments==std::vector<std::string>({"--online-test","ranked"}));
  CHECK(settings.log=="agent-test.json applied default keys: online");
#endif
  std::printf("Ranked CPU hook: %u checks passed\n",checks);
}
