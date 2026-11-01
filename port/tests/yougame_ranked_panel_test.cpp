// Production CSS panel and splash score with a fake renderer and cached host IO.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <mu_ranked.h>
#include <mu_rank_badge.h>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
struct GXColor { unsigned char r,g,b,a; };
struct HSD_GObj { void (*user_data_remove_func)(void*) = nullptr; };
struct SisBuffer { unsigned char *data, *end; };
static unsigned char font_slot;
static SisBuffer storage{&font_slot, &font_slot};
struct sislib_UnkAlloc3 { sislib_UnkAlloc3* x0=nullptr; int xA=0; };
static sislib_UnkAlloc3 css_camera;
static sislib_UnkAlloc3* HSD_SisLib_804D797C = &css_camera;
static unsigned char* HSD_SisLib_804D1124[5] = {&font_slot};
struct HSD_Text {
  SisBuffer* alloc_data=&storage;
  unsigned char* sis_buffer=&font_slot;
  HSD_GObj* entity=nullptr;
  int default_kerning;
  float pos_x=0,pos_y=0;
  struct { float x,y; } font_size;
  std::vector<std::string> rows;
  struct Row {float x,y,sx=1,sy=1; GXColor color{};};
  std::vector<Row> layout;
};
struct MuRankBadge { MuYgRank rank; float x,y,scale; bool art_only=false; };
enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED };
enum { MU_SLP_MM_IDLE, MU_SLP_MM_INITIALIZING, MU_SLP_MM_MATCHMAKING,
       MU_SLP_MM_OPPONENT_CONNECTING, MU_SLP_MM_CONNECTION_SUCCESS, MU_SLP_MM_ERROR };
struct MuSlippiMenuState { int mode; } state;
struct MuMatchState { int connection_state; int local_index; } match;
static MuRankedSet set;
static MuYgRanks ranks;
static bool online = true, rank_ok = true, e3_ok = true;
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
static bool practice;
int mu_ranked_practice() { return practice; }
#endif
static int request, fetches, reads, e3_status = 1, badge_count, text_count;
static void (*cleanup)(void*);
static HSD_GObj owner;
static bool mu_slippi_css_online() { return online; }
static MuSlippiMenuState* mu_slippi_state() { return &state; }
static const MuMatchState* mu_slippi_css_msrb() { return &match; }
static int mu_slippi_css_scene_request() { return request; }
const MuRankedSet* mu_ranked_set() { return &set; }
int mu_ranked_read_ranks(MuYgRanks* out) {
  ++reads; *out = rank_ok ? ranks : MuYgRanks{}; return rank_ok;
}
static int mu_slippi_cmd(unsigned cmd, const void*, unsigned, void* out, unsigned size, unsigned* got) {
  if (cmd == 0xE4) { ++fetches; return 0; }
  CHECK(cmd == 0xE3 && size == 16);
  if (!e3_ok) return -1;
  std::memset(out, 0, size); static_cast<unsigned char*>(out)[1] = e3_status;
  *got = size; return 0;
}
static void HSD_SisLib_803A5CC4(HSD_Text* t);
static void text_destroy(void*) {}
static HSD_Text* HSD_SisLib_803A6754(int, int) {
  ++text_count; auto* t = new HSD_Text{};
  static HSD_GObj entity{text_destroy}; t->entity=&entity; return t;
}
static int HSD_SisLib_803A6B98(HSD_Text* t, float x, float y, const char*, const char* s) {
  t->rows.emplace_back(s); t->layout.push_back({x,y}); return int(t->rows.size()-1);
}
static void HSD_SisLib_803A7548(HSD_Text* t, int id, float sx, float sy) {t->layout.at(id).sx=sx;t->layout.at(id).sy=sy;}
static void HSD_SisLib_803A74F0(HSD_Text* t, int id, GXColor* color) { t->layout.at(id).color=*color; }
static void HSD_SisLib_803A70A0(HSD_Text* t, int id, const char*, const char* s) { t->rows.at(id) = s; }
static void HSD_SisLib_803A5CC4(HSD_Text* t) { --text_count; delete t; }
static HSD_GObj* GObj_Create(int, int, int) { return &owner; }
static void GObj_InitUserData(HSD_GObj*, int, void (*fn)(void*), void*) { cleanup = fn; }
MuRankBadge* mu_rank_badge_create(HSD_GObj*, float x, float y, float scale, const MuYgRank* rank) {
  ++badge_count; return new MuRankBadge{*rank,x,y,scale};
}
void mu_rank_badge_art_only(MuRankBadge* badge) { if (badge) badge->art_only=true; }
void mu_rank_badge_set(MuRankBadge* badge, const MuYgRank* rank) { badge->rank = *rank; }
void mu_rank_badge_free(MuRankBadge* badge) { if (badge) { --badge_count; delete badge; } }
#include "../../sourceport/game/shim/mu_ranked_rank.c"

static std::vector<std::string> splash;
static GXColor splash_colors[5];
static int create_subtext(HSD_Text*, const GXColor*, float, float, float, const char*, const char* value) {
  splash.emplace_back(value); return 0;
}
static void splash_score(int local = 0) {
  HSD_Text* t = nullptr;
#include "ranked_splash_score.inc"
}
static MuYgRank rank(const char* label, int tier, int division) {
  MuYgRank out{}; out.known = out.placed = 1; out.tier = tier; out.division = division;
  out.rating = 15347; out.matches = 5; out.placement_total = 5;
  std::strcpy(out.label, label); return out;
}
static void inside(float x,float y) {
  CHECK(std::isfinite(x) && x>=-32 && x<=32);
  CHECK(std::isfinite(y) && y>=-24 && y<=24);
}
struct Box { float left, top, right, bottom; };
static bool intersects(const Box& a, const Box& b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
static Box text_box(float x, float y, float sx, float sy, const std::string& value) {
  // Measured on 2560x1440 CSS screenshot (10-31): "User" at scale .4 is ~8.2 units per
  // character, so the game's font advances ~20.5 units per character at scale 1.
  return {x, y, x + value.size()*20.5f*sx, y + 20*sy};
}
static void clear_of_css(const std::vector<Box>& css) {
  std::vector<Box> boxes;
  for (unsigned i=0; i<LINE_COUNT; ++i) {
    if (panel.text->rows[i].empty()) continue;
    const auto& row=panel.text->layout[i];
    // Convert the production text origin/font size back to CSS subtext units.
    boxes.push_back(text_box((panel.text->pos_x+row.x*panel.text->font_size.x)/.1f,
                            (panel.text->pos_y+row.y*panel.text->font_size.y)/.1f,
                            row.sx*panel.text->font_size.x/.1f,
                            row.sy*panel.text->font_size.y/.1f, panel.text->rows[i]));
  }
  for (unsigned seat=0; seat<2; ++seat) if (auto* b=panel.badges[seat]) {
    const float size=24*b->scale/.1f; // production art is 24 * .105 world units
    const Box art{b->x/.1f,b->y/.1f,b->x/.1f+size,b->y/.1f+size};
    if (!seat) CHECK(art.right < 66); // the right column starts at x 70
    boxes.push_back(art);
    CHECK(std::fabs(size-25.2f)<.001f);
    // CSS intentionally draws art only: the adjacent label includes division.
    CHECK(b->art_only);
  }
  for (unsigned i=0; i<boxes.size(); ++i) {
    const auto& b=boxes[i];
    CHECK(!intersects(b,Box{-300,-250,300,-25})); // portrait grid
    CHECK(!(b.left < -124 && b.bottom > 0)); // P1 card half-plane
    CHECK(b.left >= -320 && b.right <= 320 && b.top >= -240 && b.bottom <= 240);
    for (const auto& other:css) CHECK(!intersects(b,other));
    for (unsigned j=0; j<i; ++j) CHECK(!intersects(b,boxes[j]));
  }
}
static void bounds() {
  CHECK(panel.text->rows.size()==LINE_COUNT);
  for(unsigned i=0;i<LINE_COUNT;++i) {
    const auto& row=panel.text->layout[i];
    float x=panel.text->pos_x+row.x*panel.text->font_size.x;
    float y=panel.text->pos_y+row.y*panel.text->font_size.y;
    inside(x,y);
    inside(x+panel.text->rows[i].size()*24*panel.text->font_size.x*row.sx,
           y+24*panel.text->font_size.y*row.sy);
  }
  for(auto* b:panel.badges)if(b) {
    inside(b->x,b->y);inside(b->x+24*b->scale,b->y+24*b->scale);
  }
  clear_of_css({text_box(-112,20,.4f,.4f,"User"),
                text_box(-112,40,.5f,.5f,std::string(31,'N'))});
}
static void tick() { for (int i=0; i<15; ++i) mu_ranked_rank_frame(); bounds(); }
static void leave() { cleanup(nullptr); CHECK(!panel.text && badge_count == 0 && text_count == 0); }
static void layout_states() {
  const int states[]={MU_SLP_MM_IDLE,MU_SLP_MM_INITIALIZING,MU_SLP_MM_MATCHMAKING,
                     MU_SLP_MM_OPPONENT_CONNECTING,MU_SLP_MM_CONNECTION_SUCCESS,MU_SLP_MM_ERROR};
  // Mirror Ranked text_think's visibility branches in mu_slippi_css.c, including
  // character selection, local readiness and the early P2P-connecting path.
  // Check both first-game and returning-set CSS, with/without practice wording.
  for (int practice_mode=0; practice_mode<2; ++practice_mode) {
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
    practice=practice_mode;
#else
    if (practice_mode) continue;
#endif
    for (int conn:states) for (bool chosen:{false,true}) for (bool ready:{false,true})
      for (bool p2p:{false,true}) for (int game:{1,3}) {
        state.mode=MU_SLP_MODE_RANKED; match.connection_state=conn;
        set.cur_game=game; set.max_games=3; set.score[0]=set.score[1]=game>1;
        ranks.self=rank(practice_mode ? "Platinum IV" : "Challenger",practice_mode ? 4 : 6,practice_mode ? 4 : 0);
        ranks.opponent=practice_mode ? MuYgRank{} : rank("Diamond IV",5,4);
        rank_ok=e3_ok=true; e3_status=1; request=0;
        mu_ranked_rank_enter(); mu_ranked_rank_frame(); bounds();
        const float xs[]={-112,-112,70,70,70,-112,70};
        const float ys[]={65,105,178,218,115,85,198};
        CHECK(panel.text->pos_x==0 && panel.text->pos_y==0);
        for (unsigned i=0; i<LINE_COUNT; ++i) {
          const auto& row=panel.text->layout[i];
          const bool label=i==LOCAL_LABEL || i==OPP_LABEL;
          CHECK(row.x==xs[i] && row.y==ys[i]);
          const float width=panel.text->rows[i].size()*20.5f;
          const float available=i==LOCAL_LABEL ? 148.0f : 156.0f;
          const float label_scale=width>available/.5f ? available/width : .5f;
          CHECK(std::fabs(row.sx-(label?label_scale:.4f))<.0001f && row.sy==row.sx);
          const GXColor expected=label || i==SET_SCORE ? white : gray;
          CHECK(std::memcmp(&row.color,&expected,sizeof expected)==0);
        }
        CHECK(std::fabs(panel.badges[0]->x-4.0f)<.001f);
        CHECK(panel.badges[0]->scale==.105f);
        CHECK(std::fabs(panel.badges[0]->y+12*panel.badges[0]->scale-8.9f)<.001f);
        const bool matched=conn==MU_SLP_MM_OPPONENT_CONNECTING || conn==MU_SLP_MM_CONNECTION_SUCCESS;
        CHECK(bool(panel.badges[1])==matched);
        CHECK(panel.text->rows[OPP_TITLE].empty()==!matched);
        CHECK(panel.text->rows[SET_SCORE].empty()==!(game>1 && (conn==MU_SLP_MM_IDLE || matched)));
        if (matched) {
          CHECK(std::fabs(panel.badges[1]->x-23.0f)<.001f);
          CHECK(std::fabs(panel.badges[1]->y+12*panel.badges[1]->scale-20.2f)<.001f);
        }
        std::vector<Box> css;
        auto add=[&](float x,float y,float scale,const std::string& value) {
          css.push_back(text_box(x,y,scale,scale,value));
        };
        add(-112,20,.4f,"User"); add(-112,40,.5f,std::string(31,'N'));
        add(70,23,.5f,conn==MU_SLP_MM_ERROR ? "Error" : practice_mode ? "Practice - not rated" : "Ranked");
        if (matched) { add(-130,-246,.5f,"Playing:"); add(-50,-246,.5f,std::string(31,'N')); }
        if (conn>MU_SLP_MM_IDLE) add(70,132.5f,.4f,"Hold Z to disconnect");
        if (conn==MU_SLP_MM_CONNECTION_SUCCESS && !p2p && !practice_mode)
          add(70,152.5f,.4f,"Use D-Pad to Chat");
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
        if (conn==MU_SLP_MM_IDLE) add(70,152.5f,.4f,"X: Practice vs CPU");
#endif
        if (conn==MU_SLP_MM_ERROR) {
          for (float y:{52.0f,70.0f,88.0f,106.0f}) add(70,y,.4f,std::string(30,'E'));
        } else {
          auto status=[&](float y,const std::string& value) {
            add(90,y,.4f,value); add(70,y,.45f,"O"); // line + spinner
          };
          status(52,chosen ? "Character selected" : "Select your character");
          if (p2p && conn<MU_SLP_MM_CONNECTION_SUCCESS) status(75,"Connecting to opponent");
          else if (chosen) {
            status(75,ready ? "Locked in" : "Press START to lock in");
            if (ready) status(98,conn==MU_SLP_MM_CONNECTION_SUCCESS ? "Waiting on opponent" :
                                 conn==MU_SLP_MM_OPPONENT_CONNECTING ? "Connecting to opponent" : "Searching for opponent");
          }
        }
        // R1 opponent rows are blank whenever the ranked panel owns them.
        clear_of_css(css); leave();
      }
  }
}
int main() {
  set.max_games = 3; set.cur_game = 1;
  const MuRankedSet initial = set;
  online = false; mu_ranked_rank_enter(); CHECK(!panel.text && reads == 0);
  online = true; state.mode = MU_SLP_MODE_UNRANKED;
  mu_ranked_rank_enter(); CHECK(!panel.text && reads == 0);
  state.mode = MU_SLP_MODE_RANKED;
  mu_ranked_rank_enter(); CHECK(fetches == 0 && badge_count == 1 && text_count == 1);
  CHECK(panel.text->rows[LOCAL_DETAIL] == "Unknown" && !panel.badges[0]->rank.known);
  CHECK(panel.text->rows[OPP_TITLE].empty() && !panel.badges[1]);
  e3_status = 0; tick(); CHECK(panel.text->rows[LOCAL_DETAIL] == "Loading...");
  e3_status = 2; tick(); CHECK(panel.text->rows[LOCAL_DETAIL] == "Rank unavailable");
  e3_status = 1; ranks.self = rank("Gold IV", 3, 4); tick();
  CHECK(std::string(panel.badges[0]->rank.label) == "Gold IV");
  CHECK(panel.text->rows[LOCAL_DETAIL] == "Rating 1534.7");
  // A whole rating drops ".0", the same as the end-of-set panel ("Rating 1008", build 26).
  ranks.self.rating = 10080; tick(); CHECK(panel.text->rows[LOCAL_DETAIL] == "Rating 1008");
  ranks.self.rating = 15347;
  ranks.self.placed = 0; ranks.self.matches = 3; tick();
  CHECK(std::string(panel.badges[0]->rank.label) == "Placements 3 of 5");
  CHECK(panel.text->rows[LOCAL_LABEL] == "Placements 3 of 5");
  ranks.self.placement_total = 0; tick();
  CHECK(std::string(panel.badges[0]->rank.label) == "Placements 3 of 5");
  CHECK(panel.text->rows[LOCAL_LABEL] == "Placements 3 of 5");
  ranks.self = rank("Challenger", 6, 0); e3_status = 0; tick();
  CHECK(std::string(panel.badges[0]->rank.label) == "Challenger");
  CHECK(panel.text->rows[LOCAL_DETAIL] == "Rating 1534.7 - Loading...");
  e3_status = 1; ranks.opponent = rank("Diamond I", 5, 1);
  match.connection_state = MU_SLP_MM_OPPONENT_CONNECTING; mu_ranked_rank_frame();
  CHECK(badge_count == 2 && std::string(panel.badges[1]->rank.label) == "Diamond I");
  match.connection_state = MU_SLP_MM_CONNECTION_SUCCESS;
  ranks.opponent = {}; tick();
  CHECK(!panel.badges[1]->rank.known && panel.text->rows[OPP_DETAIL] == "Unknown");
  rank_ok = false; tick();
  CHECK(panel.text->rows[LOCAL_DETAIL] == "Rank unavailable" && panel.text->rows[OPP_DETAIL] == "Rank unavailable");
  rank_ok = true; e3_ok = false; tick(); CHECK(panel.text->rows[LOCAL_DETAIL] == "Rank unavailable");
  e3_ok = true; ranks.opponent = rank("Bronze II", 1, 2); tick();
  CHECK(panel.text->rows[SET_SCORE].empty());
  set.cur_game = 2; set.score[0] = 1; mu_ranked_rank_frame();
  CHECK(panel.text->rows[SET_SCORE] == "Game 2 - 1:0");
  splash_score(); CHECK(splash == std::vector<std::string>{"Game 2 - 1:0"});
  // Port 1 local (drawn on the left, build 25): its own count comes first on the splash and CSS.
  splash.clear(); splash_score(1); CHECK(splash == std::vector<std::string>{"Game 2 - 0:1"});
  match.local_index = 1; mu_ranked_rank_frame(); CHECK(panel.text->rows[SET_SCORE] == "Game 2 - 0:1");
  match.local_index = 0; mu_ranked_rank_frame(); splash.clear();
  set.cur_game = 3; set.score[1] = 1; mu_ranked_rank_frame();
  CHECK(panel.text->rows[SET_SCORE] == "Game 3 - 1:1");
  bounds(); // both players and the score without the real badge renderer
  request = 1; const auto count = reads; tick(); CHECK(reads == count); request = 0;
  match.connection_state = MU_SLP_MM_MATCHMAKING; mu_ranked_rank_frame();
  CHECK(badge_count == 1 && panel.text->rows[OPP_TITLE].empty() && panel.text->rows[SET_SCORE].empty());
  match.connection_state = MU_SLP_MM_IDLE; mu_ranked_rank_frame();
  CHECK(panel.text->rows[SET_SCORE] == "Game 3 - 1:1");
  const auto after = set; leave(); mu_ranked_rank_enter();
  CHECK(fetches == 1 && !std::memcmp(&after, &set, sizeof set)); leave();
  set = initial; mu_ranked_rank_enter(); CHECK(fetches == 1); leave();
  state.mode = MU_SLP_MODE_UNRANKED; splash.clear(); splash_score(); CHECK(splash.empty());
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
  state.mode = MU_SLP_MODE_RANKED; practice = true;
  match.connection_state = MU_SLP_MM_CONNECTION_SUCCESS;
  ranks.self = rank("Gold II", 3, 2); // the real account standing is unchanged
  ranks.opponent = {}; // CPU has no ladder standing
  mu_ranked_rank_enter();
  CHECK(panel.text->rows[LOCAL_TITLE] == "Practice - not rated");
  CHECK(panel.text->rows[LOCAL_DETAIL] == "Practice rating 1534.7");
  CHECK(panel.text->rows[OPP_TITLE] == "CPU practice rank");
  CHECK(panel.text->rows[OPP_DETAIL] == "Unknown");
  CHECK(std::string(panel.badges[0]->rank.label) == "Gold II");
  CHECK(!panel.badges[1]->rank.known && !panel.badges[1]->rank.rating);
  CHECK(panel.text->rows[OPP_LABEL].empty());
  bounds();
  practice = false; tick();
  CHECK(panel.text->rows[LOCAL_TITLE] == "Your rank");
  CHECK(panel.text->rows[LOCAL_DETAIL] == "Rating 1534.7");
  leave();
#endif
  state.mode=MU_SLP_MODE_RANKED;
  match.connection_state=MU_SLP_MM_CONNECTION_SUCCESS;
  for (const char* label:{"Practice Platinum IV", "Practice Challenger", "Platinum IV"}) {
    ranks.self=ranks.opponent=rank(label,4,4);
    mu_ranked_rank_enter(); bounds(); leave();
  }
  for (unsigned total:{0u,5u,255u}) for (unsigned matches:{0u,5u,65535u}) {
    ranks.self=ranks.opponent=rank("host label",0,0);
    ranks.self.placed=ranks.opponent.placed=0;
    ranks.self.placement_total=ranks.opponent.placement_total=total;
    ranks.self.matches=ranks.opponent.matches=matches;
    mu_ranked_rank_enter(); bounds();
    const unsigned limit=total?total:5;
    const std::string expected="Placements "+std::to_string(std::min(matches,limit))+" of "+std::to_string(limit);
    CHECK(panel.text->rows[LOCAL_LABEL]==expected && panel.text->rows[OPP_LABEL]==expected);
    leave();
  }
  layout_states();
  std::cout << "rank panel: " << checks << " checks passed\n";
}
