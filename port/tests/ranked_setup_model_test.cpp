// Direct C model tests; no game engine linked. GPL-3.0-or-later.
#include "mu_ranked_setup_model.h"
#include "yougame_ranked_policy.h"
#include "mu_setup_hash.h"
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <array>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
static MuRankedSet fresh() {
  MuRankedSet s{}; s.max_games=3; s.cur_game=1; s.prev_winner=255;
  std::memset(s.game_results,255,sizeof s.game_results); s.last_stage_win[0]=s.last_stage_win[1]=65535; return s;
}
static MuRankedSetupResult initial(bool collision=false) {
  MuRankedSetupResult r{}; r.stage=31; r.fighters[0]={2,0}; r.fighters[1]={(MuYgU8)(collision?2:20),0}; return r;
}
static void automatic(MuRankedSetupModel& m) {
  MuYgU8 b[5]; const auto* s=mu_ranked_setup_current(&m); CHECK(s);
  unsigned player=s->player; CHECK(mu_ranked_setup_packet(&m,nullptr,1,b));
  CHECK(mu_ranked_setup_apply(&m,player,b));
}
static void stages() {
  const auto p=yougame::ranked_policy::slippi_v1(); auto set=fresh(); auto r=initial(); MuRankedSetupModel m;
  CHECK(mu_ranked_setup_model_init(&m,&p,&set,0,&r)); CHECK(m.step==2 && m.step_count==5);
  for (unsigned i=0;i<3;++i) {
    const auto* s=mu_ranked_setup_current(&m); CHECK(s && s->type==MU_SETUP_STRIKE);
    CHECK(s->player==p.strike_players[i] && s->count==p.strike_counts[i] && s->seconds==p.strike_seconds[i]);
    unsigned legal=0; for(auto stage:p.starters) legal+=mu_ranked_setup_stage_allowed(&m,stage);
    CHECK(legal==5-m.removed_count); automatic(m);
  }
  CHECK(m.status==MU_SETUP_COMPLETE && m.result.stage==31 && m.removed_count==4);
  CHECK(mu_ranked_setup_record_game(&set,0,2,m.result.stage,0)==MU_SETUP_NEXT_GAME);
  CHECK(set.cur_game==2 && set.score[0]==1 && set.prev_winner==0 && set.last_stage_win[0]==31);
  CHECK(mu_ranked_setup_record_game(&set,1,2,2,0)==MU_SETUP_NEXT_GAME); CHECK(set.cur_game==3);
  r=m.result; CHECK(mu_ranked_setup_model_init(&m,&p,&set,1,&r));
  CHECK(m.steps[0].player==1 && m.steps[1].player==0 && m.steps[2].player==1 && m.steps[3].player==0);
  CHECK(!mu_ranked_setup_stage_allowed(&m,31)); // DSR also forbids banning loser's last win
  automatic(m); CHECK(m.removed[0]==2); CHECK(!mu_ranked_setup_stage_allowed(&m,2));
  CHECK(!mu_ranked_setup_stage_allowed(&m,31)); automatic(m); CHECK(m.result.stage==28);
  automatic(m); automatic(m); CHECK(m.status==MU_SETUP_COMPLETE);
  CHECK(mu_ranked_setup_record_game(&set,1,2,m.result.stage,0)==MU_SETUP_SET_OVER);
  CHECK(set.cur_game==3 && set.score[1]==2 && set.game_results[2]==1);
  auto changed=p; changed.dsr=0; set.cur_game=2;
  CHECK(mu_ranked_setup_model_init(&m,&changed,&set,0,&r)); CHECK(mu_ranked_setup_stage_allowed(&m,31));
  changed.strike_players[0]=1; changed.strike_seconds[0]=7; set=fresh();
  CHECK(mu_ranked_setup_model_init(&m,&changed,&set,0,&r)); CHECK(m.steps[2].player==1 && m.steps[2].seconds==7);
  changed=p; changed.available=0; CHECK(!mu_ranked_setup_model_init(&m,&changed,&set,0,&r)); CHECK(m.status==MU_SETUP_INVALID);
  changed=p; changed.starters[0]=300; CHECK(!mu_ranked_setup_model_init(&m,&changed,&set,0,&r));
  changed=p; changed.counterpicks[5]=changed.counterpicks[0]; CHECK(!mu_ranked_setup_model_init(&m,&changed,&set,0,&r));
  changed=p; changed.strike_counts[1]=3; CHECK(!mu_ranked_setup_model_init(&m,&changed,&set,0,&r));
}
static void colors() {
  auto p=yougame::ranked_policy::slippi_v1(); auto set=fresh(); auto r=initial(true); MuRankedSetupModel m;
  CHECK(mu_ranked_setup_model_init(&m,&p,&set,0,&r)); CHECK(m.step==0 && set.color_ban.active);
  CHECK(!mu_ranked_setup_color_allowed(&m,0,2,0)); CHECK(mu_ranked_setup_next_color(&m,0,2,0,-1)==3);
  CHECK(mu_ranked_setup_next_color(&m,0,2,1,0)==1);
  CHECK(mu_ranked_setup_next_color(&m,0,2,0,0)==1);
  MuRankedSetupChoice choice{}; choice.character=2; choice.color=0; MuYgU8 b[5];
  CHECK(!mu_ranked_setup_packet(&m,&choice,0,b)); automatic(m); CHECK(m.result.fighters[0].color==1);
  CHECK(!mu_ranked_setup_color_allowed(&m,1,2,1)); automatic(m); CHECK(m.result.fighters[1].color==2);
  while(mu_ranked_setup_current(&m)) automatic(m);
  CHECK(m.status==MU_SETUP_COMPLETE && m.result.fighters[0].color!=m.result.fighters[1].color);
  set.cur_game=2; set.prev_winner=0; r=m.result;
  CHECK(mu_ranked_setup_model_init(&m,&p,&set,0,&r)); CHECK(!mu_ranked_setup_color_allowed(&m,1,2,0));
  // Sheik/Zelda collision, wrap, and every vanilla palette (external character IDs).
  static const unsigned counts[]={6,5,4,4,6,4,5,4,5,5,4,4,5,4,4,5,5,6,5,5,4,5,5,5,4,5};
  for(unsigned i=0;i<26;++i) CHECK(mu_ranked_setup_color_count(i)==counts[i]);
  set=fresh(); r.fighters[0]={18,0}; r.fighters[1]={19,0};
  CHECK(mu_ranked_setup_model_init(&m,&p,&set,0,&r)); CHECK(m.step==0 && set.color_ban.active);
  CHECK(!mu_ranked_setup_color_allowed(&m,0,19,0)); automatic(m); automatic(m);
  CHECK(m.result.fighters[0].color==1 && m.result.fighters[1].color==2);
  r=initial(); r.fighters[0].color=99; set=fresh();
  CHECK(mu_ranked_setup_model_init(&m,&p,&set,0,&r)); CHECK(m.result.fighters[0].color==0);
}
static void timers() {
  const auto p=yougame::ranked_policy::slippi_v1();
  for(unsigned game=1;game<=2;++game) for(unsigned seat=0;seat<2;++seat) {
    auto set=fresh(); if(game==2) {set.cur_game=2;set.prev_winner=0;}
    auto r=initial(true); MuRankedSetupModel m; CHECK(mu_ranked_setup_model_init(&m,&p,&set,seat,&r));
    while(const auto* s=mu_ranked_setup_current(&m)) {
      const bool local=s->player==seat;
      const unsigned limit=60u*(s->seconds+p.grace_seconds+(local?0:p.wait_seconds));
      for(unsigned f=0;f<limit;++f) CHECK(mu_ranked_setup_tick(&m)==MU_SETUP_WAIT);
      CHECK(mu_ranked_setup_tick(&m)==(local?MU_SETUP_LOCAL_TIMEOUT:MU_SETUP_REMOTE_TIMEOUT));
      automatic(m); CHECK(m.frames==0);
    }
    CHECK(m.status==MU_SETUP_COMPLETE);
  }
  // Partial two-strike draft preserves the choice and fills the first enabled entry once.
  auto set=fresh(); auto r=initial(); MuRankedSetupModel m; MuYgU8 b[5];
  CHECK(mu_ranked_setup_model_init(&m,&p,&set,0,&r)); automatic(m);
  MuRankedSetupChoice draft{}; draft.count=1; draft.stages[0]=32;
  CHECK(!mu_ranked_setup_packet(&m,&draft,0,b)); CHECK(mu_ranked_setup_packet(&m,&draft,1,b));
  CHECK(b[3]==28 && b[4]==32); CHECK(mu_ranked_setup_apply(&m,1,b));
}
static void interleave() {
  const auto p=yougame::ranked_policy::slippi_v1();
  // All possible starting-stage choices and both winners, with both seat views.
  for(unsigned mirror=0;mirror<2;++mirror) for(unsigned winner=0;winner<2;++winner) for(unsigned offset=0;offset<5;++offset) {
    auto sets=std::array<MuRankedSet,2>{fresh(),fresh()}; auto r=initial(mirror); MuRankedSetupModel m[2];
    for(unsigned game=0;game<3;++game) {
      for(unsigned seat=0;seat<2;++seat) CHECK(mu_ranked_setup_model_init(&m[seat],&p,&sets[seat],seat,&r));
      while(const auto* s=mu_ranked_setup_current(&m[0])) {
        unsigned player=s->player; MuYgU8 b[5], reply[6]={1,0,0,0,0,0}; MuRankedSetupChoice draft{};
        const MuRankedSetupChoice* d=nullptr;
        if(s->type==MU_SETUP_STRIKE || s->type==MU_SETUP_STAGE) {
          const auto* pool=game?p.counterpicks:p.starters; unsigned n=game?6:5;
          for(unsigned i=0;i<n && draft.count<s->count;++i) {
            auto stage=pool[(i+offset)%n]; if(mu_ranked_setup_stage_allowed(&m[player],stage)) draft.stages[draft.count++]=stage;
          }
          d=&draft;
        }
        CHECK(mu_ranked_setup_packet(&m[player],d,1,b));
        auto before=m[player]; auto invalid=std::array<MuYgU8,5>{b[0],b[1],b[2],b[3],b[4]}; invalid[0]=99;
        CHECK(!mu_ranked_setup_apply(&m[player],player,invalid.data())); CHECK(!std::memcmp(&before,&m[player],sizeof before));
        CHECK(!mu_ranked_setup_apply(&m[player],!player,b));
        CHECK(mu_ranked_setup_apply(&m[player],player,b));
        std::memcpy(reply+2,b+1,4); CHECK(mu_ranked_setup_remote(&m[!player],reply));
        CHECK(m[0].step==m[1].step && m[0].status==m[1].status);
        CHECK(!mu_ranked_setup_apply(&m[player],player,b)); // duplicate cannot advance a new step
      }
      CHECK(m[0].status==MU_SETUP_COMPLETE && m[1].status==MU_SETUP_COMPLETE);
      CHECK(!std::memcmp(&m[0].result,&m[1].result,sizeof m[0].result)); r=m[0].result;
      unsigned w=game==1?!winner:winner;
      for(auto& set:sets) CHECK(mu_ranked_setup_record_game(&set,w,2,r.stage,0)==(game==2?MU_SETUP_SET_OVER:MU_SETUP_NEXT_GAME));
    }
  }
  // Winner switches to Sheik; loser cannot select the equivalent Zelda costume.
  {
    auto a=fresh(), b=fresh(); a.cur_game=b.cur_game=2; a.prev_winner=b.prev_winner=1;
    auto r=initial(); MuRankedSetupModel m[2];
    CHECK(mu_ranked_setup_model_init(&m[0],&p,&a,0,&r));
    CHECK(mu_ranked_setup_model_init(&m[1],&p,&b,1,&r));
    while(m[0].step<2) {
      auto player=mu_ranked_setup_current(&m[0])->player; MuYgU8 wire[5],response[6]={1,0,0,0,0,0};
      CHECK(mu_ranked_setup_packet(&m[player],nullptr,1,wire));
      CHECK(mu_ranked_setup_apply(&m[player],player,wire));
      std::memcpy(response+2,wire+1,4);CHECK(mu_ranked_setup_remote(&m[!player],response));
    }
    for(unsigned turn=0;turn<2;++turn) {
      unsigned player=turn?0:1; MuRankedSetupChoice choice{};
      choice.character=turn?18:19; choice.color=0; MuYgU8 wire[5],response[6]={1,0,0,0,0,0};
      if(turn) {CHECK(!mu_ranked_setup_packet(&m[player],&choice,0,wire));choice.color=1;}
      CHECK(mu_ranked_setup_packet(&m[player],&choice,0,wire));
      CHECK(mu_ranked_setup_apply(&m[player],player,wire));
      std::memcpy(response+2,wire+1,4);CHECK(mu_ranked_setup_remote(&m[!player],response));
    }
    CHECK(!std::memcmp(&m[0].result,&m[1].result,sizeof m[0].result));
    CHECK(m[0].result.fighters[1].character==19 && m[0].result.fighters[0].character==18);
  }
  // Remote skip uses the same deterministic fallback; absent/malformed packets do not mutate.
  auto set=fresh(); auto r=initial(); MuRankedSetupModel m; CHECK(mu_ranked_setup_model_init(&m,&p,&set,1,&r));
  MuYgU8 reply[6]={0}; CHECK(!mu_ranked_setup_remote(&m,reply));
  reply[0]=1; reply[4]=200; CHECK(!mu_ranked_setup_remote(&m,reply));
  reply[1]=1; CHECK(mu_ranked_setup_remote(&m,reply)); CHECK(m.removed[0]==2);
}
static void disconnects() {
  const auto p=yougame::ranked_policy::slippi_v1();
  for(unsigned game=1;game<=2;++game) for(unsigned seat=0;seat<2;++seat) for(unsigned stop=0;stop<6;++stop) {
    auto set=fresh(); set.cur_game=game; if(game>1)set.prev_winner=1;
    auto r=initial(true); MuRankedSetupModel m; CHECK(mu_ranked_setup_model_init(&m,&p,&set,seat,&r));
    for(unsigned i=0;i<stop && mu_ranked_setup_current(&m);++i) automatic(m);
    auto result=m.result; mu_ranked_setup_disconnect(&m); CHECK(m.status==MU_SETUP_DISCONNECTED);
    MuYgU8 b[5]={0},reply[6]={1,1,0,0,0,0}; CHECK(!mu_ranked_setup_current(&m));
    CHECK(!mu_ranked_setup_packet(&m,nullptr,1,b)); CHECK(!mu_ranked_setup_apply(&m,seat,b));
    CHECK(!mu_ranked_setup_remote(&m,reply)); CHECK(mu_ranked_setup_tick(&m)==MU_SETUP_WAIT);
    CHECK(!std::memcmp(&result,&m.result,sizeof result));
    CHECK(mu_ranked_setup_record_game(&set,0,2,r.stage,1)==MU_SETUP_SET_ABORTED); CHECK(set.score[0]==0);
  }
}
static void tiebreak() {
  const auto p=yougame::ranked_policy::slippi_v1(); auto set=fresh(); auto r=initial(); MuRankedSetupModel m;
  for(unsigned tie=1;tie<=5;++tie) {
    CHECK(mu_ranked_setup_record_game(&set,-1,7,r.stage,0)==MU_SETUP_TIEBREAK);
    CHECK(set.cur_game==1 && set.tiebreak_num==tie && set.score[0]==0 && set.last_end_mode==7);
    CHECK(mu_ranked_setup_model_init(&m,&p,&set,0,&r)); CHECK(m.status==MU_SETUP_COMPLETE && m.step_count==0);
    CHECK(!std::memcmp(&r,&m.result,sizeof r)); CHECK(mu_ranked_setup_tick(&m)==MU_SETUP_WAIT);
  }
  CHECK(mu_ranked_setup_record_game(&set,1,2,r.stage,0)==MU_SETUP_NEXT_GAME);
  CHECK(set.cur_game==2 && set.tiebreak_num==0 && set.score[1]==1);
}
// 11-18 field run on two Windows seats over the room relay: Ness (11) colour 0 on both, both idle, so
// every step (both colour steps, all three strikes) was decided by its timer. Each seat runs its own
// model; the owner of a timed-out step sends its automatic packet like a pressed one, and the other
// seat applies it a relay hop later. Both must build the same overwrite (publish_result's 0xBF) and,
// through the setup hash, agree on it. The field run died at step 4 on the slower seat: its setup
// hash wait counted from its lock-in setup (two minutes earlier), so the faster seat's newer hash
// read as "no matching hash after 8000 ms" before the last strike step had even been applied.
struct Seat {
  MuRankedSet set; MuRankedSetupModel m;
  std::deque<std::pair<unsigned,std::array<MuYgU8,5>>> inbox;   // packets from the other seat, by arrival frame
  MuSetupHash hash{}; uint64_t since=0; MuSetupHashWait wait{};   // the gate's state for this seat's setup
  MuSetupHash peer{}; bool peer_received=false; std::deque<std::pair<unsigned,MuSetupHash>> hash_inbox;
  bool built=false;
};
static std::array<MuYgU8,14> overwrite(const MuRankedSetupModel& m) {
  // publish_result's 0xBF payload, game 1: stage, then (present, char, colour) per port.
  std::array<MuYgU8,14> out{}; out[0]=(MuYgU8)(m.result.stage>>8); out[1]=(MuYgU8)m.result.stage;
  for(unsigned i=0;i<2;++i) {out[2+3*i]=1; out[3+3*i]=m.result.fighters[i].character; out[4+3*i]=m.result.fighters[i].color;}
  return out;
}
static MuSetupHash setup_of(unsigned port, const MuRankedSetupResult& r, bool overwritten) {
  // Stand-in for setup_inputs_hash/setup_block_hash: the same function of the same data on both seats.
  uint64_t h=mu_fnv64_u8(MU_FNV64_OFFSET,overwritten); h=mu_fnv64_u16(h,r.stage);
  for(const auto& f:r.fighters) {h=mu_fnv64_u8(h,f.character); h=mu_fnv64_u8(h,f.color);}
  return {(unsigned char)port,h,mu_fnv64_u8(h,0x5A)};
}
static void mirror_idle() {
  const auto p=yougame::ranked_policy::slippi_v1();
  MuRankedSetupResult css{}; css.stage=28; css.fighters[0]={11,0}; css.fighters[1]={11,0};
  for(unsigned relay=1;relay<=30;relay+=29) {   // 1 frame, and a slow relay hop (half a second)
    std::array<Seat,2> seat;
    for(unsigned i=0;i<2;++i) {
      seat[i].set=fresh(); CHECK(mu_ranked_setup_model_init(&seat[i].m,&p,&seat[i].set,i,&css));
      CHECK(seat[i].m.step==0 && seat[i].set.color_ban.active);   // mirror colour: both colour steps run
      // Lock-in: the CSS selections build the first setup on both seats, and they agree.
      seat[i].hash=setup_of(i,css,false); seat[i].since=0;
    }
    for(unsigned i=0;i<2;++i) {seat[i].peer=seat[!i].hash; seat[i].peer_received=true;}
    bool old_clock_would_drop=false;
    for(unsigned frame=1;frame<60u*400;++frame) {
      const uint64_t now=frame*1000ull/60;
      for(unsigned i=0;i<2;++i) {
        Seat& me=seat[i]; Seat& other=seat[!i];
        while(!me.hash_inbox.empty() && me.hash_inbox.front().first<=frame) {
          me.peer=me.hash_inbox.front().second; me.peer_received=true; me.hash_inbox.pop_front();
        }
        // The match-state poll runs first each frame (mu_ranked_setup_frame), then the step fetch.
        const auto d=mu_setup_hash_decide(1,me.peer_received,&me.hash,&me.peer);
        const bool over=mu_setup_hash_wait_over(&me.wait,me.since,me.peer_received,&me.peer,now,8000);
        if(d==MU_SETUP_HASH_WAIT && now-me.since>=8000) old_clock_would_drop=true;
        CHECK(d!=MU_SETUP_HASH_MISMATCH);
        CHECK(!(d==MU_SETUP_HASH_WAIT && over));   // never "no matching hash" while both are here
        const auto* s=mu_ranked_setup_current(&me.m);
        if(s && s->player==i) {
          if(mu_ranked_setup_tick(&me.m)==MU_SETUP_LOCAL_TIMEOUT) {
            std::array<MuYgU8,5> b{}; CHECK(mu_ranked_setup_packet(&me.m,nullptr,1,b.data()));
            CHECK(mu_ranked_setup_apply(&me.m,i,b.data())); other.inbox.push_back({frame+relay,b});
          }
        } else if(s) {
          mu_ranked_setup_tick(&me.m);
          if(!me.inbox.empty() && me.inbox.front().first<=frame) {
            MuYgU8 response[6]={1,0,0,0,0,0}; std::memcpy(response+2,me.inbox.front().second.data()+1,4);
            me.inbox.pop_front(); CHECK(mu_ranked_setup_remote(&me.m,response));
          }
        }
        if(me.m.status==MU_SETUP_COMPLETE && !me.built) {
          // publish_result's overwrite builds the real setup: a new hash, sent once, and a fresh wait.
          me.built=true; me.hash=setup_of(i,me.m.result,true); me.since=now; me.wait=MuSetupHashWait{};
          other.hash_inbox.push_back({frame+relay,me.hash});
        }
      }
      if(seat[0].built && seat[1].built && seat[0].hash_inbox.empty() && seat[1].hash_inbox.empty()) break;
    }
    for(unsigned i=0;i<2;++i) {
      CHECK(seat[i].m.status==MU_SETUP_COMPLETE && seat[i].built);
      CHECK(mu_setup_hash_decide(1,1,&seat[i].hash,&seat[!i].hash)==MU_SETUP_HASH_AGREE);
    }
    CHECK(overwrite(seat[0].m)==overwrite(seat[1].m));
    CHECK(!std::memcmp(&seat[0].m.result,&seat[1].m.result,sizeof seat[0].m.result));
    // The field's final setup: the last starter after strikes, the mirror recoloured 1 and 2.
    CHECK(seat[0].m.result.stage==31);
    CHECK(seat[0].m.result.fighters[0].character==11 && seat[0].m.result.fighters[0].color==1);
    CHECK(seat[0].m.result.fighters[1].character==11 && seat[0].m.result.fighters[1].color==2);
    CHECK(old_clock_would_drop);   // the 122.19 clock (lock-in setup only) dropped this very sequence
  }
}
int main(int argc,char** argv) {
  std::string group=argc>1?argv[1]:"all";
  if(group=="stages"||group=="all") stages();
  if(group=="colors"||group=="all") colors();
  if(group=="timers"||group=="all") timers();
  if(group=="interleave"||group=="all") interleave();
  if(group=="disconnects"||group=="all") disconnects();
  if(group=="tiebreak"||group=="all") tiebreak();
  if(group=="mirror_idle"||group=="all") mirror_idle();
  std::printf("Ranked setup %s: %u checks passed\n",group.c_str(),checks);
}
