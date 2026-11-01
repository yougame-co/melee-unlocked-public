/* Online-major Ranked UI data. GPL-3.0-or-later. */
#include <mu_native.h>
#include <mu_ranked.h>
#include <string.h>

static MuRankedSet ranked_set;
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH && !defined(MU_NO_SLIPPI)
int mu_ranked_practice(void)
{
    MuYgU8 sub = MU_YG_SUB_PRACTICE_STATUS, active = 0;
    unsigned got = 0;
    return mu_slippi_cmd(MU_YG_COMMAND, &sub, 1, &active, 1, &got) == 0 && got == 1 && active == 1;
}
int mu_ranked_practice_allowed(void)
{
    MuYgU8 sub = MU_YG_SUB_PRACTICE_ALLOWED, allowed = 0;
    unsigned got = 0;
    return mu_slippi_cmd(MU_YG_COMMAND, &sub, 1, &allowed, 1, &got) == 0 && got == 1 && allowed == 1;
}
int mu_ranked_practice_control(unsigned action)
{
    MuYgU8 request[2] = {MU_YG_SUB_PRACTICE, (MuYgU8)action}, ok = 0;
    unsigned got = 0;
    return mu_slippi_cmd(MU_YG_COMMAND, request, 2, &ok, 1, &got) == 0 && got == 1 && ok == 1;
}
#endif
MuRankedSet* mu_ranked_set_mut(void) { return &ranked_set; }
const MuRankedSet* mu_ranked_set(void) { return &ranked_set; }
void mu_ranked_set_reset(void)
{
    memset(&ranked_set, 0, sizeof ranked_set);
    /* Practice vs CPU plays the same best of three as Ranked (striking, game 1, then bans and
     * counterpicks), so it teaches the real set (a one-game practice read as Ranked). */
    ranked_set.max_games = 3;
    ranked_set.cur_game = 1;
    ranked_set.prev_winner = 0xFF;
    memset(ranked_set.game_results, 0xFF, sizeof ranked_set.game_results);
    ranked_set.last_stage_win[0] = ranked_set.last_stage_win[1] = 0xFFFF;
}
int mu_ranked_read_ranks(MuYgRanks* out)
{
    MuYgU8 sub = MU_YG_SUB_RANK;
    MuYgU8 reply[MU_YG_RANK_RESPONSE_SIZE];
    unsigned got = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
#ifdef MU_NO_SLIPPI
    return 0;
#else
    /* Optional API-22 command: failure leaves an unknown rank for text fallback. */
    return mu_slippi_cmd(MU_YG_COMMAND, &sub, 1, reply, sizeof reply, &got) == 0 &&
           mu_yg_ranks_decode(out, reply, got);
#endif
}
int mu_ranked_read_policy(MuRankedPolicy* out)
{
    MuYgU8 sub = MU_YG_SUB_POLICY;
    MuYgU8 reply[MU_YG_POLICY_SIZE];
    unsigned got = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
#ifdef MU_NO_SLIPPI
    return 0;
#else
    /* No policy means Ranked refused: never guess a remix's stage mapping. */
    return mu_slippi_cmd(MU_YG_COMMAND, &sub, 1, reply, sizeof reply, &got) == 0 &&
           mu_yg_policy_decode(out, reply, got) && out->available;
#endif
}
