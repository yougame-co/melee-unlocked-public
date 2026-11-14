/* Display rows are independent of Slippi matchmaking mode IDs. No disc assets. */
#ifndef MU_ONLINE_MENU_H
#define MU_ONLINE_MENU_H

enum { MU_YG_ROW_RANKED, MU_YG_ROW_UNRANKED,
    MU_YG_ROW_FRIENDS, MU_YG_ROW_SLIPPI
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    , MU_YG_ROW_SQUAD
#endif
};
#if MU_SQUAD
/* Display option only; wire matchmaking remains UNRANKED=1. The same value as mu_native.h: one past
 * the last option, so with Doubles built in it never shares Doubles' number. */
#ifndef MU_SLP_OPT_SQUAD
#ifdef MELEE_DOUBLES
#define MU_SLP_OPT_SQUAD 12
#else
#define MU_SLP_OPT_SQUAD 11
#endif
#endif
#ifdef MELEE_DOUBLES
_Static_assert(MU_SLP_OPT_SQUAD != MU_SLP_OPT_DOUBLES, "the Squad Strike row needs its own option");
#endif
#endif
static int tournament_offered; /* hello.tournament availability, refreshed by the native bridge */
static const int mu_online_top_rows[] = {
    MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_OPT_FRIENDS,
#ifdef MELEE_DOUBLES
    MU_SLP_OPT_DOUBLES,
#else
#if 0
    MU_SLP_OPT_DOUBLES,
#endif
#endif
    MU_SLP_OPT_TOURNAMENT,
    MU_SLP_OPT_SLIPPI,
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
#ifndef MU_NO_SLIPPI
    MU_SLP_OPT_SQUAD,
#endif
#endif
};
static const int mu_online_slippi_rows[] = { MU_SLP_MODE_DIRECT, MU_SLP_MODE_TEAMS, MU_SLP_MODE_PARTY };
static int mu_online_menu_count(int yougame, int submenu)
{
    return !yougame ? 8 : submenu ? sizeof mu_online_slippi_rows / sizeof *mu_online_slippi_rows :
                                  sizeof mu_online_top_rows / sizeof *mu_online_top_rows - !tournament_offered;
}
static int mu_online_menu_option(int yougame, int submenu, int row)
{
    if (row < 0 || row >= mu_online_menu_count(yougame, submenu)) return -1;
#ifdef MELEE_DOUBLES
    if (yougame && !submenu && !tournament_offered && row >= 4) ++row;
#else
    if (yougame && !submenu && !tournament_offered && row >= 3) ++row;
#endif
    return yougame ? (submenu ? mu_online_slippi_rows[row] : mu_online_top_rows[row]) : row;
}
static int mu_online_menu_row(int yougame, int submenu, int option)
{
    int row;
    for (row = 0; row < mu_online_menu_count(yougame, submenu); ++row)
        if (mu_online_menu_option(yougame, submenu, row) == option) return row;
    return 0;
}
/* Options, not display rows: the YouGame top-level modes remain available.
 * Local test tickets never reach Slippi; retain their mode rows for harness peering. */
static int mu_online_agent_locked(int agent, int yougame, int submenu, int option, int local_peer)
{
    if (!agent) return 0;
    if (option == MU_SLP_OPT_SLIPPI) return 1;
    if (yougame && !submenu) return 0;
    if (local_peer && option >= MU_SLP_MODE_RANKED && option <= MU_SLP_MODE_PARTY) return 0;
    return (option >= MU_SLP_MODE_RANKED && option <= MU_SLP_MODE_PARTY) ||
           option == MU_SLP_OPT_LOGIN || option == MU_SLP_OPT_UPDATE;
}
#endif
