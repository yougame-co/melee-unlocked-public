/* One YouGame CPU bracket game. Compiled in player builds, independent of agent hooks.
 * An event-style CSS lets the human pick freely while keeping the CPU/rules/stage immutable.
 * The VS and results scenes are the game's own; their normal teardown returns to the menu. */
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
#include <melee/mn/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/controller.h>
#include "mu_tourney.h"
#include "mu_tourney_game.h"

int mu_online_abi_command(unsigned int, const unsigned char*, unsigned int,
                          unsigned char*, unsigned int, unsigned int*);
void mu_replay_apply_game_info(StartMeleeData*, const unsigned char*);
int mu_online_singles_winner(int method);
int mu_practice_bridge(int op, int* a, int n);
enum { MU_PR_REQUEST_MAJOR = 6 };

static int active, started, completed, in_flight, returning, expired;
static GameMode* tournament_mode;
static GameModeState* original_states;
static VsModeData original_vs;
static int bracket_stage, cpu_character, cpu_level;
static s8 human_character = 33; /* ChKind_None: the player must pick */
static u8 human_color, human_nametag = 0x78;
static CSSData css_data;
static unsigned char info[312];

static void command(const unsigned char* payload, unsigned int size)
{
    unsigned char reply[MU_TOURNEY_REPLY_CAPACITY];
    unsigned int got = 0;
    mu_online_abi_command(MU_TOURNEY_COMMAND, payload, size, reply, sizeof reply, &got);
}

static void return_menu(void)
{
    returning = 1;
    gm_ChangeGameModeAfterCurrentScene(GM_MENU);
}

/* runGameMode calls mu_practice_enter_mode only AFTER the previous mode's scene exit
 * and on_unload. Restore here, never while the engine still traverses our scene table. */
void mu_tourney_enter_mode(int mode)
{
    if (!active || mode == GM_VS) return;
    tournament_mode->states = original_states;
    *gmVsMelee_GetVsData() = original_vs;
    memset(&original_vs, 0, sizeof original_vs);
    tournament_mode = NULL; original_states = NULL;
    active = started = completed = in_flight = returning = expired = 0;
    bracket_stage = cpu_character = cpu_level = 0;
    human_character = 33; human_color = 0; human_nametag = 0x78;
    memset(&css_data, 0, sizeof css_data);
    memset(info, 0, sizeof info);
    { const unsigned char p = MU_TOURNEY_EXIT; command(&p, 1); }
}

static void css_prep(GameModeState* state)
{
    CSSData* css = gm_GetGameModeStateEnterData(state);
    gm_801B06B0(css, 0xE, 33, 0, 0, 0x78, 0, 0);
    lbDvd_SetupVsPreloadCache();
}

static void css_decide(GameModeState* state)
{
    CSSData* css = gm_GetGameModeStateExitData(state);
    if (returning) { return_menu(); return; }
    if (css->pending_scene_change == 2) {
        mu_tourney_quit();
        return_menu();
        return;
    }
    gm_801B0730(css, &human_character, NULL, &human_color, &human_nametag, NULL);
    if (human_character < 0 || human_character >= 26) {
        gm_SetNextGameModeStateId(0);
        return;
    }
    gm_SetNextGameModeStateId(1); /* straight to VS: no stage or rule selection */
}

static void vs_prep(GameModeState* state)
{
    VsModeData* vs = gmVsMelee_GetVsData();
    mu_tourney_game_info(info, bracket_stage, human_character, human_color,
                         human_nametag, cpu_character, cpu_level);
    /* Populate before EnterVs so its DVD/sound preloads see both chosen fighters and the stage.
     * Reapply to the final start block, keeping saved VS preferences out of the bracket rules. */
    mu_replay_apply_game_info(&vs->start, info);
    gmVsMelee_EnterVs(state, vs, NULL, NULL);
    mu_replay_apply_game_info(gm_GetGameModeStateEnterData(state), info);
    started = 1;
}

static void vs_decide(GameModeState* state)
{
    int method = gmVsMelee_VsExitInfo.match_end.outcome;
    if (returning) { return_menu(); return; }
    gmVsMelee_ExitVs(state, 2, 2);
    /* No salty runback, sudden-death scene, repeat game or character unlock challenge. */
    if (method != 1 && method != 2) {
        return_menu();
        return;
    }
    gm_SetNextGameModeStateId(2);
}

static void results_prep(GameModeState* state) { gmVsMelee_EnterResults(state); }
static void results_decide(GameModeState* state) { (void) state; return_menu(); }

static GameModeState states[] = {
    { 0, lbDvdPreload_3, 0, css_prep, css_decide, { GS_CSS, &css_data, &css_data } },
    { 1, lbDvdPreload_3, 0, vs_prep, vs_decide,
      { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { 2, lbDvdPreload_3, 0, results_prep, results_decide,
      { GS_RESULTS, &gmVsMelee_ResultsEnterData, NULL } },
    { GM_GAMEMODESTATE_TERMINATE },
};

int mu_tourney_boot(void)
{
    unsigned char p = MU_TOURNEY_LAUNCH, reply[MU_TOURNEY_REPLY_CAPACITY];
    unsigned int got = 0;
    GameMode* mode;
    if (mu_online_abi_command(MU_TOURNEY_COMMAND, &p, 1, reply, sizeof reply, &got) != 0 ||
        got != 5 || reply[0] != 1) return 0;
    bracket_stage = (reply[1] << 8) | reply[2];
    cpu_character = reply[3]; cpu_level = reply[4];
    for (mode = gm_GetAllGameModes(); mode->kind != GM_COUNT; ++mode) {
        if (mode->kind == GM_VS) {
            tournament_mode = mode; original_states = mode->states;
            original_vs = *gmVsMelee_GetVsData();
            mode->states = states;
            active = 1; started = completed = in_flight = returning = expired = 0;
            return 1;
        }
    }
    return 0;
}

int mu_tourney_playing(void) { return active && started; }

/* Ranked cannot pause. The local CPU game still honors an actual LRAS as a forfeit;
 * read the human's physical input, never synthesize controller input or a match outcome. */
void mu_tourney_tick(void)
{
    const unsigned int lras = HSD_PAD_L | HSD_PAD_R | HSD_PAD_A | HSD_PAD_START;
    if (active && started && !completed && !in_flight && !returning && gmVs_GetSceneState()->match_result == 0 &&
        (HSD_PadMasterStatus[0].button & lras) == lras) {
        fn_8016CF4C(0, OUTCOME_NO_CONTEST);
    }
}

/* Host retrace polls this in CSS as well as gameplay/results. Request the same ordinary
 * scene leave used by practice handoffs; decide callbacks preserve the menu destination. */
void mu_tourney_poll(void)
{
    unsigned char p = MU_TOURNEY_STATUS, reply[MU_TOURNEY_REPLY_CAPACITY];
    unsigned int got = 0;
    if (!active) return;
    if (returning) {
        /* A retrace may expire during preload, before scene init resets the leave flag.
         * Keep requesting ordinary teardown until the new major mode actually enters. */
        if (expired) {
            int menu = GM_MENU;
            mu_practice_bridge(MU_PR_REQUEST_MAJOR, &menu, 1);
        }
        return;
    }
    if (mu_online_abi_command(MU_TOURNEY_COMMAND, &p, 1, reply, sizeof reply, &got) != 0 || got != 1) return;
    if (reply[0] == 2) { completed = 1; in_flight = 0; }
    if (reply[0] == 0) {
        int menu = GM_MENU;
        expired = 1; /* No late forfeit: the site's deadline already settled the walkover. */
        return_menu();
        mu_practice_bridge(MU_PR_REQUEST_MAJOR, &menu, 1);
    }
}

static void send_result(int method)
{
    unsigned char p[1 + MU_TOURNEY_RESULT_SIZE];
    unsigned int frames = started ? gmVs_GetSceneState()->frame_count : 0;
    int winner = started ? mu_online_singles_winner(method) : -1;
    p[0] = MU_TOURNEY_RESULT; p[1] = (unsigned char)method;
    p[2] = (unsigned char)(frames >> 24); p[3] = (unsigned char)(frames >> 16);
    p[4] = (unsigned char)(frames >> 8); p[5] = (unsigned char)frames;
    p[6] = (unsigned char)(started ? Player_GetStocks(0) : 4);
    p[7] = (unsigned char)(started ? Player_GetStocks(1) : 4);
    p[8] = (unsigned char)mu_tourney_result_winner(method, winner);
    in_flight = 1; /* Suppress callbacks while the host awaits its final answer/retry. */
    command(p, sizeof p);
}

void mu_tourney_report(int method)
{
    if (active && started && !completed && !in_flight && !expired && method != 0) send_result(method);
}

void mu_tourney_quit(void)
{
    /* The site owns disconnect/crash expiry; a linked orderly quit is best-effort once. */
    if (active && !completed && !in_flight && !expired) send_result(7);
}
