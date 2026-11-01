// Execute the actual art badge, panel and reveal against a capturing SIS/GX API.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstring>
#include "ranked_ui_renderer.h"
#include "ranked_ui_badge.inc"
#include "../../sourceport/game/shim/mu_ranked_rank.c"
#include "../../sourceport/game/shim/mu_ranked_reveal.c"
static unsigned update_result_shown;
void mu_ranked_update_result_shown() { ++update_result_shown; }


// CSS camera's 640x480 safe area in world units. SIS positive y goes down;
// GX vertices from the badge renderer have already negated it.
static void inside(float x, float y) {
    CHECK(std::isfinite(x) && x>=-32 && x<=32);
    CHECK(std::isfinite(y) && y>=-24 && y<=24);
}
static void text_inside(HSD_Text* t) {
    CHECK(t); inside(t->pos_x,t->pos_y);
    if(t->bg_color.a)inside(t->pos_x+t->box_size_x*t->font_size.x,
                           t->pos_y+t->box_size_y*t->font_size.y);
    for(const auto& row:t->rows) {
        const float x=t->pos_x+row.x*t->font_size.x*t->x34.x;
        const float y=t->pos_y+row.y*t->font_size.y*t->x34.y;
        inside(x,y);
        // Conservative 24-unit glyph cells, including a full line of label text.
        if (x+row.value.size()*24*t->font_size.x*t->x34.x*row.sx>32)
            std::cerr << "row outside screen: " << row.value << '\n';
        inside(x+row.value.size()*24*t->font_size.x*t->x34.x*row.sx,
               y+24*t->font_size.y*t->x34.y*row.sy);
    }
}
static void badge_inside(MuRankBadge* b) {
    CHECK(b && b->text->render_callback); vertices.clear();
    b->text->render_callback(b->text->entity);
    for(const auto& vertex:vertices)inside(vertex.x,-vertex.y);
    text_inside(b->text);
}
static MuYgRank standing(const char* label, int tier, int order) {
    MuYgRank r{};r.known=r.placed=1;r.tier=tier;r.order=order;r.division=2;
    r.rating=15347;r.matches=r.placement_total=5;std::strcpy(r.label,label);return r;
}
static void placements_with_art() {
    ranks.self=standing("Unplaced",0,0);ranks.self.placed=0;ranks.self.matches=3;
    ranks.opponent=ranks.self;
    mu_ranked_rank_enter();
    set.cur_game=3;set.score[0]=set.score[1]=1;mu_ranked_rank_frame();
    CHECK(panel.text->rows.size()==LINE_COUNT);
    text_inside(panel.text); // every row, including opponent detail and set score
    for(unsigned seat=0;seat<2;++seat) {
        auto* b=panel.badges[seat];CHECK(b && b->text->render_callback==badge_draw);
        CHECK(b->text->rows[b->subtext].value.empty()); // real art omits the label
        CHECK(panel.text->rows[seat?OPP_LABEL:LOCAL_LABEL].value=="Placements 3 of 5");
        CHECK(panel.text->rows[seat?OPP_DETAIL:LOCAL_DETAIL].value=="Rating 1534.7");
        vertices.clear();b->text->render_callback(b->text->entity);
        CHECK(vertices.empty()); // no placement ring: it read as a loading spinner (build 23)
        badge_inside(b);
    }
    ranks.self=standing("Gold II",3,11);refresh();
    vertices.clear();panel.badges[0]->text->render_callback(panel.badges[0]->text->entity);
    CHECK(vertices.size()==4); // a placed rank draws its tier art
    CHECK(panel.text->rows[LOCAL_LABEL].value=="Gold II");
    CHECK(panel.badges[0]->text->rows[panel.badges[0]->subtext].value.empty());
    text_inside(panel.text);badge_inside(panel.badges[0]);badge_inside(panel.badges[1]);
    ranks.self={};refresh();CHECK(panel.text->rows[LOCAL_LABEL].value.empty());
    match.connection_state=MU_SLP_MM_IDLE;refresh();
    CHECK(panel.text->rows[OPP_LABEL].value.empty());
    free_panel(nullptr);CHECK(!badges && text_count==0);
}
// All bounds below use the measured 20.5-unit glyph advance and a conservative
// 24-unit line height. Include hidden labels so their whole animation fits.
static void reveal_point(float x, float y) {
    inside(x,y);
    CHECK(x>=reveal.panel->pos_x-.001f && x<=reveal.panel->pos_x+REVEAL_W*.1f+.001f);
    CHECK(y>=reveal.panel->pos_y-.001f && y<=reveal.panel->pos_y+REVEAL_H*.1f+.001f);
    CHECK(y<=0 || x>=-12.4f); // P1 card region: CSS x < -124 for y > 0
}
static void reveal_text_inside(HSD_Text* t) {
    reveal_point(t->pos_x,t->pos_y);
    reveal_point(t->pos_x+t->box_size_x*.1f,t->pos_y+t->box_size_y*.1f);
    for(const auto& row:t->rows) {
        const float x=t->pos_x+row.x*.1f*t->x34.x;
        const float y=t->pos_y+row.y*.1f*t->x34.y;
        reveal_point(x,y);
        reveal_point(x+row.value.size()*20.5f*.1f*t->x34.x*row.sx,
                     y+24*.1f*t->x34.y*row.sy);
    }
}
static void reveal_bounds() {
    CHECK(reveal.panel->bg_color.a==255);
    reveal_text_inside(reveal.panel);
    reveal_text_inside(reveal.old_label);reveal_text_inside(reveal.new_label);
    const bool old_visible=!reveal.old_label->hidden && reveal.old_label->rows[0].color.a;
    const bool new_visible=!reveal.new_label->hidden && reveal.new_label->rows[0].color.a;
    CHECK(!(old_visible && new_visible));
    CHECK(reveal.panel->rows.size()==2); // no extra summary/debug row
    for(auto* badge:{reveal.old_badge,reveal.new_badge}) if(badge) {
        CHECK(badge->art_only && badge->text->rows[badge->subtext].value.empty());
        vertices.clear();badge->text->render_callback(badge->text->entity);
        for(const auto& v:vertices)reveal_point(v.x,-v.y);
    }
}
static void reveal_layout() {
    const auto gold=standing("Gold II",3,11), platinum=standing("Platinum IV",4,13);
    auto placing=standing("Placements 0/5",0,0);placing.placed=0;placing.matches=0;
    auto later=placing;later.matches=3;std::strcpy(later.label,"Placements 3/5");
    const MuYgRank snapshots[][2]={{placing,later},{later,gold},{gold,platinum},{platinum,gold},
        {gold,standing("Gold I",3,12)},{gold,gold},
        {gold,standing("Challenger",6,24)},
        {standing("12345678901234567890123",5,20),gold}};
    for(const auto& pair:snapshots)for(int reduced:{0,1}) {
        MuYgResult r{};r.status=MU_YG_RESULT_RECORDED;r.before=15347;r.after=15497;
        r.rank_before=pair[0];r.rank_after=pair[1];reveal_accept(&reveal.model,&r);
        reveal.model.reduced=reduced;
        for(unsigned f=0;f<=REVEAL_GLOW_END;++f) {
            reveal.model.frame=f;reveal_draw();
            reveal_bounds();
            const auto pose=reveal_pose(&reveal.model);
            CHECK(reveal.old_label->hidden == (pose.old_alpha<=0));
            CHECK(reveal.new_label->hidden == (pose.new_alpha<=0));
            if (!pair[0].placed) CHECK(reveal.old_label->rows[0].value=="Placements "+std::to_string(pair[0].matches)+" of 5");
            if (!pair[1].placed) {
                CHECK(reveal.new_label->rows[0].value=="Placements 3 of 5");
                CHECK(reveal.panel->rows[1].value=="Rating 1549.7  \x81\x7B" "15" || f<99);
                // The SIS encoder drops ASCII '+' and '/' (build 23 showed "Rating 1038   8").
                CHECK(reveal.panel->rows[1].value.find_first_of("+/")==std::string::npos);
            }
            badge_inside(reveal.old_badge);badge_inside(reveal.new_badge);
        }
        reveal_close();CHECK(!badges && text_count==0);
    }
    // Maximum signed wire magnitudes still fit the single rating/delta line.
    for(int direction:{-1,1}) {
        MuYgResult r{};r.status=MU_YG_RESULT_RECORDED;
        r.rank_before=gold;r.rank_after=platinum;
        r.before=direction<0 ? INT32_MAX : INT32_MIN;
        r.after=direction<0 ? INT32_MIN : INT32_MAX;
        reveal_accept(&reveal.model,&r);
        for(unsigned f=0;f<=REVEAL_GLOW_END;++f) {
            reveal.model.frame=f;reveal_draw();reveal_bounds();
        }
        reveal_close();CHECK(!badges && text_count==0);
    }
    for(int status:{MU_YG_RESULT_RECORDING,MU_YG_RESULT_UNRATED,MU_YG_RESULT_ERROR}) {
        MuYgResult r{};r.status=status;r.reason=MU_YG_REASON_VOID;
        reveal_accept(&reveal.model,&r);reveal_draw();reveal_bounds();
        CHECK(reveal.old_label->hidden && reveal.new_label->hidden);
        CHECK(!reveal.old_badge && !reveal.new_badge);reveal_close();
    }
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
    practice_mode=true;
    for(int reduced:{0,1})for(int winner:{0,1})for(int loser:{0,1}) {
        set.score[winner]=2;set.score[!winner]=loser;
        // Even an old/corrupt recorded fixture cannot leak ratings in practice.
        MuYgResult r{};r.status=MU_YG_RESULT_RECORDED;
        r.rank_before=gold;r.rank_after=platinum;r.before=15000;r.after=15250;
        reveal_accept(&reveal.model,&r);reveal.model.reduced=reduced;
        for(unsigned f=0;f<=REVEAL_GLOW_END;++f) {
            reveal.model.frame=f;reveal_draw();reveal_bounds();
            CHECK(reveal.old_label->hidden && reveal.new_label->hidden);
            CHECK(!reveal.old_badge && !reveal.new_badge);
            CHECK(reveal.panel->rows[0].value=="Practice - not rated");
            CHECK(reveal.panel->rows[1].value==std::string(winner ? "CPU won 2-" : "You won 2-")+std::to_string(loser));
            CHECK(reveal.panel->rows[1].value.find("Rating")==std::string::npos);
        }
        reveal_close();CHECK(!badges && text_count==0);
    }
    practice_mode=false;
#endif
}
static void unavailable_text_retries() {
    auto rank=standing("Gold II",3,11);
    match.connection_state=MU_SLP_MM_IDLE;
    const auto font=HSD_SisLib_804D1124[0];
    HSD_SisLib_804D1124[0]=nullptr;
    CHECK(!mu_rank_badge_create(nullptr,0,0,1,&rank));
    mu_ranked_rank_enter(); CHECK(!panel.text && !badges && text_count==0);
    mu_ranked_rank_frame(); CHECK(!panel.text);
    HSD_SisLib_804D1124[0]=font;
    HSD_SisLib_804D797C=nullptr;
    CHECK(!mu_rank_badge_create(nullptr,0,0,1,&rank));
    mu_ranked_rank_frame(); CHECK(!panel.text);
    HSD_SisLib_804D797C=&css_camera; css_camera.xA=1;
    CHECK(!mu_rank_badge_create(nullptr,0,0,1,&rank));
    css_camera.xA=0;
    for (int failure=0;failure<3;++failure) {
        fail_text=failure==0; fail_storage=failure==1; fail_entity=failure==2;
        CHECK(!mu_rank_badge_create(nullptr,0,0,1,&rank));
        mu_ranked_rank_frame(); CHECK(!panel.text && !badges && text_count==0);
    }
    fail_text=fail_storage=fail_entity=false;
    mu_ranked_rank_frame(); CHECK(panel.text && panel.badges[0]);
    mu_rank_badge_free(panel.badges[0]); panel.badges[0]=nullptr;
    fail_storage=true; refresh(); CHECK(!panel.badges[0] && text_count==1);
    fail_storage=false;
    for(int frame=0;frame<15;++frame) mu_ranked_rank_frame();
    CHECK(panel.badges[0] && panel.badges[0]->text->alloc_data);
    free_panel(nullptr); CHECK(!badges && text_count==0);
    // The result reveal used the same static constructor and must also retry safely.
    fail_storage=true; CHECK(!reveal_text(175,298,.48f) && text_count==0);
    fail_storage=false;
    match.connection_state=MU_SLP_MM_CONNECTION_SUCCESS;
}

static void update_after_reveal() {
    ranks.result={};ranks.result.status=MU_YG_RESULT_UNRATED;
    menu.mode=MU_SLP_MODE_RANKED;
    mu_ranked_reveal_enter();
    CHECK(reveal.panel && update_result_shown==0);
    for(unsigned f=1;f<REVEAL_GLOW_END;++f) {
        mu_ranked_reveal_frame();
        CHECK(update_result_shown==0);
    }
    mu_ranked_reveal_frame(); CHECK(update_result_shown==1);
    // Then the CSS gets its own rank panel and "Press START to search" back, and stays so.
    for(unsigned f=REVEAL_GLOW_END+1;f<REVEAL_HOLD_END;++f) { mu_ranked_reveal_frame(); CHECK(reveal.panel); }
    mu_ranked_reveal_frame(); CHECK(!reveal.panel && !reveal.old_badge && !reveal.new_badge);
    for(int f=0;f<600;++f) mu_ranked_reveal_frame();
    CHECK(!reveal.panel && reveal.model.active && text_count==0);
    // Failed polls and their recovery never bring a finished result back over the CSS.
    fail_ranks=true;
    for(int f=0;f<30;++f) { mu_ranked_reveal_frame(); CHECK(!reveal.panel); }
    CHECK(reveal.model.result.status==MU_YG_RESULT_UNRATED);
    fail_ranks=false;
    for(int f=0;f<30;++f) { mu_ranked_reveal_frame(); CHECK(!reveal.panel); }
    // A room still recording keeps its line up.
    ranks.result.status=MU_YG_RESULT_RECORDING;
    for(int f=0;f<600;++f) mu_ranked_reveal_frame();
    CHECK(reveal.panel && reveal.panel->rows[1].value=="Waiting for the room's result");
    reveal_close();
}

// The winner's lines when the opponent left (CTO 11-01): only for a forfeit this seat won.
static void forfeit_text() {
    char headline[64], line[80], rating[48];
    MuYgResult r{}; r.status=MU_YG_RESULT_RECORDED; r.reason=MU_YG_REASON_FORFEIT_WON;
    r.before=10920; r.after=11110;
    r.rank_before=standing("Silver I",2,8); r.rank_before.division=1;
    r.rank_after=standing("Gold IV",3,9); r.rank_after.division=4;
    CHECK(mu_rank_text_forfeit(headline,line,rating,&r,1,0));
    CHECK(std::string(headline)=="Opponent left - you win the set 1:0");
    CHECK(std::string(line)=="Promoted from Silver I to Gold IV");
    CHECK(std::string(rating)=="Rating 1111  \x81\x7B" "19");
    // No ASCII '+', '/', '>' or middle dot: the SIS encoder would eat them.
    for (const char* s : {headline,line,rating}) CHECK(!std::strpbrk(s,"+/>()!?"));
    r.rank_before=r.rank_after; r.before=11020; CHECK(mu_rank_text_forfeit(headline,line,rating,&r,2,1));
    CHECK(std::string(line)=="Gold IV" && std::string(rating)=="Rating 1111  \x81\x7B" "9");
    CHECK(std::string(headline)=="Opponent left - you win the set 2:1");
    r.rank_before.placed=0; r.rank_before.matches=4; CHECK(mu_rank_text_forfeit(headline,line,rating,&r,0,0));
    CHECK(std::string(line)=="Placed in Gold IV" && std::string(headline)=="Opponent left - you win the set");
    r.rank_after=r.rank_before; r.rank_after.matches=5; r.rank_after.placed=0;
    CHECK(mu_rank_text_forfeit(headline,line,rating,&r,1,0) && std::string(line)=="Placements 5 of 5");
    // The longest line fits the stage panel (x 58..583 of 640) at .45, centered on 320.
    r.rank_before=standing("Platinum IV",4,16); r.rank_after=standing("Diamond III",5,18);
    CHECK(mu_rank_text_forfeit(headline,line,rating,&r,1,0));
    CHECK(std::strlen(line)*20.5f*.45f<=2*(320-58) && std::strlen(headline)*20.5f*.55f<=2*(320-58));
    for (unsigned reason : {MU_YG_REASON_NONE, MU_YG_REASON_FORFEIT_LOST}) {
        r.reason=reason; CHECK(!mu_rank_text_forfeit(headline,line,rating,&r,1,0));
    }
    r.reason=MU_YG_REASON_FORFEIT_WON; r.status=MU_YG_RESULT_RECORDING;
    CHECK(!mu_rank_text_forfeit(headline,line,rating,&r,1,0));
    // A placed label never carries the '/' the encoder would eat.
    MuYgRank odd=standing("Gold IV/Test",3,9); char label[32];
    mu_rank_text_label(label,&odd); CHECK(std::string(label)=="Gold IV-Test");
}

// The setup screen draws the division numeral (build 31: the right badge's fell off screen).
static void setup_numerals() {
    MuYgRank gold=standing("Gold IV",3,9), bronze=standing("Bronze I",1,4);
    gold.division=4; bronze.division=1;
    for(int side=0;side<2;++side) {
        auto* b=mu_rank_badge_create(nullptr,side ? 584 : 28,52,1.4f,side ? &bronze : &gold);
        CHECK(b && !b->art_only && b->text->default_alignment==1);
        const auto& row=b->text->rows[b->subtext];
        CHECK(row.value==(side ? "I" : "IV"));
        const float center=b->text->pos_x+row.x, half=row.value.size()*20.5f*b->text->x34.x/2;
        CHECK(std::fabs(center-(b->x+12*b->size))<.01f); // centered under the art
        CHECK(center-half>=0 && center+half<=640);       // on screen on both sides
        CHECK(b->text->pos_y+row.y>b->y+12*b->size);     // below the art's middle
        mu_rank_badge_free(b);
    }
}

int main() {
    setup_numerals();forfeit_text();unavailable_text_retries();placements_with_art();reveal_layout();update_after_reveal();
    CHECK(text_count==0 && !badges);
    std::cout<<"ranked UI with real badges: "<<checks<<" checks passed\n";
}
