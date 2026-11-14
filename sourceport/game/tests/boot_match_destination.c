/* agent-test bootMatch: the main menu's own exit must not replace the VS it was asked for (Linux test PC,
 * 11-09: the request landed on the title screen and its attract demo instead). */
#include <assert.h>
#include <stddef.h>
enum { GM_TITLE = 0, GM_MENU = 1, GM_VS = 2 };
static int current_mode;
static void* boot_mode;
static int gm_GetCurrentGameMode(void) { return current_mode; }
#include "mu_boot_match_resolve_body.h"

int main(void)
{
    current_mode = GM_MENU;
    assert(mu_boot_match_resolve(GM_TITLE) == GM_TITLE);   /* no boot asked: the menu's own choice */
    boot_mode = &current_mode;
    assert(mu_boot_match_resolve(GM_TITLE) == GM_VS);      /* boot pending, the menu exiting */
    assert(mu_boot_match_resolve(GM_MENU) == GM_VS);
    current_mode = GM_VS;
    assert(mu_boot_match_resolve(GM_MENU) == GM_MENU);     /* in the match: its own decide rules */
    return 0;
}
