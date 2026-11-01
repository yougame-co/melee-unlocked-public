/* Ranked scene transport; GPL-3.0-or-later. Slippi source references are relative
 * to slippi-ssbm-c d7174ca unless marked asm. All choices live outside match state. */
#include <mu_native.h>
#include <mu_ranked_setup_online.h>
/* Scene exit only; keeping decomp structs out also permits host adapter tests. */
void gm_801A4B60(void);
#include <string.h>
void mu_online_abi_log(const char* text);
int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);

static int update_set_active, update_result_pending;
int mu_ranked_update_busy(void) { return update_set_active; }
void mu_ranked_update_reset(void) { update_set_active = update_result_pending = 0; }
void mu_ranked_update_result_shown(void) {
    if (update_result_pending) mu_ranked_update_reset();
}
static MuRankedSetupModel model;
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
static MuRankedSetupModel cpu_model;
static MuRankedSet cpu_set;
#endif
static MuRankedSetupChoice draft;
static unsigned draft_step;
static int has_draft;
static unsigned ready_frames;
static int sent_result, ended, result_winner = -1, result_disconnect = 1;
static unsigned result_end;
const MuRankedSetupModel* mu_ranked_setup_model(void) { return &model; }
int mu_ranked_setup_cpu(void)
{
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
    return strcmp(mu_slippi_match_state()->match_id, "agent-ranked-cpu") == 0;
#else
    return 0;
#endif
}
static void abort_setup(void)
{
    MuYgU8 mode = 1;
    if (ended) return;
    ended = 1;
    /* The set ends here, as a SET_ABORTED game would end it: its result reveal releases the
     * update gate. A conclusive game whose link is gone now gets here, not to finish_game's abort. */
    update_result_pending = 1;
    mu_ranked_setup_disconnect(&model);
    mu_slippi_cmd(0xC2, &mode, 1, NULL, 0, NULL);
    mu_slippi_cleanup_connections();
    gm_801A4B60();
}
static void publish_result(void)
{
    MuYgU8 payload[14] = {0};
    MuSlippiSelections selection;
    unsigned i;
    if (sent_result || model.status != MU_SETUP_COMPLETE) return;
    /* CompleteGamePrep :985-1059. Tiebreak :22-33 retains the prior BF override. */
    if (!model.set->tiebreak_num) {
        mu_yg_put16(payload, model.result.stage);
        for (i = 0; i < 2; ++i) {
            payload[2+3*i] = 1;
            payload[3+3*i] = model.result.fighters[i].character;
            payload[4+3*i] = model.result.fighters[i].color;
        }
        if (mu_slippi_cmd(0xBF, payload, sizeof payload, NULL, 0, NULL)) { abort_setup(); return; }
    }
    if (model.set->cur_game > 1 || model.set->tiebreak_num) {
        memset(&selection, 0, sizeof selection);
        selection.char_id = model.result.fighters[model.local_player].character;
        selection.char_color = model.result.fighters[model.local_player].color;
        selection.stage_id = model.result.stage;
        selection.char_opt = 2 /* MERGE, GameSetup.c:1055 */;
        /* Gameplay clears stage-selected flags between games. Publish the
         * agreed stage explicitly so BF's overwrite is used by the host. */
        selection.stage_opt = MU_SLP_STAGE_PICK;
        selection.online_mode = MU_SLP_MODE_RANKED;
        mu_slippi_set_selections(&selection);
    }
    sent_result = 1;
}
void mu_ranked_setup_begin(void)
{
    MuRankedPolicy policy;
    MuRankedSetupResult previous;
    const MuMatchState* ms;
    MuYgU8 status;
    unsigned i;
    update_set_active = 1; /* conservative: include game-one setup/striking */
    update_result_pending = 0;
    memset(&model, 0, sizeof model);
    sent_result = ended = has_draft = 0; ready_frames = 0;
    /* Require a fresh finalized result for every match (including a tiebreak). */
    result_winner = -1; result_disconnect = 1; result_end = 0;
    if (mu_slippi_load_match_state(NULL) || !mu_ranked_read_policy(&policy)) { abort_setup(); return; }
    ms = mu_slippi_match_state();
    if (ms->connection_state != MU_SLP_MM_CONNECTION_SUCCESS) { abort_setup(); return; }
    previous.stage = mu_yg_get16(ms->game_info + 0xE);
    for (i = 0; i < 2; ++i) {
        previous.fighters[i].character = ms->game_info[0x60+0x24*i];
        previous.fighters[i].color = ms->game_info[0x63+0x24*i];
    }
    if (!mu_ranked_setup_model_init(&model, &policy, mu_ranked_set_mut(), ms->local_index, &previous)) {
        abort_setup(); return;
    }
    if (!model.set->tiebreak_num) {
        /* minor_load :44-50, game_setup_N. */
        status = (MuYgU8)(9 + model.set->cur_game);
        mu_slippi_cmd(0xC4, &status, 1, NULL, 0, NULL);
    }
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
    if (mu_ranked_setup_cpu()) {
        cpu_set = *mu_ranked_set();
        mu_ranked_setup_model_init(&cpu_model, &policy, &cpu_set, !ms->local_index, &previous);
    }
#endif
    publish_result();
}
void mu_ranked_setup_set_draft(const MuRankedSetupChoice* choice)
{
    has_draft = choice != NULL;
    draft_step = model.step;
    if (choice) draft = *choice;
}
int mu_ranked_setup_submit(const MuRankedSetupChoice* choice, int automatic)
{
    MuYgU8 packet[5];
    const MuRankedSetupStep* s = mu_ranked_setup_current(&model);
    if (!s || s->player != model.local_player || ended ||
        !mu_ranked_setup_packet(&model, choice, automatic, packet)) return 0;
    /* :949-958: only the controlling seat sends C0. Apply only after send succeeds. */
    if (mu_slippi_cmd(0xC0, packet, sizeof packet, NULL, 0, NULL)) { abort_setup(); return 0; }
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
    if (mu_ranked_setup_cpu()) {
        MuYgU8 response[6] = {1,0,0,0,0,0};
        memcpy(response+2, packet+1, 4);
        if (!mu_ranked_setup_remote(&cpu_model, response)) { abort_setup(); return 0; }
    }
#endif
    if (!mu_ranked_setup_apply(&model, model.local_player, packet)) { abort_setup(); return 0; }
    publish_result();
    return 1;
}
void mu_ranked_setup_frame(void)
{
    const MuRankedSetupStep* s;
    MuYgU8 response[6] = {0}, index;
    unsigned got = 0;
    int timeout;
    if (ended) return;
    /* InputsThink :797-812: connection loss at ANY step, even the ready handshake. */
    if (mu_slippi_load_match_state(NULL) ||
        mu_slippi_match_state()->connection_state != MU_SLP_MM_CONNECTION_SUCCESS) { abort_setup(); return; }
    if (model.status == MU_SETUP_COMPLETE) {
        /* minor_think :138-169: wait for both ready, at most 15 s / 5 tiebreaks. */
        if (++ready_frames > 60u * model.policy.wait_seconds || model.set->tiebreak_num >= 5) { abort_setup(); return; }
        if (mu_slippi_match_state()->local_ready && mu_slippi_match_state()->remote_ready) {
            ended = 1; gm_801A4B60();
        }
        return;
    }
    s = mu_ranked_setup_current(&model);
    if (!s) { abort_setup(); return; }
    timeout = mu_ranked_setup_tick(&model);
    if (s->player == model.local_player) {
        if (timeout == MU_SETUP_LOCAL_TIMEOUT)
            mu_ranked_setup_submit(has_draft && draft_step == model.step ? &draft : NULL, 1);
    } else {
        /* HandleOpponentStep :844-892. CPU test feeds precisely this response/apply path. */
        index = model.step;
        if (mu_slippi_cmd(0xC1, &index, 1, response, sizeof response, &got) || got != 6) { abort_setup(); return; }
#if defined(YOUGAME_AGENT_TEST_HOOK) || (defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH)
        if (mu_ranked_setup_cpu() && model.frames >= 30) {
            MuYgU8 packet[5];
            /* A second seat, same model APIs as a remote client; 0.5 s think time. */
            if (!mu_ranked_setup_packet(&cpu_model, NULL, 1, packet) ||
                !mu_ranked_setup_apply(&cpu_model, cpu_model.local_player, packet)) { abort_setup(); return; }
            response[0] = 1; response[1] = 0; memcpy(response+2, packet+1, 4);
        }
#endif
        if (response[0]) {
            if (!mu_ranked_setup_remote(&model, response)) { abort_setup(); return; }
            publish_result();
        } else if (timeout == MU_SETUP_REMOTE_TIMEOUT) abort_setup();
    }
}
void mu_ranked_setup_end(void) { /* A2 releases screen objects here. Model stays readable. */ }
void mu_ranked_setup_game_result(int winner, unsigned end_mode, int disconnected)
{
    result_winner = winner; result_end = end_mode; result_disconnect = disconnected;
}
int mu_ranked_setup_finish_game(int connected, MuYgU16 stage)
{
    MuRankedSet* set = mu_ranked_set_mut();
    unsigned game = set->cur_game;
    /* Slippi scores a conclusive game and ends the set only on its disconnect winner (-3, shown
     * during the game; asm main.asm:617-741). The setup scene checks the link on every frame and
     * ends the set there if it is really gone. 11-01: a post-game read that said "not connected"
     * abandoned the set after a conclusive game 1, so that read no longer decides; it is logged. */
    int result = mu_ranked_setup_record_game(set, result_winner, result_end, stage, result_disconnect);
    if (result == MU_SETUP_SET_OVER || result == MU_SETUP_SET_ABORTED) update_result_pending = 1;
    if (result == MU_SETUP_NEXT_GAME || result == MU_SETUP_TIEBREAK || result == MU_SETUP_SET_OVER ||
        result == MU_SETUP_SET_ABORTED) {
        unsigned local = mu_slippi_match_state()->local_index & 1;
        int practice = mu_ranked_setup_cpu();
        char line[192];
        /* A disconnect or an inconclusive end abandons the set: no winner is recorded. */
        snprintf(line, sizeof line,
            "ranked set: game %u over, winner %s, score %u-%u, practice %d, next %s\n",
            game, result == MU_SETUP_SET_ABORTED ? "none" : result_winner < 0 ? "tie" :
                  result_winner == (int)local ? "local" : "opponent",
            (unsigned)set->score[local], (unsigned)set->score[!local], practice,
            result == MU_SETUP_NEXT_GAME ? "setup" : result == MU_SETUP_TIEBREAK ? "tiebreak" :
            result == MU_SETUP_SET_OVER ? "set over" : "abandoned");
        mu_online_abi_log(line);
        if (!connected && result != MU_SETUP_SET_ABORTED) {
            snprintf(line, sizeof line, "ranked set: game %u counted although the link read state %u "
                     "after it; the next screen checks the link again\n", game,
                     (unsigned)mu_slippi_match_state()->connection_state);
            mu_online_abi_log(line);
        }
        if (result == MU_SETUP_SET_ABORTED) {
            /* Which of record_game's conditions ended it (11-01: the set died after a conclusive
             * game 1; only the post-game link read can have said "disconnected"). */
            const MuMatchState* ms = mu_slippi_match_state();
            snprintf(line, sizeof line, "ranked set: abandoned after game %u: disconnect shown %d, "
                     "link state %u (%s), winner %d\n", game, result_disconnect,
                     (unsigned)ms->connection_state, connected ? "connected" : "not connected", result_winner);
            mu_online_abi_log(line);
            if (ms->error[0]) {
                snprintf(line, sizeof line, "ranked set: link error: %.120s\n", ms->error);
                mu_online_abi_log(line);
            }
        }
        if (result == MU_SETUP_SET_OVER) {
            snprintf(line, sizeof line, "ranked set: over %u-%u, practice %d\n",
                     (unsigned)set->score[local], (unsigned)set->score[!local], practice);
            mu_online_abi_log(line);
        }
    }
    return result;
}
