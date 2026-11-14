/* Console Doubles' fixed VS rules and four human CSS doors. */
#ifndef MU_DOUBLES_GAME_H
#define MU_DOUBLES_GAME_H
#ifdef MELEE_DOUBLES
#include <melee/gm/types.h>
int mu_doubles_boot(void);
int mu_doubles_active(void);
int mu_doubles_take_menu_return(void);
void mu_doubles_enter_mode(int mode);
void mu_doubles_apply(VsModeData* vs);
/* The 4-peer route: the home's match plays on the Slippi Teams character select. team: this seat's
 * team from the room (0 or 1), anything else unknown. */
void mu_doubles_teams_enter(int team);
int mu_doubles_teams_css(void);
int mu_doubles_teams_team(void);
int mu_doubles_take_teams_return(void);
#endif
#endif
