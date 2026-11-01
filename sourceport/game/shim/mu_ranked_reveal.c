/* YouGame CSS-return rank reveal. GPL-3.0-or-later.
 * Presentation only: the room's result is the only rating authority. No E4,
 * matchmaking, scene changes, inputs or writes to the simulation/set state.
 * The portable model is also compiled directly by the host regression test. */
#include <mu_ranked_wire.h>
#include <stdio.h>
#include <string.h>
#ifndef MU_RANKED_REVEAL_TEST
/* The game's MSL stdio.h has no snprintf; the host CRT does (as in mu_css_state.c). */
int snprintf(char* dst, __SIZE_TYPE__ size, const char* format, ...);
#endif

enum {
    REVEAL_OLD_START = 48, REVEAL_OLD_END = 72, /* .8s + .4s */
    REVEAL_NEW_START = 66, REVEAL_NEW_END = 102, /* 1.1s + .6s */
    REVEAL_POP_END = 114, REVEAL_GLOW_END = 126, /* .8s / 1s */
    REVEAL_RATING_START = 15, REVEAL_RATING_END = 99, /* .25s + 1.4s */
    /* Build 29 (11-01): held for good, the panel hid the CSS's own rank panel and
     * "Press START to search". A final result now gives the CSS back 3s after the glow. */
    REVEAL_HOLD_END = REVEAL_GLOW_END + 180
};
typedef struct MuReveal {
    MuYgResult result;
    unsigned frame;
    int active, reduced, logged;
} MuReveal;
typedef struct MuRevealPose {
    float old_y, old_alpha, new_y, new_alpha, scale, glow;
    MuYgI32 rating;
    int direction, changed, tier_up;
} MuRevealPose;

static float reveal_progress(unsigned frame, unsigned first, unsigned last)
{
    if (frame <= first) return 0;
    if (frame >= last) return 1;
    return (float)(frame - first) / (float)(last - first);
}

static int reveal_direction(const MuYgResult* r)
{
    return (r->rank_after.order > r->rank_before.order) -
           (r->rank_after.order < r->rank_before.order);
}

static const char* reveal_note(const MuYgResult* r)
{
    if (r->status == MU_YG_RESULT_RECORDING) return "Result being recorded";
    if (r->status == MU_YG_RESULT_UNRATED) return "Unrated result";
    if (r->status == MU_YG_RESULT_ERROR) return "Result unavailable";
    if (!r->rank_after.placed) return "Placement progress";
    if (!r->rank_before.placed) return "Rank revealed";
    if (reveal_direction(r) < 0) return "Demoted to";
    if (reveal_direction(r) > 0)
        return r->rank_before.tier == r->rank_after.tier ? "Division promotion" : "Promoted to";
    return "Ranked result";
}

static const char* reveal_reason(unsigned reason)
{
    switch (reason) {
    case MU_YG_REASON_PACK: return "Pack is not eligible for rating";
    case MU_YG_REASON_VOID: return "Set was voided";
    case MU_YG_REASON_OTHER: return "Room marked this set unrated";
    case MU_YG_REASON_MALFORMED: return "Room result could not be read";
    case MU_YG_REASON_UNAVAILABLE: return "Room result is unavailable";
    default: return "No rated result supplied by the room";
    }
}

static int reveal_same_rank(const MuYgRank* a, const MuYgRank* b)
{
    return a->known == b->known && a->placed == b->placed && a->tier == b->tier &&
           a->division == b->division && a->order == b->order && a->rating == b->rating &&
           a->matches == b->matches && a->placement_total == b->placement_total &&
           memcmp(a->label, b->label, sizeof(a->label)) == 0;
}

/* Repeated polls never restart an animation. No memcmp of padded C structs. */
static void reveal_accept(MuReveal* s, const MuYgResult* input)
{
    MuYgResult r = *input;
    if (r.status > MU_YG_RESULT_ERROR || (r.status == MU_YG_RESULT_RECORDED &&
        (!r.rank_before.known || !r.rank_after.known))) {
        memset(&r, 0, sizeof r);
        r.status = MU_YG_RESULT_ERROR; r.reason = MU_YG_REASON_MALFORMED;
    }
    if (r.status != MU_YG_RESULT_RECORDED) {
        /* Non-authoritative numbers and rank labels can never leak into UI. */
        r.before = r.after = 0;
        memset(&r.rank_before, 0, sizeof r.rank_before);
        memset(&r.rank_after, 0, sizeof r.rank_after);
    }
    if (s->result.status != r.status || s->result.reason != r.reason ||
        s->result.before != r.before || s->result.after != r.after ||
        !reveal_same_rank(&s->result.rank_before, &r.rank_before) ||
        !reveal_same_rank(&s->result.rank_after, &r.rank_after)) {
        s->result = r; s->frame = 0; s->logged = 0;
    }
    s->active = r.status != MU_YG_RESULT_NONE;
}

/* Tenths as the panel shows them: "872", "1534.7", "-25" (mu_rank_number, whose header needs the
 * game's SIS headers). */
static void reveal_tenths(char out[24], long long tenths)
{
    unsigned long long v = tenths < 0 ? 0ULL - (unsigned long long)tenths : (unsigned long long)tenths;
    if (v % 10) snprintf(out, 24, "%s%llu.%u", tenths < 0 ? "-" : "", v/10, (unsigned)(v%10));
    else snprintf(out, 24, "%s%llu", tenths < 0 ? "-" : "", v/10);
}

/* One plain log line the first time the panel shows a rated result: ranks, ratings and the
 * change, never a name or id. Zero when there is nothing new to log. */
static int reveal_shown_line(MuReveal* s, char out[160])
{
    const MuYgResult* r = &s->result;
    char before[24], after[24], delta[24];
    long long d = (long long)r->after - r->before;
    if (!s->active || r->status != MU_YG_RESULT_RECORDED || s->logged) return 0;
    s->logged = 1;
    reveal_tenths(before, r->before); reveal_tenths(after, r->after); reveal_tenths(delta, d);
    snprintf(out, 160, "yougame: ranked reveal shown: %.24s %s -> %.24s %s (%s%s)\n",
             r->rank_before.label, before, r->rank_after.label, after, d > 0 ? "+" : "", delta);
    return 1;
}

static MuRevealPose reveal_pose(const MuReveal* s)
{
    MuRevealPose p;
    const MuYgResult* r = &s->result;
    float old_t, new_t, pop_t, glow_t, rating_t;
    memset(&p, 0, sizeof p); p.scale = 1;
    if (!s->active || r->status != MU_YG_RESULT_RECORDED) return p;
    p.direction = reveal_direction(r);
    p.changed = r->rank_before.placed != r->rank_after.placed ||
                r->rank_before.order != r->rank_after.order ||
                memcmp(r->rank_before.label, r->rank_after.label, 24) != 0;
    p.tier_up = r->rank_after.placed && p.direction > 0 &&
                r->rank_before.tier != r->rank_after.tier;
    old_t = reveal_progress(s->frame, REVEAL_OLD_START, REVEAL_OLD_END);
    new_t = reveal_progress(s->frame, REVEAL_NEW_START, REVEAL_NEW_END);
    pop_t = reveal_progress(s->frame, REVEAL_NEW_START, REVEAL_POP_END);
    glow_t = reveal_progress(s->frame, REVEAL_NEW_START, REVEAL_GLOW_END);
    rating_t = reveal_progress(s->frame, REVEAL_RATING_START, REVEAL_RATING_END);
    if (s->reduced) { old_t = new_t = pop_t = glow_t = rating_t = 1; }
    /* Rating ease-out follows sdk.js animateRating; double avoids i32 overflow. */
    {
        double t = rating_t, u = 1-t;
        double rating = r->before + ((double)r->after-r->before)*(1-u*u*u);
        p.rating = rating_t >= 1 ? r->after : (MuYgI32)rating;
    }
    p.old_alpha = p.changed ? 1-old_t : 0;
    p.old_y = -18*old_t;
    p.new_alpha = p.changed ? new_t : 1;
    p.new_y = p.changed && !p.tier_up ? (p.direction < 0 ? -22 : 22)*(1-new_t) : 0;
    if (p.tier_up) {
        p.new_alpha = pop_t < .65f ? pop_t/.65f : 1;
        p.scale = pop_t < .65f ? .65f + (.47f*pop_t/.65f) : 1.12f - (.12f*(pop_t-.65f)/.35f);
        p.glow = glow_t < .5f ? glow_t*2 : (1-glow_t)*2;
    }
    /* The incoming phase keeps its existing timing, but never crossfades two
     * words/badges in the same slot. SIS subtext alpha alone is insufficient. */
    if (p.new_alpha > 0) p.old_alpha = 0;
    return p;
}

static void reveal_tick(MuReveal* s)
{
    /* No wrapping/replaying if the CSS stays open for hours. */
    if (s->active && s->frame < REVEAL_HOLD_END) ++s->frame;
}

/* "Waiting for the room's result" stays until the room answers; the answer restarts the frame. */
static int reveal_done(const MuReveal* s)
{
    return s->active && s->result.status != MU_YG_RESULT_RECORDING && s->frame >= REVEAL_HOLD_END;
}

#ifndef MU_RANKED_REVEAL_TEST
#include <mu_ranked_setup_online.h>
#include <mu_native.h>
#include <mu_ranked.h>
#include <mu_rank_badge.h>
#include "mu_rank_text.h"
#include <dolphin/os.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <melee/lb/lbaudio_ax.h>
#include <stdio.h>

/* Private renderer extension: the public badge API used by A/B stays intact. */
void mu_rank_badge_pose(MuRankBadge*, float y, float scale, float alpha, float glow);
int mu_rank_badge_reduced_motion(void);
void mu_rank_badge_art_only(MuRankBadge*);

static struct {
    MuReveal model;
    HSD_Text *panel, *old_label, *new_label;
    MuRankBadge *old_badge, *new_badge;
} reveal;

/* CSS subtext coordinates: origin (0,0), y down, font size .1.
 * x < -124 below y=0 belongs to the P1 card. Leave a four-unit gap.
 * Keep object scale at 1 so subtext offsets and the background share units.
 * Build 23 on Windows (10-31): at REVEAL_Y 8 the background's top edge cut the name plates of
 * the bottom portrait row and its bottom edge sat just above "X: Practice vs CPU" (CSS y 152.5).
 * The drawn background starts about 20 units above pos_y, so y 18 clears the grid and height 146
 * keeps the bottom edge where it was. */
enum { REVEAL_X = -120, REVEAL_Y = 18, REVEAL_W = 430, REVEAL_H = 146,
       REVEAL_LABEL_X = -42, REVEAL_LABEL_Y = 78, REVEAL_BADGE_Y = 66, REVEAL_RATING_Y = 104 };
#define REVEAL_PLUS MU_RANK_PLUS /* mu_rank_text.h: the SIS encoder has no ASCII '+' */
static float reveal_x(float x) { return x * .1f; }
static float reveal_y(float y) { return y * .1f; }

static HSD_Text* reveal_text(float x, float y, float scale)
{
    HSD_Text* t = mu_rank_text_create();
    if (!t) return NULL;
    t->pos_x = reveal_x(x); t->pos_y = reveal_y(y);
    t->box_size_x = t->box_size_y = 20;
    t->default_kerning = 1;
    t->font_size.x = t->font_size.y = .1f;
    t->x34.x = t->x34.y = 1;
    HSD_SisLib_803A6B98(t, 0, 0, "%s", "");
    HSD_SisLib_803A7548(t, 0, scale, scale);
    return t;
}

/* SIS entities die on CSS exit. Clear static text pointers in their destructor. */
static void reveal_text_destroy(void* data)
{
    if (data == reveal.panel) reveal.panel = NULL;
    if (data == reveal.old_label) reveal.old_label = NULL;
    if (data == reveal.new_label) reveal.new_label = NULL;
    HSD_SisLib_803A5A2C(data);
}

static void reveal_close(void)
{
    if (reveal.old_badge) mu_rank_badge_free(reveal.old_badge);
    if (reveal.new_badge) mu_rank_badge_free(reveal.new_badge);
    reveal.old_badge = reveal.new_badge = NULL;
    if (reveal.old_label) HSD_SisLib_803A5CC4(reveal.old_label);
    if (reveal.new_label) HSD_SisLib_803A5CC4(reveal.new_label);
    if (reveal.panel) HSD_SisLib_803A5CC4(reveal.panel);
    reveal.panel = reveal.old_label = reveal.new_label = NULL;
}

static void reveal_label(HSD_Text* t, const MuYgRank* rank, float offset, float scale, float alpha)
{
    unsigned c = mu_rank_text_rgb(rank->tier);
    GXColor color = {c >> 16, c >> 8, c, (unsigned char)(alpha*255)};
    char label[32];
    if (!t) return;
    mu_rank_text_label(label, rank);
    /* No badge during placements: the label takes the badge's place instead of leaving a hole. */
    t->pos_x = reveal_x(rank->known && rank->placed ? REVEAL_LABEL_X : REVEAL_X + 12);
    t->pos_y = reveal_y(REVEAL_LABEL_Y + offset);
    /* Fit the longest wire label, reserving room for the 1.12 reveal pop. */
    {
        float width = (float)strlen(label) * 20.5f;
        float fit = width > 0 ? 340.0f / (width * 1.12f) : .5f;
        if (fit > .5f) fit = .5f;
        HSD_SisLib_803A7548(t, 0, fit*scale, fit*scale);
    }
    t->hidden = alpha <= 0;
    HSD_SisLib_803A70A0(t, 0, "%s", label);
    HSD_SisLib_803A74F0(t, 0, &color);
}

static void reveal_draw(void)
{
    MuReveal* s = &reveal.model;
    MuYgResult* r = &s->result;
    MuRevealPose p = reveal_pose(s);
    GXColor chip = {0xb5, 0xc0, 0xcd, 0xff};
    char d[24], current[24], line[128];
    int practice = mu_ranked_practice();
    int recorded = !practice && r->status == MU_YG_RESULT_RECORDED;
    if (!reveal.panel) {
        reveal.panel = reveal_text(REVEAL_X, REVEAL_Y, .42f);
        reveal.old_label = reveal_text(REVEAL_LABEL_X, REVEAL_LABEL_Y, .5f);
        reveal.new_label = reveal_text(REVEAL_LABEL_X, REVEAL_LABEL_Y, .5f);
        if (!reveal.panel || !reveal.old_label || !reveal.new_label) { reveal_close(); return; }
        reveal.panel->entity->user_data_remove_func = reveal_text_destroy;
        reveal.old_label->entity->user_data_remove_func = reveal_text_destroy;
        reveal.new_label->entity->user_data_remove_func = reveal_text_destroy;
        reveal.panel->bg_color = (GXColor){0x0f, 0x17, 0x2a, 0xff};
        reveal.panel->box_size_x = REVEAL_W; reveal.panel->box_size_y = REVEAL_H;
        /* Coordinates are independent of the per-row glyph scales. */
        HSD_SisLib_803A746C(reveal.panel, 0, 12, 10);
        HSD_SisLib_803A6B98(reveal.panel, 12, REVEAL_RATING_Y, "%s", "");
        HSD_SisLib_803A7548(reveal.panel, 1, .38f, .38f);
    }
    HSD_SisLib_803A70A0(reveal.panel, 0, "%s", practice ? "Practice - not rated" : "Ranked");
    reveal.old_label->hidden = reveal.new_label->hidden = !recorded;
    if (!recorded) {
        if (reveal.old_badge) mu_rank_badge_free(reveal.old_badge);
        if (reveal.new_badge) mu_rank_badge_free(reveal.new_badge);
        reveal.old_badge = reveal.new_badge = NULL;
        if (practice) {
            const MuRankedSet* set = mu_ranked_set();
            if (set->score[0] > set->score[1])
                sprintf(line, "You won %u-%u", (unsigned)set->score[0], (unsigned)set->score[1]);
            else if (set->score[1] > set->score[0])
                sprintf(line, "CPU won %u-%u", (unsigned)set->score[1], (unsigned)set->score[0]);
            else sprintf(line, "Set tied %u-%u", (unsigned)set->score[0], (unsigned)set->score[1]);
        } else {
            sprintf(line, "%s", r->status == MU_YG_RESULT_RECORDING ?
                    "Waiting for the room's result" : reveal_reason(r->reason));
        }
        HSD_SisLib_803A70A0(reveal.panel, 1, "%s", line);
        return;
    }
    if (!reveal.old_badge) reveal.old_badge = mu_rank_badge_create(NULL, reveal_x(-108), reveal_y(REVEAL_BADGE_Y), .14f, &r->rank_before);
    if (!reveal.new_badge) reveal.new_badge = mu_rank_badge_create(NULL, reveal_x(-108), reveal_y(REVEAL_BADGE_Y), .14f, &r->rank_after);
    mu_rank_badge_art_only(reveal.old_badge);
    mu_rank_badge_art_only(reveal.new_badge);
    mu_rank_badge_set(reveal.old_badge, &r->rank_before);
    mu_rank_badge_set(reveal.new_badge, &r->rank_after);
    mu_rank_badge_pose(reveal.old_badge, p.old_y * .1f, 1, p.old_alpha, 0);
    mu_rank_badge_pose(reveal.new_badge, p.new_y * .1f, p.scale, p.new_alpha, p.glow);
    reveal_label(reveal.old_label, &r->rank_before, p.old_y, 1, p.old_alpha);
    reveal_label(reveal.new_label, &r->rank_after, p.new_y, p.scale, p.new_alpha);
    mu_rank_number(current, p.rating);
    mu_rank_number(d, (long long)r->after-r->before);
    if (r->after > r->before) chip = (GXColor){0x86,0xef,0xac,0xff};
    if (r->after < r->before) chip = (GXColor){0xfd,0xa4,0xaf,0xff};
    HSD_SisLib_803A70A0(reveal.panel, 1, "Rating %s  %s%s", current, r->after > r->before ? REVEAL_PLUS : "", d);
    HSD_SisLib_803A74F0(reveal.panel, 1, &chip);
}

void mu_ranked_reveal_enter(void)
{
    /* The scene allocator has already destroyed entities from the prior CSS. */
    memset(&reveal, 0, sizeof reveal);
    reveal.model.reduced = mu_rank_badge_reduced_motion();
    mu_ranked_reveal_frame();
}

void mu_ranked_reveal_frame(void)
{
    MuYgRanks ranks;
    MuYgResult error;
    MuReveal* s = &reveal.model;
    unsigned previous;
    if (!mu_slippi_css_online() || mu_slippi_state()->mode != MU_SLP_MODE_RANKED ||
        !mu_slippi_ranked_available()) {
        reveal_close(); memset(s, 0, sizeof *s); return;
    }
    if (mu_slippi_css_scene_request()) { reveal_close(); return; }
    s->reduced = mu_rank_badge_reduced_motion();
    if (mu_ranked_read_ranks(&ranks)) reveal_accept(s, &ranks.result);
    else {
        memset(&error, 0, sizeof error); error.status = MU_YG_RESULT_ERROR;
        error.reason = MU_YG_REASON_UNAVAILABLE;
        /* A failed poll cannot imply that a set exists on an idle CSS, nor reopen a result
         * that has already had its turn (the panel would flicker back over the CSS). */
        if (s->active && s->result.status != MU_YG_RESULT_RECORDED && !reveal_done(s))
            reveal_accept(s, &error);
    }
    if (!s->active || reveal_done(s)) { reveal_close(); return; }
    previous = s->frame;
    reveal_draw();
    {
        char line[160];
        if (reveal.panel && !mu_ranked_practice() && reveal_shown_line(s, line)) OSReport("%s", line);
    }
    reveal_tick(s);
    // Allow the result to finish drawing/animating before handing the game back.
    if (reveal.panel && previous >= REVEAL_GLOW_END) mu_ranked_update_result_shown();
    /* Existing game's menu confirmation, once at the swap; no imported sounds. */
    if (!s->reduced && s->result.status == MU_YG_RESULT_RECORDED &&
        previous < REVEAL_NEW_START && s->frame == REVEAL_NEW_START &&
        reveal_direction(&s->result)) lbAudioAx_80024030(1);
}
#endif
