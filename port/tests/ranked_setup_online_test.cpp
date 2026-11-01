// Actual scene adapter with a deterministic local bridge, no game engine/drawing.
// GPL-3.0-or-later.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <array>
#include <string>
#include "yougame_ranked_policy.h"
#include "yougame_ranked.h"
extern "C" {
#include <mu_native.h>
#include <mu_ranked_setup_online.h>
}
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
static MuMatchState match;
static MuRankedPolicy policy;
static MuRankedSet state, peer_set;
static MuRankedSetupModel peer;
static std::vector<unsigned> commands;
static std::vector<std::string> set_lines;
static std::array<unsigned char,14> overwritten;
static MuSlippiSelections selected;
static unsigned exits, cleanups, merges, c0_count, c2_count, c2_mode;
static bool no_remote, invalid_remote, fail_load, fail_policy;
static bool stadium_counterpick;
static unsigned fail_command;
static const MuRankedSetupChoice* stage_choice(const MuRankedSetupModel* m, MuRankedSetupChoice& choice) {
  const auto* step=mu_ranked_setup_current(m);
  if(!stadium_counterpick || !step || (step->type!=MU_SETUP_STRIKE && step->type!=MU_SETUP_STAGE))return nullptr;
  choice={};choice.count=1;choice.stages[0]=step->type==MU_SETUP_STRIKE?31:3;
  return &choice;
}
extern "C" {
void mu_online_abi_log(const char* line) {set_lines.emplace_back(line);}
MuRankedSet* mu_ranked_set_mut() {return &state;}
const MuRankedSet* mu_ranked_set() {return &state;}
int mu_ranked_read_policy(MuRankedPolicy* out) {*out=policy;return !fail_policy;}
const MuMatchState* mu_slippi_match_state() {return &match;}
int mu_slippi_load_match_state(MuMatchState* out) {if(out)*out=match;return fail_load?-1:0;}
void gm_801A4B60() {++exits;}
void mu_slippi_cleanup_connections() {++cleanups;match.connection_state=MU_SLP_MM_IDLE;}
void mu_slippi_set_selections(const MuSlippiSelections* s) {
  CHECK(s->char_opt==2 && s->stage_opt==MU_SLP_STAGE_PICK && s->online_mode==MU_SLP_MODE_RANKED);
  selected=*s; ++merges;
}
int mu_slippi_cmd(unsigned cmd,const void* data,unsigned n,void* reply,unsigned capacity,unsigned* got) {
  commands.push_back(cmd); if(got)*got=0; const auto* p=(const unsigned char*)data;
  if(cmd==fail_command) return -1;
  if(cmd==0xC0) {
    CHECK(n==5); ++c0_count;
    if(!mu_ranked_setup_cpu()) {
      unsigned char r[6]={1,0,p[1],p[2],p[3],p[4]}; CHECK(mu_ranked_setup_remote(&peer,r));
    }
  } else if(cmd==0xC1) {
    CHECK(n==1 && capacity==6 && reply); unsigned char r[6]={0};
    if(!no_remote && !mu_ranked_setup_cpu()) {
      unsigned char packet[5]; MuRankedSetupChoice choice{};
      CHECK(mu_ranked_setup_packet(&peer,stage_choice(&peer,choice),1,packet));
      CHECK(mu_ranked_setup_apply(&peer,peer.local_player,packet));
      r[0]=1; std::memcpy(r+2,packet+1,4);
      if(invalid_remote) r[4]=255;
    }
    std::memcpy(reply,r,6); if(got)*got=6;
  } else if(cmd==0xBF) {CHECK(n==14);std::memcpy(overwritten.data(),p,14);}
  else if(cmd==0xC2) {CHECK(n==1);++c2_count;c2_mode=p[0];}
  else if(cmd==0xC4) {CHECK(n==1 && p[0]==9+state.cur_game);}
  else CHECK(false);
  return 0;
}
}
static void reset(unsigned seat=0,unsigned game=1,bool cpu=false,bool mirror=false) {
  match={};state={};peer_set={};peer={};policy=yougame::ranked_policy::slippi_v1();commands.clear();set_lines.clear();overwritten={};
  exits=cleanups=merges=c0_count=c2_count=c2_mode=0;
  no_remote=invalid_remote=fail_load=fail_policy=stadium_counterpick=false;fail_command=0;selected={};
  state.max_games=3;state.cur_game=game;state.prev_winner=game>1?0:255;
  state.last_stage_win[0]=state.last_stage_win[1]=65535;
  match.connection_state=MU_SLP_MM_CONNECTION_SUCCESS;match.local_ready=match.remote_ready=1;
  match.local_index=(unsigned char)seat; match.remote_index=(unsigned char)!seat;
  mu_yg_put16(match.game_info+0xE,31);match.game_info[0x60]=2;match.game_info[0x84]=mirror?2:20;
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
  if(cpu)std::strcpy(match.match_id,"agent-ranked-cpu");
#else
  (void)cpu;
#endif
  MuRankedSetupResult r{};r.stage=31;r.fighters[0]={2,0};r.fighters[1]={(unsigned char)(mirror?2:20),0};
  peer_set=state; CHECK(mu_ranked_setup_model_init(&peer,&policy,&peer_set,!seat,&r));
}
static void begin() {mu_ranked_setup_begin();CHECK(mu_ranked_setup_model()->status!=MU_SETUP_DISCONNECTED);}
static void step() {
  auto* m=mu_ranked_setup_model(); const auto* s=mu_ranked_setup_current(m);CHECK(s);
  if(s->player==m->local_player)CHECK(mu_ranked_setup_submit(nullptr,1));
  else mu_ranked_setup_frame();
}
static void normal() {
  for(unsigned seat=0;seat<2;++seat)for(unsigned game=1;game<=2;++game)for(unsigned mirror=0;mirror<2;++mirror) {
    reset(seat,game,false,mirror);begin();
    while(mu_ranked_setup_current(mu_ranked_setup_model()))step();
    auto* m=mu_ranked_setup_model();CHECK(m->status==MU_SETUP_COMPLETE);
    CHECK(!std::memcmp(&m->result,&peer.result,sizeof m->result));
    CHECK(mu_yg_get16(overwritten.data())==m->result.stage && overwritten[2] && overwritten[5]);
    CHECK(overwritten[8]==0 && overwritten[11]==0);
    CHECK(merges==(game>1?1u:0u) && c0_count>0 && !exits);
    match.remote_ready=0;mu_ranked_setup_frame();CHECK(!exits);
    match.remote_ready=1;mu_ranked_setup_frame();CHECK(exits==1 && !cleanups);
    mu_ranked_setup_frame();CHECK(exits==1);mu_ranked_setup_end();
    mu_ranked_setup_game_result(0,2,0);CHECK(mu_ranked_setup_finish_game(1,m->result.stage)==MU_SETUP_NEXT_GAME);
    CHECK(state.cur_game==game+1 && state.score[0]==1);
  }
}
static void game_two_counterpick() {
  for(unsigned seat=0;seat<2;++seat) {
    reset(seat,2);stadium_counterpick=true;begin();
    const auto* m=mu_ranked_setup_model();
    while(const auto* s=mu_ranked_setup_current(m)) {
      if(s->player==seat) {
        MuRankedSetupChoice choice{};
        CHECK(mu_ranked_setup_submit(stage_choice(m,choice),1));
      } else mu_ranked_setup_frame();
    }
    CHECK(m->removed_count==1 && m->removed[0]==31); // winner bans Battlefield
    CHECK(m->result.stage==3 && peer.result.stage==3); // loser picks Stadium
    CHECK(mu_yg_get16(overwritten.data())==3 && overwritten[2] && overwritten[5]);
    CHECK(merges==1 && selected.stage_id==3 && selected.stage_opt==MU_SLP_STAGE_PICK);
    mu_ranked_setup_frame();CHECK(exits==1 && !cleanups);
  }
}
static void disconnects() {
  for(unsigned game=1;game<=2;++game)for(unsigned seat=0;seat<2;++seat)for(unsigned stop=0;stop<6;++stop) {
    reset(seat,game,false,true);begin();
    for(unsigned i=0;i<stop && mu_ranked_setup_current(mu_ranked_setup_model());++i)step();
    match.connection_state=MU_SLP_MM_IDLE;mu_ranked_setup_frame();
    CHECK(mu_ranked_setup_model()->status==MU_SETUP_DISCONNECTED && exits==1 && cleanups==1);
    CHECK(c2_count==1 && c2_mode==1);mu_ranked_setup_frame();CHECK(exits==1);
  }
  reset();fail_load=true;mu_ranked_setup_begin();CHECK(exits==1 && c2_count==1);
  reset();fail_policy=true;mu_ranked_setup_begin();CHECK(exits==1 && c2_count==1);
  reset();fail_command=0xC0;begin();CHECK(!mu_ranked_setup_submit(nullptr,1));CHECK(exits==1 && c2_count==1);
  reset(1);invalid_remote=true;begin();mu_ranked_setup_frame();CHECK(exits==1 && c2_count==1);
}
static void timers() {
  reset(1);no_remote=true;begin();auto* m=mu_ranked_setup_model();auto* s=mu_ranked_setup_current(m);
  unsigned limit=60u*(s->seconds+policy.grace_seconds+policy.wait_seconds);
  for(unsigned i=0;i<limit;++i)mu_ranked_setup_frame();CHECK(!exits);
  mu_ranked_setup_frame();CHECK(exits==1 && c2_count==1 && cleanups==1);
  reset(1);begin();step(); // remote first strike, then local double strike
  MuRankedSetupChoice draft{};draft.count=1;draft.stages[0]=32;mu_ranked_setup_set_draft(&draft);
  m=mu_ranked_setup_model();s=mu_ranked_setup_current(m);CHECK(s->count==2 && s->player==1);
  limit=60u*(s->seconds+policy.grace_seconds);
  for(unsigned i=0;i<=limit;++i)mu_ranked_setup_frame();
  CHECK(m->removed_count==3 && m->removed[1]==28 && m->removed[2]==32);
  while(mu_ranked_setup_current(m))step();match.remote_ready=0;
  for(unsigned i=0;i<900;++i)mu_ranked_setup_frame();CHECK(!exits);
  mu_ranked_setup_frame();CHECK(exits==1 && c2_mode==1);
}
static void tiebreak() {
  reset();mu_ranked_setup_game_result(-1,7,0);
  CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_TIEBREAK);
  CHECK(state.cur_game==1 && state.tiebreak_num==1 && state.last_end_mode==7 && !state.score[0]);
  reset();mu_ranked_setup_game_result(0,2,1);
  CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_SET_ABORTED && !state.score[0]);
  reset();state.tiebreak_num=1;begin();CHECK(commands.empty() && merges==1);
  CHECK(mu_ranked_setup_model()->status==MU_SETUP_COMPLETE);mu_ranked_setup_frame();CHECK(exits==1);
  reset();state.tiebreak_num=5;begin();mu_ranked_setup_frame();CHECK(exits==1 && c2_count==1 && cleanups==1);
}
static void desync_replay_hosts() {
  using namespace yougame::ranked;
  Set hosts[2];
  std::vector<json> reports[2];
  const int placements[4]={255,255,255,255};
  for(unsigned seat=0;seat<2;++seat) {
    auto& host=hosts[seat];host.configure(seat,{"a","b"});
    host.start_game("replay-game");
    for(const auto& msg:host.due(1,seat==0,1))host.sent(msg);
    host.receive({{"t","start"},{"matchId","set"}});
    for(const auto& msg:host.due(1,seat==0,1))host.sent(msg);
    // h_replay_event publishes method 7 with placements first. Both remote
    // pausers would award opposite winners if this record were classified.
    CHECK(no_contest_waits_for_report(7,true,true));
    CHECK(!no_contest_waits_for_report(7,true,false)); // casual policy unchanged
    CHECK(game_winner(7,!seat,-1,placements)==(int)seat);
    CHECK(!host.awaiting_receipt() && !host.over());
    CHECK(!no_contest_waits_for_report(7,false,true));
    auto game=game_outcome(7,!seat,-2,nullptr); // finalized connected desync
    CHECK(game.winner==-1 && !game.forfeit);
    host.report_game(game);
    CHECK(host.awaiting_receipt() && !host.decided());
    reports[seat]=host.due(2,seat==0,1);
    CHECK(reports[seat].size()==1 && reports[seat][0]["t"]=="game-report");
    CHECK(reports[seat][0]["void"]==true && !reports[seat][0].count("winner"));
    host.sent(reports[seat][0]);
  }
  CHECK(reports[0][0]==reports[1][0]);
  // The room settles the agreeing void reports. The production readiness
  // predicate must release both clients before the scene can replay.
  for(auto& host:hosts) {
    host.receive({{"t","game-result"},{"matchId","set"},
      {"id",reports[0][0]["id"]},{"void",true},{"ranking",{"a","b"}}});
    CHECK(host.games_received()==1);
    CHECK(!host.awaiting_receipt() && !host.over() && !host.decided());
    CHECK(host.due(3,false,1).empty()); // no set finish, and no forfeit latch
  }
  for(unsigned seat=0;seat<2;++seat) {
    reset(seat);mu_ranked_setup_game_result(-1,7,0);
    CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_TIEBREAK);
    begin();CHECK(mu_ranked_setup_model()->status==MU_SETUP_COMPLETE);
    match.local_ready=!(hosts[seat].awaiting_receipt() || hosts[seat].over());
    match.remote_ready=!(hosts[!seat].awaiting_receipt() || hosts[!seat].over());
    mu_ranked_setup_frame();CHECK(exits==1 && !cleanups && !c2_count);
  }
  // A real disconnect still names the same losing port on both hosts and
  // immediately decides the set by forfeit, retaining the readiness hold.
  for(auto& host:hosts) {
    host.start_game("disconnect-game");
    auto game=game_outcome(7,1,-3,nullptr);
    CHECK(game.winner==0 && game.forfeit);
    host.report_game(game);CHECK(host.decided() && host.over());
    reset();mu_ranked_setup_game_result(-1,7,1);
    CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_SET_ABORTED);
  }
}
static void cpu() {
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
  for(unsigned game=1;game<=3;++game) {
    reset(0,game,true,true);begin();CHECK(mu_ranked_setup_cpu());
    unsigned frames=0;
    while(mu_ranked_setup_current(mu_ranked_setup_model())) {
      const auto* s=mu_ranked_setup_current(mu_ranked_setup_model());
      if(s->player==0)CHECK(mu_ranked_setup_submit(nullptr,1));
      else {mu_ranked_setup_frame();++frames;}
      CHECK(frames<=150);
    }
    CHECK(frames>=30 && frames%30==0);
    CHECK(mu_ranked_setup_model()->status==MU_SETUP_COMPLETE);mu_ranked_setup_frame();CHECK(exits==1);
  }
#else
  reset();CHECK(!mu_ranked_setup_cpu()); // production adapter has no test path
#endif
}
static void update_lifecycle() {
  reset(); mu_ranked_update_reset(); CHECK(!mu_ranked_update_busy());
  mu_ranked_update_result_shown(); CHECK(!mu_ranked_update_busy());
  begin(); CHECK(mu_ranked_update_busy());
  mu_ranked_update_result_shown(); CHECK(mu_ranked_update_busy()); // idle CSS cannot clear game one
  mu_ranked_setup_game_result(0,2,0);
  CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_NEXT_GAME);
  CHECK(mu_ranked_update_busy());
  mu_ranked_update_result_shown(); CHECK(mu_ranked_update_busy()); // between games
  mu_ranked_setup_begin(); CHECK(mu_ranked_update_busy());
  mu_ranked_setup_game_result(0,2,0);
  CHECK(mu_ranked_setup_finish_game(1,32)==MU_SETUP_SET_OVER);
  CHECK(mu_ranked_update_busy()); // room cleanup / set-over log is earlier than the result reveal
  mu_ranked_update_result_shown(); CHECK(!mu_ranked_update_busy());
  reset(); begin(); mu_ranked_setup_game_result(-1,7,1);
  CHECK(mu_ranked_setup_finish_game(0,31)==MU_SETUP_SET_ABORTED);
  CHECK(mu_ranked_update_busy());
  mu_ranked_update_reset(); CHECK(!mu_ranked_update_busy()); // online-major exit/abandon
}
static void result_logging() {
  for(unsigned seat=0;seat<2;++seat) for(bool practice : {false,true}) {
#if !defined(YOUGAME_AGENT_TEST_HOOK) && !(defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
    if(practice)continue;
#endif
    const std::string p = practice ? "1" : "0";
    // Practice and online: a 2-0 sweep and a 2-1 comeback, identical from either local seat.
    reset(seat,1,practice);
    mu_ranked_setup_game_result(seat,2,0);CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_NEXT_GAME);
    mu_ranked_setup_game_result(seat,2,0);CHECK(mu_ranked_setup_finish_game(1,32)==MU_SETUP_SET_OVER);
    CHECK((set_lines==std::vector<std::string>{
      "ranked set: game 1 over, winner local, score 1-0, practice "+p+", next setup\n",
      "ranked set: game 2 over, winner local, score 2-0, practice "+p+", next set over\n",
      "ranked set: over 2-0, practice "+p+"\n"}));
    reset(seat,1,practice);
    mu_ranked_setup_game_result(!seat,2,0);CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_NEXT_GAME);
    mu_ranked_setup_game_result(seat,2,0);CHECK(mu_ranked_setup_finish_game(1,32)==MU_SETUP_NEXT_GAME);
    mu_ranked_setup_game_result(seat,2,0);CHECK(mu_ranked_setup_finish_game(1,3)==MU_SETUP_SET_OVER);
    CHECK((set_lines==std::vector<std::string>{
      "ranked set: game 1 over, winner opponent, score 0-1, practice "+p+", next setup\n",
      "ranked set: game 2 over, winner local, score 1-1, practice "+p+", next setup\n",
      "ranked set: game 3 over, winner local, score 2-1, practice "+p+", next set over\n",
      "ranked set: over 2-1, practice "+p+"\n"}));
    reset(seat,1,practice);
    mu_ranked_setup_game_result(-1,7,0);CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_TIEBREAK);
    mu_ranked_setup_game_result(!seat,2,0);CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_NEXT_GAME);
    mu_ranked_setup_game_result(!seat,2,0);CHECK(mu_ranked_setup_finish_game(1,32)==MU_SETUP_SET_OVER);
    CHECK((set_lines==std::vector<std::string>{
      "ranked set: game 1 over, winner tie, score 0-0, practice "+p+", next tiebreak\n",
      "ranked set: game 1 over, winner opponent, score 0-1, practice "+p+", next setup\n",
      "ranked set: game 2 over, winner opponent, score 0-2, practice "+p+", next set over\n",
      "ranked set: over 0-2, practice "+p+"\n"}));
    reset(seat,1,practice);
    mu_ranked_setup_game_result(seat,2,1);CHECK(mu_ranked_setup_finish_game(1,31)==MU_SETUP_SET_ABORTED);
    std::strcpy(match.error,"Lost connection to the mm server");match.connection_state=MU_SLP_MM_ERROR;
    mu_ranked_setup_game_result(seat,2,1);CHECK(mu_ranked_setup_finish_game(0,31)==MU_SETUP_SET_ABORTED);
    // A disconnect shown during the game abandons the set: no winner or set-over line, and a line
    // saying which condition ended it, with the link's error text when it has one.
    const std::string ok=std::to_string(MU_SLP_MM_CONNECTION_SUCCESS), err=std::to_string(MU_SLP_MM_ERROR);
    const std::string game="ranked set: game 1 over, winner none, score 0-0, practice "+p+", next abandoned\n";
    const std::string why="ranked set: abandoned after game 1: disconnect shown 1, link state ", w=", winner "+std::to_string(seat)+"\n";
    CHECK((set_lines==std::vector<std::string>{
      game, why+ok+" (connected)"+w,
      game, why+err+" (not connected)"+w,
      "ranked set: link error: Lost connection to the mm server\n"}));
    // 11-01, a set: a conclusive game counts even when the read after it says "not connected"
    // (Slippi ends a set only on the disconnect shown in game); the setup screen checks the link.
    reset(seat,1,practice);set_lines.clear();match.connection_state=MU_SLP_MM_IDLE;
    mu_ranked_setup_game_result(seat,2,0);CHECK(mu_ranked_setup_finish_game(0,31)==MU_SETUP_NEXT_GAME);
    CHECK(state.score[seat]==1 && state.cur_game==2);
    CHECK((set_lines==std::vector<std::string>{
      "ranked set: game 1 over, winner local, score 1-0, practice "+p+", next setup\n",
      "ranked set: game 1 counted although the link read state "+std::to_string(MU_SLP_MM_IDLE)+
        " after it; the next screen checks the link again\n"}));
    mu_ranked_setup_begin();CHECK(mu_ranked_setup_model()->status==MU_SETUP_DISCONNECTED && c2_mode==1);
    // The set ended in the setup scene: its result reveal releases the update gate.
    CHECK(mu_ranked_update_busy());mu_ranked_update_result_shown();CHECK(!mu_ranked_update_busy());
  }
}
int main() {normal();game_two_counterpick();disconnects();timers();tiebreak();desync_replay_hosts();cpu();update_lifecycle();result_logging();std::printf("Ranked setup adapter: %u checks passed\n",checks);}
