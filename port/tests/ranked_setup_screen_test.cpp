// A2 input drafts through A1's real adapter and paired CPU model. GPL-3.0-or-later.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "yougame_ranked_policy.h"
extern "C" {
#include <mu_native.h>
#include <mu_ranked_setup_online.h>
#include "mu_ranked_widgets.h"
}
static unsigned checks, exits, sends, overwrites, cleanups;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static MuRankedSet state;
static MuMatchState match;
extern "C" {
void mu_online_abi_log(const char*) {}
MuRankedSet* mu_ranked_set_mut() {return &state;}
const MuRankedSet* mu_ranked_set() {return &state;}
int mu_ranked_read_policy(MuRankedPolicy* out) {*out=yougame::ranked_policy::slippi_v1();return 1;}
const MuMatchState* mu_slippi_match_state() {return &match;}
int mu_slippi_load_match_state(MuMatchState* out) {if(out)*out=match;return 0;}
void gm_801A4B60() {++exits;}
void mu_slippi_cleanup_connections() {++cleanups;match.connection_state=MU_SLP_MM_IDLE;}
void mu_slippi_set_selections(const MuSlippiSelections* s) {
  CHECK(s->char_opt==2 && s->stage_opt==MU_SLP_STAGE_PICK); match.local_ready=match.remote_ready=1;
}
int mu_slippi_cmd(unsigned cmd,const void* data,unsigned n,void* reply,unsigned capacity,unsigned* got) {
  auto* p=(const unsigned char*)data; if(got)*got=0;
  if(cmd==0xC1) {CHECK(n==1 && capacity==6);std::memset(reply,0,6);if(got)*got=6;}
  else if(cmd==0xC0) {CHECK(n==5);++sends;}
  else if(cmd==0xBF) {
    CHECK(n==14);++overwrites;std::memcpy(match.game_info+0xE,p,2);
    for(unsigned i=0;i<2;++i) {match.game_info[0x60+0x24*i]=p[3+3*i];match.game_info[0x63+0x24*i]=p[4+3*i];}
  } else if(cmd==0xC4) CHECK(n==1 && p[0]==9+state.cur_game);
  else if(cmd==0xC2) CHECK(n==1 && p[0]==1);
  else CHECK(false);
  return 0;
}
}
static void start(unsigned seat) {
  state={};state.max_games=3;state.cur_game=1;state.prev_winner=255;
  state.last_stage_win[0]=state.last_stage_win[1]=65535;
  match={};match.connection_state=MU_SLP_MM_CONNECTION_SUCCESS;
  match.local_index=seat;match.remote_index=!seat;
  std::strcpy(match.match_id,"agent-ranked-cpu");
  match.game_info[0x60]=2;match.game_info[0x84]=20;
  mu_yg_put16(match.game_info+0xE,31);exits=sends=overwrites=cleanups=0;
}
static void press(MuRankedSetupView& v,unsigned down,unsigned repeat=0) {
  const auto* m=mu_ranked_setup_model();
  if(mu_ranked_view_input(&v,m,down,repeat)) CHECK(mu_ranked_setup_submit(&v.draft,0));
  if(v.step==m->step) mu_ranked_setup_set_draft(&v.draft);
  mu_ranked_setup_frame();mu_ranked_view_sync(&v,m);
}
static void game(unsigned winner) {
  unsigned expected_exit=exits+1;
  MuRankedSetupView v{};mu_ranked_setup_begin();CHECK(mu_ranked_setup_cpu());
  const auto* m=mu_ranked_setup_model();mu_ranked_view_sync(&v,m);
  unsigned visited[4]={}, guard=0;
  while(auto* s=mu_ranked_setup_current(m)) {
    CHECK(++guard<1000);++visited[s->type];
    if(s->player!=m->local_player) {press(v,0);continue;}
    if(s->type==MU_SETUP_STRIKE || s->type==MU_SETUP_STAGE) {
      unsigned count=s->count;
      for(unsigned i=0;i<count;++i) {press(v,MU_SETUP_A);if(i+1<count)press(v,0,MU_SETUP_RIGHT);}
      CHECK(v.confirming);press(v,MU_SETUP_A);
    } else {
      // Exercise dialog navigation and costume changes through the timeout draft path.
      press(v,MU_SETUP_B);CHECK(v.dialog);press(v,0,MU_SETUP_DOWN);press(v,MU_SETUP_X);
      press(v,MU_SETUP_A);CHECK(!v.dialog);press(v,MU_SETUP_A);
    }
  }
  CHECK(m->status==MU_SETUP_COMPLETE && m->result.stage!=0);
  CHECK(visited[MU_SETUP_STRIKE]);
  if(state.cur_game>1) CHECK(visited[MU_SETUP_STAGE] && visited[MU_SETUP_CHARACTER]);
  match.local_ready=match.remote_ready=1;press(v,0);CHECK(exits==expected_exit);
  auto stage=m->result.stage;mu_ranked_setup_game_result(winner,2,0);
  int result=mu_ranked_setup_finish_game(1,stage);
  CHECK(result==MU_SETUP_NEXT_GAME || result==MU_SETUP_SET_OVER);
  mu_ranked_setup_set_draft(nullptr);mu_ranked_setup_end();
}
int main() {
  for(unsigned seat=0;seat<2;++seat) {
    start(seat);game(seat);game(!seat);game(seat);
    CHECK(state.score[seat]==2 && state.score[!seat]==1 && overwrites==3 && !cleanups);
    start(seat);game(!seat);game(!seat); // game 2 loser gets the local counterpick
    CHECK(state.score[!seat]==2 && overwrites==2);
  }
  // A1 aborts, A2 freezes editing and can draw its disconnected presentation.
  start(0);mu_ranked_setup_begin();MuRankedSetupView v{};
  mu_ranked_view_sync(&v,mu_ranked_setup_model());match.connection_state=MU_SLP_MM_IDLE;
  press(v,0);CHECK(mu_ranked_setup_model()->status==MU_SETUP_DISCONNECTED && exits==1 && cleanups==1);
  auto draft=v.draft;press(v,MU_SETUP_A,MU_SETUP_RIGHT);
  CHECK(!std::memcmp(&draft,&v.draft,sizeof draft));
  std::printf("Ranked screen CPU adapter: %u checks passed\n",checks);
}
