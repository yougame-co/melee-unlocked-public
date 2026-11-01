// Native GameSetup presentation against A1's actual model. GPL-3.0-or-later.
#include "mu_ranked_widgets.h"
#include "yougame_ranked_policy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
struct Fixture {
  MuRankedSet set{};
  MuRankedSetupModel model{};
  MuRankedSetupView view{};
  Fixture(unsigned seat=0,unsigned game=1,bool mirror=false) {
    set.max_games=3; set.cur_game=game; set.prev_winner=game>1?0:255;
    set.last_stage_win[0]=set.last_stage_win[1]=65535;
    if(game>1) {set.score[0]=1;set.game_results[0]=0;}
    auto policy=yougame::ranked_policy::slippi_v1();
    MuRankedSetupResult result{}; result.stage=31;
    result.fighters[0]={2,0}; result.fighters[1]={(MuYgU8)(mirror?2:20),0};
    CHECK(mu_ranked_setup_model_init(&model,&policy,&set,seat,&result));
    mu_ranked_view_sync(&view,&model);
  }
  int input(unsigned down,unsigned repeat=0) {return mu_ranked_view_input(&view,&model,down,repeat);}
  void commit(bool automatic=false) {
    unsigned char p[5]; CHECK(mu_ranked_setup_packet(&model,&view.draft,automatic,p));
    CHECK(mu_ranked_setup_apply(&model,model.local_player,p));
    mu_ranked_view_sync(&view,&model);
  }
  void remote() {
    unsigned char p[5]; CHECK(mu_ranked_setup_packet(&model,nullptr,1,p));
    const auto* s=mu_ranked_setup_current(&model); CHECK(s && s->player!=model.local_player);
    CHECK(mu_ranked_setup_apply(&model,s->player,p)); mu_ranked_view_sync(&view,&model);
  }
  void choose_stages() {
    auto* s=mu_ranked_setup_current(&model); CHECK(s);
    unsigned count=s->count;
    for(unsigned i=0;i<count;++i) {
      CHECK(!input(MU_SETUP_A));
      if(i+1<count) CHECK(!input(0,MU_SETUP_RIGHT));
    }
    CHECK(view.confirming); CHECK(input(MU_SETUP_A)); commit();
  }
};
static void layout() {
  const unsigned stages[]={2,3,8,28,31,32}, mats[]={6,7,3,4,2,5};
  for(unsigned i=0;i<6;++i) CHECK(mu_ranked_stage_material(stages[i])==mats[i]);
  CHECK(mu_ranked_stage_material(999)==1);
  for(unsigned c=0;c<26;++c) CHECK(mu_ranked_character_material(c)==c+8);
  CHECK(mu_ranked_character_material(26)==1);
  CHECK(mu_ranked_stock_frame(19,3)==119); CHECK(mu_ranked_stock_frame(20,0)==19);
  for(unsigned n=1;n<=6;++n) {
    CHECK(mu_ranked_selector_x(0,n)==-mu_ranked_selector_x(n-1,n));
    for(unsigned i=1;i<n;++i) CHECK(mu_ranked_selector_x(i,n)-mu_ranked_selector_x(i-1,n)==8);
  }
  Fixture f; CHECK(mu_ranked_timeline_x(&f.model,0,0)==-26);
  CHECK(mu_ranked_timeline_x(&f.model,3,1)>mu_ranked_timeline_x(&f.model,3,0)+5);
  for(unsigned c=0;c<27;++c) for(unsigned d:{MU_SETUP_LEFT,MU_SETUP_RIGHT,MU_SETUP_UP,MU_SETUP_DOWN})
    CHECK(mu_ranked_picker_move(c,d)<27);
  for(unsigned c=0;c<27;++c) {
    CHECK(mu_ranked_picker_move(mu_ranked_picker_move(c,MU_SETUP_LEFT),MU_SETUP_RIGHT)==c);
    CHECK(mu_ranked_picker_move(mu_ranked_picker_move(c,MU_SETUP_UP),MU_SETUP_DOWN)==c);
  }
  CHECK(mu_ranked_picker_move(26,MU_SETUP_RIGHT)==21);
  CHECK(mu_ranked_picker_move(6,MU_SETUP_UP)==20);
  for(unsigned mask=0;mask<64;++mask) for(int c=-1;c<6;++c) for(int d:{-1,1}) {
    int next=mu_ranked_selector_move(mask,6,c,d);
    CHECK(mask ? next>=0 && next<6 && (mask&(1u<<next)) : next==-1);
  }
  const unsigned long long raw[]={1,2,4,8,0x40000,0x80000,0x20000,0x10000,0x100,0x200,0x400,0x800,0x1000};
  const unsigned key[]={1,2,4,8,1,2,4,8,16,32,64,128,256};
  for(unsigned i=0;i<13;++i) CHECK(mu_ranked_pad_buttons(raw[i])==key[i]);
  CHECK(mu_ranked_pad_buttons(15ULL<<32)==0); // synthesized confirm/cancel are not scroll
}
static void timers() {
  char text[8];
  CHECK(mu_ranked_timer_text(text,30,0,1)==MU_SETUP_TIMER_WHITE && !std::strcmp(text,"0:30"));
  CHECK(mu_ranked_timer_text(text,30,1199,1)==MU_SETUP_TIMER_WHITE && !std::strcmp(text,"0:11"));
  CHECK(mu_ranked_timer_text(text,30,1200,1)==MU_SETUP_TIMER_YELLOW && !std::strcmp(text,"0:10"));
  CHECK(mu_ranked_timer_text(text,30,1620,1)==MU_SETUP_TIMER_RED && !std::strcmp(text,"0:03"));
  CHECK(mu_ranked_timer_text(text,30,1620,0)==MU_SETUP_TIMER_WHITE);
  CHECK(mu_ranked_timer_text(text,30,999999,1)==MU_SETUP_TIMER_RED && !std::strcmp(text,"0:00"));
  mu_ranked_timer_text(text,255,60,1); CHECK(!std::strcmp(text,"4:14"));
}
static void inputs() {
  Fixture f;
  CHECK(f.model.step==2 && f.view.cursor==0);
  CHECK(!f.input(MU_SETUP_A)); CHECK(f.view.confirming && f.view.draft.count==1);
  CHECK(!f.input(0,MU_SETUP_RIGHT)); CHECK(f.view.button==1);
  CHECK(!f.input(MU_SETUP_A)); CHECK(!f.view.confirming && f.view.draft.count==0);
  CHECK(!f.input(MU_SETUP_A)); CHECK(!f.input(MU_SETUP_B)); CHECK(!f.view.confirming);
  f.choose_stages(); CHECK(f.model.step==3); CHECK(f.view.history_valid[2]);
  auto before=f.view; CHECK(!f.input(MU_SETUP_A,MU_SETUP_RIGHT));
  CHECK(!std::memcmp(&f.view,&before,sizeof before)); // no opponent control
  f.remote(); CHECK(f.model.step==4); f.choose_stages(); CHECK(f.model.status==MU_SETUP_COMPLETE);
  CHECK(f.view.history[3].count==2 && f.view.history_valid[4]);
  CHECK(!f.input(MU_SETUP_A));
  Fixture two(1); two.remote(); CHECK(two.model.step==3);
  CHECK(!two.input(MU_SETUP_A)); CHECK(two.view.draft.count==1 && !two.view.confirming);
  CHECK(!two.input(MU_SETUP_A)); CHECK(two.view.draft.count==0); // toggles
  CHECK(!two.input(MU_SETUP_A)); CHECK(!two.input(MU_SETUP_B)); CHECK(two.view.draft.count==0);
  two.choose_stages(); CHECK(two.model.step==4);
  Fixture mirror(0,1,true); CHECK(mirror.model.step==0 && !mirror.view.confirming);
  CHECK(!mirror.input(MU_SETUP_A)); CHECK(mirror.model.step==0);
  CHECK(!mirror.input(MU_SETUP_X)); CHECK(mirror.view.confirming);
  CHECK(mu_ranked_setup_color_allowed(&mirror.model,0,mirror.view.draft.character,mirror.view.draft.color));
  CHECK(mirror.input(MU_SETUP_A)); mirror.commit(); CHECK(mirror.model.step==1);
  mirror.remote(); CHECK(mirror.model.step==2);
  unsigned frames=0;
  for(unsigned i=0;i<30;++i) CHECK(mu_ranked_disconnect_wait(&frames,MU_SETUP_A));
  CHECK(mu_ranked_disconnect_wait(&frames,0) && frames==30);
  CHECK(!mu_ranked_disconnect_wait(&frames,MU_SETUP_A));
  CHECK(!mu_ranked_disconnect_wait(&frames,MU_SETUP_START));
  Fixture cp; cp.model.status=MU_SETUP_DISCONNECTED; auto frozen=cp.view;
  CHECK(!cp.input(MU_SETUP_A,MU_SETUP_RIGHT)); CHECK(!std::memcmp(&cp.view,&frozen,sizeof frozen));
}
static void characters() {
  Fixture f(0,2); f.choose_stages(); f.remote(); CHECK(f.model.step==2);
  CHECK(f.view.confirming && !f.view.dialog);
  CHECK(!f.input(MU_SETUP_B)); CHECK(f.view.dialog);
  auto saved=f.view.saved;
  CHECK(!f.input(0,MU_SETUP_RIGHT)); CHECK(f.view.draft.character!=saved.character);
  CHECK(!f.input(MU_SETUP_X)); CHECK(!f.input(MU_SETUP_B));
  CHECK(!f.view.dialog && f.view.draft.character==saved.character && f.view.draft.color==saved.color);
  CHECK(!f.input(0,MU_SETUP_RIGHT)); CHECK(f.view.button==1);
  CHECK(!f.input(MU_SETUP_A)); CHECK(f.view.dialog);
  for(unsigned i=0;i<27;++i) {
    f.view.picker=i; if(i<26) f.view.draft.character=i;
    CHECK(!f.input(MU_SETUP_X));
    unsigned char p[5]; CHECK(mu_ranked_setup_packet(&f.model,&f.view.draft,1,p));
  }
  CHECK(f.view.picker==26 && f.view.draft.character<26); // Random timeout remains legal
  CHECK(!f.input(MU_SETUP_A)); CHECK(!f.view.dialog && f.view.draft.character<26);
  CHECK(f.view.button==0); CHECK(f.input(MU_SETUP_A)); f.commit(); f.remote();
  CHECK(f.model.status==MU_SETUP_COMPLETE);
  CHECK(f.view.history_valid[2] && f.view.history[2].character<26);
}
static void walk() {
  for(unsigned seat=0;seat<2;++seat) for(unsigned game=1;game<=3;++game) {
    Fixture f(seat,game);
    unsigned visits=0;
    while(auto* s=mu_ranked_setup_current(&f.model)) {
      CHECK(++visits<=5);
      if(s->player!=seat) {f.remote();continue;}
      if(s->type==MU_SETUP_STRIKE || s->type==MU_SETUP_STAGE) f.choose_stages();
      else { CHECK(f.input(MU_SETUP_A)); f.commit(); }
    }
    CHECK(f.model.status==MU_SETUP_COMPLETE);
    for(unsigned i=0;i<f.model.step_count;++i) CHECK(f.view.history_valid[i]);
  }
  // A half-filled two-strike draft survives sync and supplies a timeout packet.
  Fixture partial(1); partial.remote(); CHECK(!partial.input(MU_SETUP_A));
  auto stage=partial.view.draft.stages[0]; mu_ranked_view_sync(&partial.view,&partial.model);
  CHECK(partial.view.draft.count==1 && partial.view.draft.stages[0]==stage);
  partial.commit(true); CHECK(partial.model.removed_count==3);
  CHECK(partial.view.history[3].count==2);
}
int main(int argc,char** argv) {
  const char* group=argc>1?argv[1]:"all";
  if(!std::strcmp(group,"layout") || !std::strcmp(group,"all")) layout();
  if(!std::strcmp(group,"timers") || !std::strcmp(group,"all")) timers();
  if(!std::strcmp(group,"inputs") || !std::strcmp(group,"all")) inputs();
  if(!std::strcmp(group,"characters") || !std::strcmp(group,"all")) characters();
  if(!std::strcmp(group,"walk") || !std::strcmp(group,"all")) walk();
  CHECK(checks>0); std::printf("Ranked widgets %s: %u checks passed\n",group,checks);
}
