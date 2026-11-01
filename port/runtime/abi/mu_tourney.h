/* CPU bracket launch/result wire shared by the native game and host. No test hook. */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_TOURNEY_H
#define MU_TOURNEY_H

enum { MU_TOURNEY_COMMAND = 0xEB, MU_TOURNEY_LAUNCH = 0, MU_TOURNEY_RESULT = 1,
       MU_TOURNEY_EXIT = 2, MU_TOURNEY_STATUS = 3, MU_TOURNEY_REPLY_CAPACITY = 4096,
       MU_TOURNEY_RESULT_SIZE = 8, MU_TOURNEY_QUIT_OP = 32, MU_TOURNEY_BOOT_OP = 33,
       MU_TOURNEY_TICK_OP = 34, MU_TOURNEY_MENU = 4, MU_TOURNEY_JOIN = 5, MU_TOURNEY_LEAVE = 6 };

/* MU computes the zero-based winner first (including its normal timeout rules).
 * Any undecidable game is a bracket loss for the human: CPU is side 2. */
static inline int mu_tourney_result_winner(int method, int winner)
{
    (void) method;
    return winner == 0 ? 1 : 2;
}

/* Exactly the singles online match rules (slippi_online.cpp's match-state block).
 * The player's choice is supplied only after CSS; no choice is made at launch. */
static inline void mu_tourney_game_info(unsigned char out[312], int stage, int human,
                                 int color, int nametag, int cpu, int level)
{
    static const unsigned char rules[96] = {
        0x32, 0x01, 0x86, 0x4C, 0xC3, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0x6E, 0, 0x1F,
        0, 0, 1, 0xE0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,
        0x3F, 0x80, 0, 0, 0x3F, 0x80, 0, 0, 0x3F, 0x80, 0, 0
    };
    static const unsigned char player[36] = {
        33, 3, 4, 0, 0, 0xFF, 0, 0, 9, 0, 0x78, 0, 0xC0, 0, 4, 1,
        0, 0, 0, 0, 0, 0, 0, 0, 0x3F, 0x80, 0, 0, 0x3F, 0x80, 0, 0, 0x3F, 0x80, 0, 0
    };
    int i, j;
    for (i = 0; i < 96; ++i) out[i] = rules[i];
    /* prepare_online_match_state's final Ranked overrides: no pause, items off. */
    out[2] |= 0x08;
    { static const unsigned char items[8] = {0xF8, 0, 0, 0, 0x0F, 0, 0, 0};
      for (i = 0; i < 8; ++i) out[0x23 + i] = items[i]; }
    out[14] = (unsigned char)(stage >> 8); out[15] = (unsigned char)stage;
    for (i = 0; i < 6; ++i) {
        unsigned char* p = out + 96 + i * 36;
        for (j = 0; j < 36; ++j) p[j] = player[j];
        p[4] = (unsigned char)i;
        if (i < 2) {
            p[0] = (unsigned char)(i == 0 ? human : cpu);
            p[1] = (unsigned char)(i == 0 ? 0 : 1); /* human / VS CPU */
            p[3] = (unsigned char)(i == 0 ? color : (cpu == human && color == 0 ? 1 : 0));
            p[10] = (unsigned char)(i == 0 ? nametag : 0x78);
            p[15] = (unsigned char)(i == 0 ? 1 : level);
        }
    }
}
#endif
