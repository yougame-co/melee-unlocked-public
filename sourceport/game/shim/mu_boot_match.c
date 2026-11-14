/* Agent test bootMatch: the --match VS game straight from the main menu.
 *
 * --match only forces the players where gmVsMelee_EnterVs finalises them (mu_match.c); the
 * character and stage screens still run, so a test had to drive them. With bootMatch the host
 * asks for this once, from the main menu, and GM_VS runs a two-state table instead of its own:
 * the match, then the results, then the main menu. It is the CPU bracket's shape (mu_tourney.c):
 * the game info is applied before gmVsMelee_EnterVs so its preloads see the forced fighters and
 * stage, and again after it, onto the final start block. The rules are the singles online block
 * (mu_tourney.h). Built with and without the Slippi layer; nothing here runs unless the host asks.
 */
#include <string.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_1B03.h>
#include <melee/gm/gmscdata.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/types.h>
#include <melee/lb/lbdvd.h>
#include "mu_tourney.h"

int mu_match_request(int* stage, int* kind, int* cpu, int* level, int* costume, int count);
void mu_replay_apply_game_info(StartMeleeData*, const unsigned char*);
struct gm_80479D58_t* mu_gm_engine_state(void);

static GameMode* boot_mode;
static GameModeState* original_states;
static VsModeData original_vs;
static unsigned char info[312];

/* Leaving GM_VS for any other mode puts its own table back. runGameMode calls this only after
 * the previous mode's scene exit and unload, never while the engine still walks our table. */
void mu_boot_match_enter_mode(int mode)
{
    if (boot_mode == NULL || mode == GM_VS) return;
    boot_mode->states = original_states;
    *gmVsMelee_GetVsData() = original_vs;
    boot_mode = NULL;
    original_states = NULL;
}

static void vs_prep(GameModeState* state)
{
    VsModeData* vs = gmVsMelee_GetVsData();
    mu_replay_apply_game_info(&vs->start, info);
    gmVsMelee_EnterVs(state, vs, NULL, NULL);
    mu_replay_apply_game_info(gm_GetGameModeStateEnterData(state), info);
}

static void vs_decide(GameModeState* state)
{
    int method = gmVsMelee_VsExitInfo.match_end.outcome;
    gmVsMelee_ExitVs(state, 2, 2);
    /* No sudden-death scene or repeat game: a decided match shows its results, anything else
     * (a no contest) goes straight back to the menu. */
    if (method != 1 && method != 2) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        return;
    }
    gm_SetNextGameModeStateId(1);
}

static void results_prep(GameModeState* state) { gmVsMelee_EnterResults(state); }
static void results_decide(GameModeState* state)
{
    (void) state;
    gm_ChangeGameModeAfterCurrentScene(GM_MENU);
}

static GameModeState states[] = {
    { 0, lbDvdPreload_3, 0, vs_prep, vs_decide,
      { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { 1, lbDvdPreload_3, 0, results_prep, results_decide,
      { GS_RESULTS, &gmVsMelee_ResultsEnterData, NULL } },
    { GM_GAMEMODESTATE_TERMINATE },
};

/* The game info for the --match players: kind, human or CPU and its level, costume, stage. */
static int build_info(void)
{
    int stage = -1, kind[6], cpu[6], level[6], costume[6], i, players = 0;
    if (!mu_match_request(&stage, kind, cpu, level, costume, 6) || stage < 0) return 0;
    mu_tourney_game_info(info, stage, 33, 0, 0x78, 33, 1);
    for (i = 0; i < 6; ++i) {
        unsigned char* p = info + 96 + i * 36;
        if (kind[i] < 0) {
            p[0] = 33;   /* no fighter */
            p[1] = 3;    /* slot off */
            continue;
        }
        p[0] = (unsigned char) kind[i];
        p[1] = (unsigned char) (cpu[i] ? 1 : 0);
        p[3] = (unsigned char) costume[i];
        p[15] = (unsigned char) (cpu[i] ? level[i] : 1);
        ++players;
    }
    return players >= 1;
}

/* While the boot is pending, the main menu still runs its own exit, and its decide picks the title
 * screen (the leave flag reads as "no menu choice"): without this the request landed on the title and
 * its attract demo (Linux test PC, 11-09). Hold VS as the destination until GM_VS is entered, the way a practice
 * handoff holds its own (mu_practice_resolve_pending_mode, which calls this). */
int mu_boot_match_resolve(int requested)
{
    return boot_mode != NULL && gm_GetCurrentGameMode() == GM_MENU ? GM_VS : requested;
}

/* The host's bootMatch (MU_BOOT_MATCH_OP): only from the main menu, only with --match. Swaps
 * GM_VS's table and leaves the menu for it, the way MU_PRACTICE_REQUEST_MAJOR leaves a scene.
 * Returns the stage on success so the host can log it, -1 when refused. */
int mu_boot_match_boot(void)
{
    GameMode* mode;
    if (boot_mode != NULL || gm_GetCurrentGameMode() != GM_MENU || !build_info()) return -1;
    for (mode = gm_GetAllGameModes(); mode->kind != GM_COUNT; ++mode) {
        if (mode->kind == GM_VS) {
            boot_mode = mode;
            original_states = mode->states;
            original_vs = *gmVsMelee_GetVsData();
            mode->states = states;
            gm_ChangeGameModeAfterCurrentScene(GM_VS);
            mu_gm_engine_state()->unk_C = 1;
            return (info[14] << 8) | info[15];
        }
    }
    return -1;
}
