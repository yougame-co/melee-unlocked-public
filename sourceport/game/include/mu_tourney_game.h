/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_TOURNEY_GAME_H
#define MU_TOURNEY_GAME_H
int mu_tourney_boot(void);
int mu_tourney_playing(void);
void mu_tourney_tick(void);
void mu_tourney_poll(void);
void mu_tourney_enter_mode(int mode);
void mu_tourney_report(int method);
void mu_tourney_quit(void);
#endif
