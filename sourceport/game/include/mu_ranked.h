/* Shared Ranked client state, owned by the online major. GPL-3.0-or-later. */
#ifndef MU_RANKED_H
#define MU_RANKED_H
#include <mu_ranked_wire.h>

/* Native equivalent of Slippi Online.s:517-546 / GameSetup.h:22-37.
 * No function pointer or console packing: this is native UI bookkeeping, not a
 * wire/simulation block. cur_game is 1-based, prev_winner/results are port 0/1;
 * 0xFF means no winner/result. last_stage_win is a stage ID (0xFFFF unset).
 * Pips show the game's own counts, as Slippi does. The host's room-settled
 * score is the sole rating authority. Never copy these fields into match state.
 */
typedef struct MuRankedSet {
    MuYgU8 max_games;
    MuYgU16 cur_game;
    MuYgU8 score[2], prev_winner, tiebreak_num, game_results[9];
    MuYgU16 last_stage_win[2];
    struct { MuYgU8 active, character, color; } color_ban;
    MuYgU8 last_end_mode;
} MuRankedSet;

/* Mutable access for track A; const access for score/badge consumers. Reset at
 * online-major entry/exit; A also resets when a fresh pair starts a new set. */
MuRankedSet* mu_ranked_set_mut(void);
const MuRankedSet* mu_ranked_set(void);
void mu_ranked_set_reset(void);
int mu_ranked_read_ranks(MuYgRanks* out);
int mu_ranked_read_policy(MuRankedPolicy* out);
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH && !defined(MU_NO_SLIPPI)
int mu_ranked_practice(void);
int mu_ranked_practice_allowed(void);
int mu_ranked_practice_control(unsigned action);
#else
static inline int mu_ranked_practice(void) { return 0; }
static inline int mu_ranked_practice_allowed(void) { return 0; }
static inline int mu_ranked_practice_control(unsigned action) { (void)action; return 0; }
#endif
#endif
