/* Native Slippi online menus: connect-code entry and stage select support.
 *
 * The screen code lives in the decomp (mn/mnnamenew.c, mn/mnstagesel.c, under MU_NATIVE). This file
 * holds the pieces those screens share with Slippi's CSS code and that need the core's state:
 *
 *   - FN_TX_LOCK_IN (HandleInputsOnCSS.asm): B5 with the CSS choice, a stage behavior and the
 *     frozen Stadium byte.
 *   - FN_TX_FIND_MATCH: B4 with the connect code typed on the name entry.
 *   - The lock-in callback Slippi keeps in r13-0x5018 (an enum natively, see s6-core-api.md 6):
 *     the connect-code confirm and the SSS pick run it.
 *   - The frozen Stadium toggle (the IngameCheckIfFrozen cave byte, CursorOnHoverStadium.asm).
 *   - The slpCSS symbol the CSS loads (SceneLoadCSS.asm); the name entry reads its connect help.
 *
 * Nothing here runs unless the Slippi-menus option is on (the callers are gated).
 */
#include <melee/gm/gm_1601.h>
#include <melee/gm/gmscdata.h>
#include <melee/gm/types.h>
#include <melee/mn/types.h>
#include <mu_native.h>
#include <mu_ranked.h>
#if MU_SQUAD
/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
#include "mu_squad.h"
#endif

extern char mnNameNew_CurrentNameText[];

static void* slpcss_root;

void mu_slippi_sss_set_slpcss(void* root)
{
    slpcss_root = root;
}

void* mu_slippi_sss_slpcss(void)
{
    return mu_slippi_menus_enabled() ? slpcss_root : NULL;
}

int mu_slippi_sss_frozen(void)
{
    return mu_slippi_state()->frozen_toggle;
}

/* CursorOnHoverStadium: xori 1 on the cave byte. */
void mu_slippi_sss_frozen_flip(void)
{
    MuSlippiMenuState* s = mu_slippi_state();
    s->frozen_toggle ^= 1;
}

/* Slippi reads the CSS data through the CSS's own pointer (r13-0x49F0, mnCharSel_804D6CB0), which
 * is the online major's CSS enter data; reach it through the major's state table. */
static CSSData* online_css_data(void)
{
    GameMode* mode;
    for (mode = gm_GetAllGameModes(); mode->kind != GM_COUNT; mode++) {
        if (mode->kind == GM_HANYU_CSS) {
            return mode->states != NULL ? (CSSData*) mode->states[0].info.enter_data : NULL;
        }
    }
    return NULL;
}

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
#define CMD_ARENA_PICK 0xDC /* upstream 0xEE: MU_MAKER_COMMAND in this build */

/* An arena room shows every player's portrait: the character and costume on this screen go to the
 * host whenever they change (Slippi's external id, 0xFF for none; the costume; picked or not), the
 * same values a lock-in sends. */
void mu_slippi_sss_report_pick(int chosen)
{
    static int last = -1;
    static unsigned char reply[4096];   /* the host's minimum response capacity */
    unsigned char payload[3];
    unsigned int got = 0;
    CSSData* css = online_css_data();
    u8 port = gm_801677F0();
    int ext, color, pick;

    if (css == NULL || port >= 4) {
        return;
    }
    ext = css->vs.start.players[port].ckind;
#ifdef MU_AKANEIA_FIGHTERS
    {
        const int mapped = mu_ak_mex_external(ext);
        ext = mapped >= 0 ? mapped : 0xFF;
    }
#endif
    color = css->vs.start.players[port].color;
    pick = (chosen ? 1 : 0) << 16 | (ext & 0xFF) << 8 | (color & 0xFF);
    if (pick == last) {
        return;
    }
    last = pick;
    payload[0] = (unsigned char) ext;
    payload[1] = (unsigned char) color;
    payload[2] = (unsigned char) (chosen ? 1 : 0);
    mu_online_abi_command(CMD_ARENA_PICK, payload, sizeof payload, reply, sizeof reply, &got);
}

/* FN_TX_LOCK_IN. stage_behavior: -2 random, -1 unset (use the opponent's), 0+ that stage id.
 * Returns 1 once the selections went to the host, 0 when Squad Strike refused the lock-in (its
 * reason is on screen): then there is nothing to search with. -1: a friend room's lock-in held
 * until the friend's game is heard from (nonzero, so whatever follows runs as it always did). */
int mu_slippi_sss_lock_in(int stage_behavior)
{
    MuSlippiMenuState* s = mu_slippi_state();
    MuSlippiSelections sel;
    CSSData* css = online_css_data();
    u8 port = gm_801677F0();   /* lbl_804D6598, r13-0x5108 */

    sel.char_id = 0;
    sel.char_color = 0;
    if (css != NULL && port < 4) {
        sel.char_id = (u8) css->vs.start.players[port].ckind;
#ifdef MU_AKANEIA_FIGHTERS
        {
            const int ext = mu_ak_mex_external(css->vs.start.players[port].ckind);
            sel.char_id = ext >= 0 ? (u8) ext : ChKind_None;
        }
#endif
        sel.char_color = css->vs.start.players[port].color;
    }
    sel.char_opt = 1;   /* merge character */
    sel.team_id = s->mode == MU_SLP_MODE_TEAMS ? (u8) (s->team_idx - 1) : 0;
    if (stage_behavior == -1) {
        sel.stage_id = 0;
        sel.stage_opt = MU_SLP_STAGE_UNSET;
    } else if (stage_behavior >= 0) {
        sel.stage_id = (u16) stage_behavior;
        sel.stage_opt = MU_SLP_STAGE_PICK;
    } else {
        sel.stage_id = 0;
        sel.stage_opt = MU_SLP_STAGE_RANDOM;
    }
    sel.alt_stage_mode = s->frozen_toggle;
    sel.online_mode = s->mode;
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    /* A friend room: never locked in before the friend's game said whether it plays Squad Strike. */
    if (s->mode == MU_SLP_MODE_DIRECT && !gmSquad_OnlineFriendLockIn())
        return gmSquad_OnlineFriendHeld() ? -1 : 0;
    if (gmSquad_OnlineLive(s->mode)) {
        if (!gmSquad_OnlineSelect(sel.char_id, sel.char_color)) return 0;
        /* A friend room has no search to check the memory floors and disc identity: lock-in does. */
        if (gmSquad_OnlineFriend() && !gmSquad_OnlineBudget()) return 0;
    }
#endif
    mu_slippi_set_selections(&sel);
    return 1;
}

/* FN_TX_FIND_MATCH: 2 of every 3 bytes of the typed name, 9 slots (the ninth reads past the eighth
 * letter, as on the console). asm HandleInputsOnCSS.asm:334-343 resets prep. */
void mu_slippi_sss_find_match(void)
{
    unsigned char code[18];
    int i;
    for (i = 0; i < 9; i++) {
        code[i * 2] = (unsigned char) mnNameNew_CurrentNameText[i * 3];
        code[i * 2 + 1] = (unsigned char) mnNameNew_CurrentNameText[i * 3 + 1];
    }
    mu_ranked_set_reset();
    mu_slippi_find_opponent(mu_slippi_state()->mode, code);
}

/* bctrl on r13-0x5018: FN_LOCK_IN_AND_SEARCH for the connect code, FN_TX_LOCK_IN for the SSS. */
void mu_slippi_sss_run_callback(int stage_behavior)
{
    switch (mu_slippi_state()->callback) {
    case MU_SLP_CB_CODE_LOCK_IN:
        if (mu_slippi_sss_lock_in(stage_behavior)) mu_slippi_sss_find_match();
        break;
    case MU_SLP_CB_SSS_LOCK_IN:
        mu_slippi_sss_lock_in(stage_behavior);
        break;
    default:
        break;
    }
}
