/* Included inside mncharsel.c, after its private CSS door types. */
#ifdef MELEE_DOUBLES
#include <mu_doubles_game.h>
/* The team each door last drew (0xFF: not yet drawn this CSS visit). */
static unsigned char mu_doubles_drawn[4];
/* Each door's HMN/CPU switch bounds while Doubles empties them (0, 0: no switch saved). */
static float mu_doubles_switch[4][2];
static void mu_doubles_css_frame(void)
{
    CSSData* css = mnCharSel_804D6CB0;
    int i;
    if (!mu_doubles_active() || !css || css->match_type != VS_MELEE) {
        /* The bounds are static: give every other mode its switches back. */
        for (i = 0; i < 4; ++i) {
            CSSDoor* door = &mnCharSel_803F0DFC.doors[i];
            if (mu_doubles_switch[i][0] == mu_doubles_switch[i][1]) continue;
            if (door->togglebtn_left == door->togglebtn_right) {
                door->togglebtn_left = mu_doubles_switch[i][0];
                door->togglebtn_right = mu_doubles_switch[i][1];
            }
            mu_doubles_switch[i][0] = mu_doubles_switch[i][1] = 0;
        }
        return;
    }
    mu_doubles_apply(&css->vs);
    /* Reassert immutable rules before and after CSS input, including the team toggle.
     * Each door keeps its own fighter/costume/cursor; no computer owns another door. */
    for (i = 0; i < 4; ++i) {
        CSSDoor* door = &mnCharSel_803F0DFC.doors[i];
        /* The retail team box still moves a door (and redraws it, costume included): draw it back too. */
        if (door->team != css->vs.start.players[i].team) mu_doubles_drawn[i] = 0xFF;
        door->team = css->vs.start.players[i].team;
        /* All four doors are players. A press on the HMN/CPU switch would make the door a CPU and, in the
         * same press, grab the CPU level slider beside it, which the reset below hides: the hand would hold
         * a slider no one can see. So the switch takes no presses here (empty bounds). */
        if (door->togglebtn_left != door->togglebtn_right) {
            mu_doubles_switch[i][0] = door->togglebtn_left;
            mu_doubles_switch[i][1] = door->togglebtn_right;
            door->togglebtn_left = door->togglebtn_right = 0;
        }
        /* A door that turned CPU anyway (its pad dropped) is drawn back as a player, and a hand left on a
         * level slider lets go of it. */
        if (door->p_kind != Gm_PKind_Human) mu_doubles_drawn[i] = 0xFF;
        door->p_kind = Gm_PKind_Human;
        if (door->p_kind_prev != Gm_PKind_Cpu) door->p_kind_prev = Gm_PKind_Human;
        if (door->is_hold_cpu_slider) {
            int c;
            for (c = 0; c < 4; ++c)
                if (mnCharSel_804A0BC0[c] && mnCharSel_804A0BC0[c]->x5 == 1 && mnCharSel_804A0BC0[c]->x6 == i + 4)
                    mnCharSel_804A0BC0[c]->x5 = 2;
            door->is_hold_cpu_slider = 0;
        }
        /* Setting the field doesn't recolour the door, its fighter or its token, so a door could
         * keep the colour it opened with. Redraw once per change, after the first
         * frame has built the doors, the way the team button does. */
        if (mu_friends_css_frames == 0) mu_doubles_drawn[i] = 0xFF;
        else if (mu_doubles_drawn[i] != door->team) {
            mu_doubles_drawn[i] = door->team;
            mnCharSel_8025DB34((u8) i);
        }
    }
}
#endif
