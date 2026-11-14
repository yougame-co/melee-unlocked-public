/* Pre-save, C-only continuation. The game library requires x86-64 GCC. */
#ifdef MELEE_DOUBLES
#include "mu_shim.h"
static void** doubles_boot_context;
MU_EXCLUSIONS(doubles_boot, MU_EXCLUDE(doubles_boot_context))
void mu_doubles_set_restart(void (*restart)(void));
static void doubles_restart(void)
{
    if (!doubles_boot_context) __builtin_trap();
    __builtin_longjmp(doubles_boot_context, 1);
}
int mu_doubles_main(int (*game_main)(void))
{
    void* context[5];
    int resumed;
    uint32_t seed;
    void mu_apply_seed(uint32_t seed);
    doubles_boot_context = context;
    mu_doubles_set_restart(doubles_restart);
    resumed = __builtin_setjmp(context);
    if (!resumed) {
        if (mu_host->console_origin) mu_host->console_origin(64);
    } else if (mu_host->console_seed && mu_host->console_seed(&seed)) {
        mu_apply_seed(seed);
    }
    return game_main();
}
/* Keep the game's public entry and its field-layout checks unchanged. */
int mu_doubles_game_main(void);
int main(void) { return mu_doubles_main(mu_doubles_game_main); }
#endif
