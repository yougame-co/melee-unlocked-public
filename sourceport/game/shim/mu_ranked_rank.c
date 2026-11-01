/* Online CSS RankInfo, adapted from project-slippi/slippi-ssbm-c
 * Scenes/CSS/RankInfo/RankInfo.c:25-188 and Scenes/CSS/Main.c (GPL-3.0).
 * YouGame supplies the standing, words and badge; no Slippi art or server IO.
 * This panel only reads UI state. The post-set reveal belongs to Track C.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <mu_native.h>
#include <mu_rank_badge.h>
#include "mu_rank_text.h"
#include <stdio.h>
#include <string.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjuserdata.h>
#include <sysdolphin/baselib/sislib.h>

/* CSS already draws the full division/practice label beside the art. */
void mu_rank_badge_art_only(MuRankBadge*);

enum { LOCAL_TITLE, LOCAL_DETAIL, OPP_TITLE, OPP_DETAIL, SET_SCORE,
       LOCAL_LABEL, OPP_LABEL, LINE_COUNT };
static struct {
    HSD_Text* text;
    MuRankBadge* badges[2];
    MuYgRank shown[2];
    char lines[LINE_COUNT][64];
    unsigned frames;
    int matched;
} panel;
static const GXColor white = { 255, 255, 255, 255 };
static const GXColor gray = { 160, 165, 175, 255 };

/* CSS subtexts are relative to (0,0), in tenths of a world unit, y down.
 * Badges take world coordinates; use the same origin and font-size factor. */
static float panel_x(float x) { return x * 0.1f; }
static float panel_y(float y) { return y * 0.1f; }

static int active(void)
{
    return mu_slippi_css_online() && mu_slippi_state()->mode == MU_SLP_MODE_RANKED;
}

static void line(int id, const char* value)
{
    if (strcmp(panel.lines[id], value) == 0) return;
    /* All callers supply bounded local strings, never a host format string. */
    strcpy(panel.lines[id], value);
    HSD_SisLib_803A70A0(panel.text, id, "%s", value);
}

static void free_panel(void* unused)
{
    (void) unused;
    mu_rank_badge_free(panel.badges[0]);
    mu_rank_badge_free(panel.badges[1]);
    if (panel.text) HSD_SisLib_803A5CC4(panel.text);
    memset(&panel, 0, sizeof panel);
}

static void rank_panel(int player, const MuYgRank* rank, int status)
{
    char detail[64], label[32], rating[24];
    MuYgRank display = *rank;
    int id = player ? OPP_DETAIL : LOCAL_DETAIL;
    if (!rank->known) {
        const char* state = status == 0 ? "Loading..." : status == 2 ? "Rank unavailable" : "Unknown";
        strcpy(detail, state);
    } else if (!rank->placed) {
        mu_rank_text_label(label, rank);
        strcpy(display.label, label);
        mu_rank_number(rating, rank->rating);
        sprintf(detail, mu_ranked_practice() ? "Practice rating %s" : "Rating %s", rating);
    } else {
        /* Preserve YouGame's label, including IV and Challenger. No E3 enum is
         * used for art or words: it compresses YouGame's four divisions. */
        display.label[sizeof display.label - 1] = 0;
        mu_rank_number(rating, rank->rating);
        sprintf(detail, mu_ranked_practice() ? "Practice rating %s" : "Rating %s", rating);
    }
    if (!player && status == 0 && rank->known) strcat(detail, " - Loading...");
    if (!player && status == 2) strcpy(detail, "Rank unavailable");
    if (!panel.badges[player]) {
        /* 25.2 CSS units square, centered at y 89/202 across the three rows.
         * Leave a four-unit gap after the label and keep local art below x 70. */
        panel.badges[player] = mu_rank_badge_create(NULL, panel_x(player ? 230 : 40),
            panel_y(player ? 189.4f : 76.4f), 0.105f, &display);
        mu_rank_badge_art_only(panel.badges[player]);
        panel.shown[player] = display;
    } else if (memcmp(&panel.shown[player], &display, sizeof display) != 0) {
        mu_rank_badge_set(panel.badges[player], &display);
        panel.shown[player] = display;
    }
    /* The full label carries the division beside the CSS art. */
    {
        int label_id = player ? OPP_LABEL : LOCAL_LABEL;
        const char* value = rank->known ? display.label : "";
        /* Measured advance: 20.5 CSS units at scale 1. Fit all wire labels,
         * including Practice Platinum IV and placement totals up to 255. */
        float width = (float)strlen(value) * 20.5f;
        float available = player ? 156.0f : 148.0f;
        float scale = width > available / .5f ? available / width : .5f;
        HSD_SisLib_803A7548(panel.text, label_id, scale, scale);
        line(label_id, value);
    }
    line(id, detail);
}

static void refresh(void)
{
    MuYgRanks ranks;
    MuYgU8 e3[16];
    unsigned got = 0;
    int status = 1;
    int ok = mu_ranked_read_ranks(&ranks);
    line(LOCAL_TITLE, mu_ranked_practice() ? "Practice - not rated" : "Your rank");
    const MuMatchState* ms = mu_slippi_css_msrb();
    int matched = ms->connection_state == MU_SLP_MM_OPPONENT_CONNECTING ||
                  ms->connection_state == MU_SLP_MM_CONNECTION_SUCCESS;
    if (!ok) status = 2;
    else {
        if (mu_slippi_cmd(0xE3, NULL, 0, e3, sizeof e3, &got) == 0 && got == sizeof e3)
            status = e3[1] <= 2 ? e3[1] : 2;
        else status = 2;
    }
    rank_panel(0, &ranks.self, status);
    if (matched) {
        line(OPP_TITLE, mu_ranked_practice() ? "CPU practice rank" : "Opponent rank");
        /* A missing roster standing is unknown, not an invented placement or
         * a loader that can never finish. A failed record read is an error. */
        rank_panel(1, &ranks.opponent, ok ? 1 : 2);
    } else {
        line(OPP_TITLE, "");
        line(OPP_DETAIL, "");
        line(OPP_LABEL, "");
        mu_rank_badge_free(panel.badges[1]);
        panel.badges[1] = NULL;
        memset(&panel.shown[1], 0, sizeof panel.shown[1]);
    }
    panel.matched = matched;
}

void mu_ranked_rank_enter(void)
{
    HSD_GObj* owner;
    int i;
    static const float ys[LINE_COUNT] = { 65, 105, 178, 218, 115, 85, 198 };
    /* Like SlippiCSS's chat hooks: the scene arena is discarded on transition,
     * even when destructors aren't run. Never free last scene's stale pointers. */
    memset(&panel, 0, sizeof panel);
    if (!active()) return;
    panel.text = mu_rank_text_create();
    if (!panel.text) return;
    panel.text->default_kerning = 1;
    panel.text->pos_x = 0;
    panel.text->pos_y = 0;
    panel.text->font_size.x = panel.text->font_size.y = 0.1f;
    for (i = 0; i < LINE_COUNT; ++i) {
        int local = i == LOCAL_TITLE || i == LOCAL_LABEL || i == LOCAL_DETAIL;
        int label = i == LOCAL_LABEL || i == OPP_LABEL;
        float scale = label ? 0.5f : 0.4f;
        int id = HSD_SisLib_803A6B98(panel.text,
            local ? -112.0f : 70.0f, ys[i], "%s", "");
        HSD_SisLib_803A7548(panel.text, id, scale, scale);
        HSD_SisLib_803A74F0(panel.text, id,
            (GXColor*) (label || i == SET_SCORE ? &white : &gray));
    }
    owner = GObj_Create(4, 5, 0x80);
    if (!owner) { free_panel(NULL); return; }
    GObj_InitUserData(owner, 4, free_panel, NULL);
    line(LOCAL_TITLE, "Your rank");
    line(LOCAL_DETAIL, "Loading...");
    /* E4 is host-local (YouGame's result already arrives through its room).
     * As RankInfo does, request it only on return from a game, not menu entry. */
    if (mu_ranked_set()->cur_game > 1 || mu_ranked_set()->score[0] || mu_ranked_set()->score[1])
        mu_slippi_cmd(0xE4, NULL, 0, NULL, 0, NULL);
    refresh();
}

void mu_ranked_rank_frame(void)
{
    char score[64] = "";
    const MuRankedSet* set;
    const MuMatchState* ms;
    int matched;
    if (!active() || mu_slippi_css_scene_request()) return;
    /* Scene assets can become ready after OnEnter. Badges retry in refresh(). */
    if (!panel.text) mu_ranked_rank_enter();
    if (!panel.text) return;
    ms = mu_slippi_css_msrb();
    matched = ms->connection_state == MU_SLP_MM_OPPONENT_CONNECTING ||
              ms->connection_state == MU_SLP_MM_CONNECTION_SUCCESS;
    /* Cheap cached commands; poll at 4 Hz and immediately on match/disconnect. */
    if (matched != panel.matched || ++panel.frames >= 15) {
        panel.frames = 0;
        refresh();
    }
    set = mu_ranked_set();
    if (set->cur_game > 1 && set->max_games &&
        (ms->connection_state == MU_SLP_MM_IDLE || matched))
        /* By port in the set; the local player's count first, as on the setup screen. */
        sprintf(score, "Game %u - %u:%u", (unsigned) set->cur_game,
                (unsigned) set->score[(ms->local_index & 3) == 1],
                (unsigned) set->score[(ms->local_index & 3) != 1]);
    line(SET_SCORE, score);
}
