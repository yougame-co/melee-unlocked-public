/* Ranked game prep, GPL-3.0-or-later. Behaviour ported from project-slippi/
 * slippi-ssbm-c d7174ca, Scenes/Ranked/GameSetup.c and Game/Characters.c.
 * Pure, allocation-free C contract for A2. No scene, input, drawing or I/O.
 * All player indices are absolute seats 0/1; stage IDs are external Melee IDs.
 * The caller owns policy/set lifetimes. Never include this state in simulation.
 */
#ifndef MU_RANKED_SETUP_MODEL_H
#define MU_RANKED_SETUP_MODEL_H
#include <mu_ranked.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { MU_SETUP_COLOR, MU_SETUP_STRIKE, MU_SETUP_STAGE, MU_SETUP_CHARACTER };
enum { MU_SETUP_ACTIVE, MU_SETUP_COMPLETE, MU_SETUP_DISCONNECTED, MU_SETUP_INVALID };
enum { MU_SETUP_WAIT, MU_SETUP_LOCAL_TIMEOUT, MU_SETUP_REMOTE_TIMEOUT };
enum { MU_SETUP_NEXT_GAME, MU_SETUP_TIEBREAK, MU_SETUP_SET_OVER, MU_SETUP_SET_ABORTED };
typedef struct MuRankedFighter { MuYgU8 character, color; } MuRankedFighter;
typedef struct MuRankedSetupStep {
    MuYgU8 type, player, count, seconds;
} MuRankedSetupStep;
/* count = selected stages (0..2); character/color used only for character steps.
 * A2 keeps drafts separately; timeout fills missing stages in policy pool order.
 * stages[] use IDs, not selector indices. Unused packet bytes are zero. */
typedef struct MuRankedSetupChoice {
    MuYgU8 character, color, count;
    MuYgU16 stages[2];
} MuRankedSetupChoice;
typedef struct MuRankedSetupResult {
    MuYgU16 stage;
    MuRankedFighter fighters[2];
} MuRankedSetupResult;
typedef struct MuRankedSetupModel {
    MuRankedPolicy policy;
    MuRankedSet* set;
    MuRankedSetupStep steps[5];
    MuRankedSetupResult result;
    MuRankedFighter initial[2];
    MuYgU16 removed[5];
    MuYgU8 removed_count, step, step_count, local_player, status;
    MuYgU32 frames;
} MuRankedSetupModel;

/* Returns 0 and INVALID for unavailable/malformed policy or invalid selections.
 * Game 1's colour steps remain numbered 0/1 on the wire, but are skipped locally
 * on both seats unless the original choices collide. Tiebreak completes at once,
 * retaining the supplied last stage/characters and requiring B5 MERGE only. */
int mu_ranked_setup_model_init(MuRankedSetupModel*, const MuRankedPolicy*,
    MuRankedSet*, unsigned local_player, const MuRankedSetupResult* previous);
const MuRankedSetupStep* mu_ranked_setup_current(const MuRankedSetupModel*);
int mu_ranked_setup_stage_allowed(const MuRankedSetupModel*, MuYgU16 stage);
int mu_ranked_setup_color_allowed(const MuRankedSetupModel*, unsigned player,
    unsigned character, unsigned color);
unsigned mu_ranked_setup_color_count(unsigned character);
unsigned mu_ranked_setup_next_color(const MuRankedSetupModel*, unsigned player,
    unsigned character, unsigned color, int direction);
/* Build a C0 payload (5 bytes: step,char,color,stage,stage). No mutation. NULL
 * draft uses the previous character/color. timeout=1 completes a partial draft;
 * timeout=0 requires a complete legal choice. Local sends C0 before applying it.
 * Remote application uses the same validation. Bad/stale/out-of-turn packets
 * return 0 without mutation; transport must abort on an invalid remote packet. */
int mu_ranked_setup_packet(const MuRankedSetupModel*, const MuRankedSetupChoice* draft,
    int timeout, MuYgU8 packet[5]);
int mu_ranked_setup_apply(MuRankedSetupModel*, unsigned player, const MuYgU8 packet[5]);
/* C1 response: found,skip,char,color,stage,stage. A skip deterministically
 * auto-picks as in HandleOpponentStep. Returns 0 if absent/invalid. */
int mu_ranked_setup_remote(MuRankedSetupModel*, const MuYgU8 response[6]);
/* One display frame, 60 Hz. Local timeout after seconds+grace; remote timeout
 * after seconds+grace+wait, strictly > as in GameSetup.c:848,1366. Caller commits
 * local timeout through packet/apply, or disconnects on remote timeout. */
int mu_ranked_setup_tick(MuRankedSetupModel*);
void mu_ranked_setup_disconnect(MuRankedSetupModel*);
/* Pure Bo3 bookkeeping (main.asm:650-710). winner=-1 is a tie/exit replay;
 * disconnected overrides any winner. Does not advance game index for ties. */
int mu_ranked_setup_record_game(MuRankedSet*, int winner, unsigned end_mode,
    MuYgU16 stage, int disconnected);
#ifdef __cplusplus
}
#endif
#endif
