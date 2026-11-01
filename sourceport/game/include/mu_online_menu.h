/* Display rows are independent of Slippi matchmaking mode IDs. No disc assets. */
#ifndef MU_ONLINE_MENU_H
#define MU_ONLINE_MENU_H

enum { MU_YG_ROW_RANKED, MU_YG_ROW_UNRANKED, MU_YG_ROW_FRIENDS, MU_YG_ROW_SLIPPI };
static int tournament_offered; /* hello.tournament availability, refreshed by the native bridge */
static const int mu_online_top_rows[] = {
    MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_OPT_FRIENDS,
#if 0
    /* Show each here, between Friends and Slippi, only once its YouGame mode works. */
    MU_SLP_OPT_DOUBLES,
#endif
    MU_SLP_OPT_TOURNAMENT,
    MU_SLP_OPT_SLIPPI
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
    if (yougame && !submenu && !tournament_offered && row >= 3) ++row;
    return yougame ? (submenu ? mu_online_slippi_rows[row] : mu_online_top_rows[row]) : row;
}
static int mu_online_menu_row(int yougame, int submenu, int option)
{
    int row;
    for (row = 0; row < mu_online_menu_count(yougame, submenu); ++row)
        if (mu_online_menu_option(yougame, submenu, row) == option) return row;
    return 0;
}
#endif
