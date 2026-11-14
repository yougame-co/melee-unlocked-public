/* A one-shot practice handoff must also finish when Training restarts itself. */
#include <assert.h>
static int practice_pending_mode = -1, practice_origin_mode = -1;
static int current_mode;
static int gm_GetCurrentGameMode(void) { return current_mode; }
/* The tournament and boot-match hooks entering a mode: not part of the handoff. */
static void mu_tourney_enter_mode(int mode) { (void)mode; }
static void mu_boot_match_enter_mode(int mode) { (void)mode; }
static int mu_boot_match_resolve(int requested) { return requested; }   /* no bootMatch pending */
#include "mu_practice_resolve_pending_mode_body.h"
#include "mu_practice_enter_mode_body.h"

int main(void)
{
    const int menu = 1, training = 0x1C, online = 8;
    for (int origin = 0; origin < 0x30; ++origin) {
        for (int destination = 0; destination < 0x30; ++destination) {
            current_mode = practice_origin_mode = origin;
            practice_pending_mode = destination;
            /* Origin decide callbacks cannot replace the accepted destination. */
            assert(mu_practice_resolve_pending_mode(menu) == destination);
            mu_practice_enter_mode(destination);
            current_mode = destination;
            /* Once entered, ordinary Back/quit choices belong to the player again. */
            assert(mu_practice_resolve_pending_mode(menu) == menu);
            assert(practice_pending_mode == -1 && practice_origin_mode == -1);
        }
    }
    current_mode = practice_origin_mode = training;
    practice_pending_mode = online;
    assert(mu_practice_resolve_pending_mode(training) == online);
    mu_practice_enter_mode(online);
    current_mode = online;
    assert(mu_practice_resolve_pending_mode(training) == training);
    return 0;
}
