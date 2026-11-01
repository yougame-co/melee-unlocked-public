/* Ranked setup scene adapter; GPL-3.0-or-later. A2 calls this from its screen.
 * Owns transport and ready handshake, never draws or reads pads. */
#ifndef MU_RANKED_SETUP_ONLINE_H
#define MU_RANKED_SETUP_ONLINE_H
#include <mu_ranked_setup_model.h>
const MuRankedSetupModel* mu_ranked_setup_model(void);
void mu_ranked_setup_begin(void);
void mu_ranked_setup_frame(void);
void mu_ranked_setup_end(void);
/* Submit a complete draft for the local turn. Returns 0 without advancing on
 * illegal choice or failed send. Timed-out drafts can be submitted with auto=1. */
/* A2 updates the draft after input changes; frame() uses it on local timeout.
 * NULL clears it. Draft is automatically ignored after the step advances. */
void mu_ranked_setup_set_draft(const MuRankedSetupChoice*);
int mu_ranked_setup_submit(const MuRankedSetupChoice*, int automatic);
/* Capture the finalized online winner before scene exit destroys fighter data. */
void mu_ranked_setup_game_result(int winner, unsigned end_mode, int disconnected);
int mu_ranked_setup_finish_game(int connected, MuYgU16 stage);
/* True only with paired host/game YOUGAME_AGENT_TEST_HOOK builds and rankedCpu. */
int mu_ranked_setup_cpu(void);
#endif
