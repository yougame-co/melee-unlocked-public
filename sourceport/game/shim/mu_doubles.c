/* Console Doubles uses retail VS and four synchronized controller ports. */
#ifdef MELEE_DOUBLES
#include <string.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmscdata.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/types.h>
#include <melee/lb/lbdvd.h>
#include <melee/mn/types.h>
#include "mu_doubles_game.h"

int mu_online_abi_command(unsigned int, const unsigned char*, unsigned int,
                          unsigned char*, unsigned int, unsigned int*);
static GameMode* doubles_mode;
static GameModeState* original_states;
static VsModeData original_vs;
static CSSData css_data;
/* stocks, minutes, items, friendly fire, stage (BE16), four teams by port */
static unsigned char frozen[10];
/* A Doubles session just gave VS back: the next menu entry opens Online on Doubles. */
static int menu_return;
int mu_doubles_active(void) { return doubles_mode != NULL; }
/* The 4-peer route (yougame_doubles_teams.h): set when the online menu opens the Teams character select
 * for the home's match, cleared when that select gives the menu back. Menu and CSS only. */
static int teams_css, teams_team = -1;
void mu_doubles_teams_enter(int team)
{
    teams_css = 1;
    teams_team = team == 0 || team == 1 ? team : -1;
}
int mu_doubles_teams_css(void) { return teams_css; }
int mu_doubles_teams_team(void) { return teams_team; }
int mu_doubles_take_teams_return(void)
{
    const int back = teams_css;
    teams_css = 0;
    teams_team = -1;
    return back;
}
int mu_doubles_take_menu_return(void)
{
    const int back = menu_return;
    menu_return = 0;
    return back;
}
void mu_doubles_apply(VsModeData* vs)
{
    GameRules* rules;
    struct GamePrefs* prefs;
    int i;
    if (!mu_doubles_active()) return;
    rules = gmMainLib_GetGameRules(); prefs = gmMainLib_GetGamePrefs();
    rules->mode = 1; rules->stock_count = frozen[0];
    rules->stock_time_limit = frozen[1]; rules->friendly_fire = frozen[3];
    prefs->item_freq = frozen[2] == 0 ? 0xFF : frozen[2];
    prefs->item_mask = 0;
    gm_80167BC8(vs);
    vs->start.rules.is_teams = 1;
    vs->start.rules.stkind = (frozen[4] << 8) | frozen[5];
    for (i = 0; i < 4; ++i) {
        vs->start.players[i].team = frozen[6 + i];
        vs->start.players[i].slot_type = Gm_PKind_Human;
        vs->start.players[i].stocks = frozen[0];
    }
}
void mu_doubles_enter_mode(int mode)
{
    if (!doubles_mode || mode == GM_VS) return;
    doubles_mode->states = original_states;
    *gmVsMelee_GetVsData() = original_vs;
    doubles_mode = NULL; original_states = NULL;
    menu_return = 1;
}
static void css_prep(GameModeState* state)
{
    CSSData* css;
    mu_doubles_apply(gmVsMelee_GetVsData());
    gmVsMelee_EnterCss(state, gmVsMelee_GetVsData(), VS_MELEE);
    css = gm_GetGameModeStateEnterData(state);
    mu_doubles_apply(&css->vs);
}
static void css_decide(GameModeState* state)
{
    CSSData* css = gm_GetGameModeStateExitData(state);
    if (css->pending_scene_change == 2) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        return;
    }
    mu_doubles_apply(&css->vs);
    gmVsMelee_ExitCss(state, gmVsMelee_GetVsData());
    gm_SetNextGameModeStateId(1); /* seed chose the stage; no stage picker */
}
static void vs_prep(GameModeState* state)
{
    VsModeData* vs = gmVsMelee_GetVsData();
    mu_doubles_apply(vs);
    gmVsMelee_EnterVs(state, vs, NULL, NULL);
    /* EnterVs copies the same frozen rules and four CSS selections to StartMeleeData. */
}
static void vs_decide(GameModeState* state)
{
    int method = gmVsMelee_VsExitInfo.match_end.outcome;
    gmVsMelee_ExitVs(state, 2, 2);
    /* A decided match (time, or the last team standing) shows who won before the menu; a no contest
     * goes straight back. */
    if (method != 1 && method != 2 && method != 3) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        return;
    }
    gm_SetNextGameModeStateId(2);
}
static void results_prep(GameModeState* state) { gmVsMelee_EnterResults(state); }
static void results_decide(GameModeState* state)
{
    (void) state;
    gm_ChangeGameModeAfterCurrentScene(GM_MENU);
}
static GameModeState states[] = {
    { 0, lbDvdPreload_3, 0, css_prep, css_decide, { GS_CSS, &css_data, &css_data } },
    { 1, lbDvdPreload_3, 0, vs_prep, vs_decide,
      { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { 2, lbDvdPreload_3, 0, results_prep, results_decide,
      { GS_RESULTS, &gmVsMelee_ResultsEnterData, NULL } },
    { GM_GAMEMODESTATE_TERMINATE },
};
int mu_doubles_boot(void)
{
    unsigned char query = 11;
    unsigned int got = 0;
    GameMode* mode;
    if (doubles_mode) return 1;
    if (mu_online_abi_command(0xE6, &query, 1, frozen, sizeof frozen, &got) != 0 || got != sizeof frozen) return 0;
    for (mode = gm_GetAllGameModes(); mode->kind != GM_COUNT; ++mode) if (mode->kind == GM_VS) {
        doubles_mode = mode; original_states = mode->states;
        original_vs = *gmVsMelee_GetVsData(); mode->states = states;
        return 1;
    }
    return 0;
}
#endif
