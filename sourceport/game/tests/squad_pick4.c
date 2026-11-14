/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
 *
 * The online Squad Strike row's four-fighter pick, run through the production character
 * select code (shim/mu_squad_css.c, extracted by squad_wiring_test.py into
 * mu_squad_pick4_fixture.h) against a small model of the screen: one door, its coin, the
 * cursor and the pad. Covers pick (A drops the coin, the first three become members and
 * the coin comes back, the fourth stays down), undo (B), clear (hold B), random without
 * duplicates (A on the random area), recolour (X/Y on a member) and that Z is never read.
 * A on a fighter already picked (marked on the grid) buzzes and never drops the coin there.
 * A pick the memory budget refuses (the fourth too) buzzes, comes back and notes why, and so does
 * one no hand dropped (a door opening on a coin, the game's random "?" placement). */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mu_squad_wire.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint32_t u32;
typedef float f32;

#include "mu_squad_pick4_types.h" /* GMSQUAD_* and GmSquadMember from include/mu_squad.h */

enum { ChKind_Max = 26, HSD_PAD_Z = 1, HSD_PAD_R = 2, HSD_PAD_L = 4, HSD_PAD_X = 8, HSD_PAD_Y = 16,
       HSD_PAD_A = 128, HSD_PAD_B = 256, SQUAD_CLEAR_HOLD = 90 };

GmSquadMember gmSquad_rows[GMSQUAD_PORTS][GMSQUAD_EXTRA];
u8 gmSquad_coin_pos[GMSQUAD_PORTS];
static bool s_init;
static void init(void)
{
    if (!s_init) {
        memset(gmSquad_rows, GMSQUAD_NONE, sizeof(gmSquad_rows));
        memset(gmSquad_coin_pos, GMSQUAD_NONE, sizeof(gmSquad_coin_pos));
        s_init = true;
    }
}

/* The screen. */
typedef struct { int ckind; u8 color; } PlayerInitData;
struct HSD_GObj { int unused; };
struct CSSCursorData { u8 x5, x6; f32 xC, x10; struct HSD_GObj* gobj; };
struct CSSCharModel { s8 x5; f32 x8, xC; struct HSD_GObj* gobj; };
typedef struct { u8 char_kind; s8 state; f32 bound_l, bound_u, bound_r, bound_d; } CSSIcon;
static struct HSD_GObj gobjs[8];
static struct CSSCursorData cursor0 = {1, 0, 0, 0, &gobjs[0]};
static struct CSSCursorData* mnCharSel_804A0BC0[4] = {&cursor0};
static struct CSSCharModel coin0 = {1, 0, 0, &gobjs[4]};
static struct CSSCharModel* mnCharSel_804A0BD0[4] = {&coin0};
static struct { struct { u8 sel_icon, sel_icon_prev, costume; } doors[4]; } mnCharSel_803F0DFC;
static u8 mnCharSel_804D6CF0, mnCharSel_804D6CF5 = 1;
static CSSIcon icons[25];
static PlayerInitData players[4];
static struct { u32 trigger, button; } HSD_PadCopyStatus[4];
static u8 squad_b_held[4];
static bool squad_b_owned[4];
static bool squad_b_down[4];
static bool squad_b_edge[4];
static u32 last_button; /* the status as the previous frame left it (B possibly hidden) */
static u8 squad_coin_holder[4];
static u8 squad_coin_seen[4];
static bool open_doors[4] = {true};
static bool budget_ok = true;
static int member_under = -1;
static u32 rolls[64];
static int roll_at;
static int buzzes, backs, moves, forwards, reports;
static u8 squad_note[4]; /* production's refusal note (SQUAD_NOTE_FRAMES comes with the types) */

static PlayerInitData* squad_player(int port) { return &players[port]; }
static bool squad_door_open(int port) { return port >= 0 && port < 4 && open_doors[port]; }
static bool squad_door_shows(int port, u8* ck, u8* color) { (void) port; (void) ck; (void) color; return false; }
static u8 squad_coin_color(int port)
{ return (u8) mu_squad_color((unsigned) players[port].ckind, mnCharSel_803F0DFC.doors[port].costume); }
static int squad_num_colors(u8 ck) { return ck >= 26 ? 1 : (int) mu_squad_colors(ck); }
static bool squad_budget(int coin_port, u8 coin_ck, u8 coin_color)
{ (void) coin_port; (void) coin_ck; (void) coin_color; return budget_ok; }
#ifndef FIXTURE_LOBBY
#define FIXTURE_LOBBY 0
#endif
static bool gmSquad_OnlineChosen(void) { return !FIXTURE_LOBBY; }
static bool gmSquad_LobbyChosen(void) { return FIXTURE_LOBBY; }
static bool gmSquad_OnlineEditable(void) { return true; }
static bool gmSquad_Any(void) /* production's: any door has a member (one door here) */
{ return gmSquad_rows[0][0].ckind != GMSQUAD_NONE; }
static void OSReport(const char* format, ...) { (void) format; ++reports; }

static u32 squad_random(int port, int salt) { (void) port; (void) salt; return rolls[roll_at++ % 64]; }
static int squad_member_under_cursor(int port, int* owner) { *owner = port; return member_under; }
static void squad_regrab(int door, int cport)
{   /* production's effects on the coin, the cursor and the holder (graphics left out) */
    mnCharSel_804A0BD0[door]->x5 = (s8) (cport + 1);
    mnCharSel_804A0BC0[cport]->x5 = 1;
    mnCharSel_804A0BC0[cport]->x6 = (u8) door;
    squad_coin_holder[door] = (u8) (cport + 1);
}
static void HSD_GObjGXLink_803909D8(struct HSD_GObj* a, struct HSD_GObj* b) { (void) a; (void) b; }
static void mnCharSel_8025DB34(u8 port) { players[port].color = mnCharSel_803F0DFC.doors[port].costume; }
static void lbAudioAx_800237A8(int a, int b, int c) { (void) a; (void) b; (void) c; }
static void lbAudioAx_80024030(int sound) { assert(sound == 3); ++buzzes; }
static void sfxBack(void) { ++backs; }
static void sfxMove(void) { ++moves; }
static void sfxForward(void) { ++forwards; }

#include "mu_squad_pick4_fixture.h"

/* One character select frame: the drop check, then the buttons (mnCharSel_SquadFrame's order). */
static void frame(u32 trigger, u32 button)
{   /* HSD_PadRenewCopyStatus: trigger = button & (last ^ button), with last = the status the
     * previous frame left, so a B the squad hid comes back as a new trigger. */
    HSD_PadCopyStatus[0].trigger = trigger | (button & HSD_PAD_B & (last_button ^ button));
    HSD_PadCopyStatus[0].button = button;
    squad_b_track();
    squad_fill_on_drop(0);
    squad_fill_buttons(0, &cursor0, HSD_PadCopyStatus[0].trigger);
    last_button = HSD_PadCopyStatus[0].button;
}
/* A frame where the squad's keys are not read (a search, a closed door): only the tracker runs. */
static void quiet_frame(u32 button)
{
    HSD_PadCopyStatus[0].trigger = button & (last_button ^ button);
    HSD_PadCopyStatus[0].button = button;
    squad_b_track();
    last_button = button;
}
/* The game's own A on a portrait: the coin goes down on that fighter in the colour X/Y chose. */
static void drop(u8 ck, u8 color)
{
    players[0].ckind = ck;
    mnCharSel_803F0DFC.doors[0].costume = color;
    players[0].color = color;
    coin0.x5 = 0;
    cursor0.x5 = 2;
    frame(HSD_PAD_A, HSD_PAD_A);
    frame(0, 0);
}
static void reset(void)
{
    init();
    gmSquad_Clear(0);
    players[0].ckind = 33; /* nobody yet */
    coin0.x5 = 1; cursor0.x5 = 1; cursor0.x6 = 0; cursor0.xC = 0.0f; cursor0.x10 = 20.0f;
    squad_coin_holder[0] = 1;
    memset(squad_coin_seen, GMSQUAD_NONE, sizeof squad_coin_seen);
    memset(squad_b_held, 0, sizeof squad_b_held);
    memset(squad_b_owned, 0, sizeof squad_b_owned);
    budget_ok = true; member_under = -1; roll_at = 0; last_button = 0;
    buzzes = backs = moves = forwards = reports = 0;
    memset(squad_note, 0, sizeof squad_note);
    frame(0, 0);
}
static int picks(void) { return gmSquad_Count(0) + (squad_coin_pick(0) != GMSQUAD_NONE); }
static bool member(int i, u8 ck, u8 color)
{ return gmSquad_rows[0][i].ckind == ck && gmSquad_rows[0][i].color == color; }

int main(void)
{
    int i, seed;
    _Static_assert(GMSQUAD_SIZE == 4 && GMSQUAD_EXTRA == 3, "four fighters: three members and the coin");
    /* Fighter i's portrait spans x i..i+1, y -4..0 (where a dropped coin lands, squad_hovered). */
    for (i = 0; i < 25; ++i) icons[i] = (CSSIcon){(u8) i, 1, (f32) i, 0, (f32) i + 1, -4};
    icons[24].state = 0; /* a locked fighter */

    /* Pick: A on a portrait. The first three drops become members in pick order, in the colour
     * shown, and the coin comes straight back to the hand; the fourth stays down. */
    reset();
    drop(2, 1);
    assert(member(0, 2, 1) && squad_coin_held(0) && cursor0.x5 == 1 && picks() == 1 && forwards == 1);
    drop(2, 0); /* already in the squad: refused, the coin comes back for another pick */
    assert(gmSquad_Count(0) == 1 && buzzes == 1 && squad_coin_held(0));
    drop(9, 7); /* a colour past the fighter's last plays as its last (G1's clamp) */
    assert(member(1, 9, (u8) (mu_squad_colors(9) - 1)) && squad_coin_held(0));
    drop(14, 2);
    assert(member(2, 14, 2) && squad_coin_held(0) && picks() == 3);
    drop(18, 3);
    assert(!squad_coin_held(0) && squad_coin_pick(0) == 18 && gmSquad_Count(0) == 3 && picks() == 4);
    assert(cursor0.x5 == 2); /* free pointer: the squad is complete */

    /* Undo: B takes the last pick back. The fourth (the coin) returns to the hand; then the
     * members go from the back. Every press is the squad's: no B for the screen. */
    frame(HSD_PAD_B, HSD_PAD_B);
    assert(squad_coin_held(0) && gmSquad_Count(0) == 3 && picks() == 3 && backs == 1);
    assert(!(HSD_PadCopyStatus[0].trigger & HSD_PAD_B) && !(HSD_PadCopyStatus[0].button & HSD_PAD_B));
    frame(0, 0);
    frame(HSD_PAD_B, HSD_PAD_B);
    assert(gmSquad_Count(0) == 2 && member(1, 9, (u8) (mu_squad_colors(9) - 1)) && backs == 2);
    assert(gmSquad_rows[0][2].ckind == GMSQUAD_NONE);
    frame(0, 0);
    drop(14, 2); /* re-pick after an undo lands in the freed place */
    assert(member(2, 14, 2) && squad_coin_held(0));
    frame(HSD_PAD_B, HSD_PAD_B); frame(0, 0);
    frame(HSD_PAD_B, HSD_PAD_B); frame(0, 0);
    frame(HSD_PAD_B, HSD_PAD_B); frame(0, 0);
    assert(picks() == 0 && backs == 5);
    /* With nothing picked B stays the screen's (its hold-B exit). */
    frame(HSD_PAD_B, HSD_PAD_B);
    assert(HSD_PadCopyStatus[0].button == HSD_PAD_B && !squad_b_owned[0] && backs == 5);
    frame(0, 0);

    /* Clear: holding B for 1.5 s drops every pick, once per hold, and the hold never reaches the
     * screen even after the squad is empty. */
    reset();
    drop(2, 0); drop(9, 0); drop(14, 0); drop(18, 0);
    assert(picks() == 4);
    frame(HSD_PAD_B, HSD_PAD_B); /* the press itself takes the fourth back */
    assert(picks() == 3);
    for (i = 2; i < SQUAD_CLEAR_HOLD; ++i) {
        frame(0, HSD_PAD_B);
        assert(picks() == 3 && !(HSD_PadCopyStatus[0].button & HSD_PAD_B));
    }
    frame(0, HSD_PAD_B);
    assert(squad_b_held[0] == SQUAD_CLEAR_HOLD && picks() == 0 && backs == 2);
    for (i = 0; i < 300; ++i) frame(0, HSD_PAD_B);
    assert(squad_b_held[0] == 0xFF && backs == 2 && !(HSD_PadCopyStatus[0].button & HSD_PAD_B));
    frame(0, 0);
    assert(!squad_b_held[0] && !squad_b_owned[0]);
    /* A hold with the fourth pick down and the pointer free clears the coin too. */
    drop(2, 0); drop(9, 0); drop(14, 0); drop(18, 0);
    squad_b_held[0] = SQUAD_CLEAR_HOLD - 1; squad_b_owned[0] = true;
    frame(0, HSD_PAD_B);
    assert(picks() == 0 && squad_coin_held(0));
    /* The same with the pointer resting on the coin's door (x5 0): only carrying keeps the coin. */
    reset();
    drop(2, 0); drop(9, 0); drop(14, 0); drop(18, 0);
    cursor0.x5 = 0;
    squad_b_held[0] = SQUAD_CLEAR_HOLD - 1; squad_b_owned[0] = true;
    frame(0, HSD_PAD_B);
    assert(picks() == 0 && squad_coin_held(0));

    /* A held B whose trigger bit repeats every frame (online, the edited status makes the pad
     * report it again) still takes back exactly one pick: the press is the hold's first frame. */
    reset();
    drop(2, 0); drop(9, 0); drop(14, 0); drop(18, 0);
    for (i = 0; i < 8; ++i) frame(HSD_PAD_B, HSD_PAD_B);
    assert(picks() == 3 && backs == 1);
    frame(0, 0);
    frame(HSD_PAD_B, HSD_PAD_B); frame(HSD_PAD_B, HSD_PAD_B);
    assert(picks() == 2 && backs == 2);
    frame(0, 0);
    /* A real tap: pressed one frame, held a few (the engine re-triggers the hidden B). */
    frame(HSD_PAD_B, HSD_PAD_B);
    for (i = 0; i < 5; ++i) frame(0, HSD_PAD_B);
    assert(picks() == 1 && backs == 3);
    frame(0, 0);
    /* A B pressed while the keys were not read (searching) is no press when they come back, and
     * that hold stays the screen's; a release nobody read still ends the hold. */
    quiet_frame(HSD_PAD_B); quiet_frame(HSD_PAD_B);
    for (i = 0; i < 3; ++i) frame(0, HSD_PAD_B);
    assert(picks() == 1 && backs == 3 && !squad_b_owned[0]);
    quiet_frame(0);
    frame(HSD_PAD_B, HSD_PAD_B);
    assert(picks() == 0 && backs == 4);
    frame(0, 0);

    /* B on a placed fourth pick works wherever the pointer is, unless it is carrying a coin. */
    reset();
    drop(2, 0); drop(9, 0); drop(14, 0); drop(18, 0);
    cursor0.x5 = 0;
    frame(HSD_PAD_B, HSD_PAD_B);
    assert(picks() == 3 && squad_coin_held(0) && backs == 1);
    frame(0, 0);

    /* The fourth pick runs the same budget check as the others: refused, the coin comes back to
     * the hand with a buzz and the door says why; nothing is left to trim after START. */
    reset();
    drop(2, 0); drop(9, 0); drop(14, 0);
    budget_ok = false;
    drop(18, 0);
    assert(picks() == 3 && squad_coin_held(0) && buzzes == 1 && cursor0.x5 == 1);
    assert(squad_note[0] == SQUAD_NOTE_FRAMES && reports == 1);
    budget_ok = true;
    drop(18, 0);
    assert(picks() == 4 && !squad_coin_held(0) && buzzes == 1);
    /* A member refused the same way notes it too. */
    reset();
    drop(2, 0);
    budget_ok = false;
    drop(9, 0);
    assert(picks() == 1 && squad_coin_held(0) && buzzes == 1 && squad_note[0] == SQUAD_NOTE_FRAMES);
    budget_ok = true;

    /* Random: A on the random area with the coin in hand. Never a fighter the squad already
     * has (members and the coin), never a locked one; the fourth press puts the coin down. */
    for (seed = 0; seed < 200; ++seed) {
        u8 seen[GMSQUAD_SIZE];
        int k, j;
        reset();
        for (i = 0; i < 64; ++i) rolls[i] = (u32) (seed * 2654435761u + (u32) i * 40503u) >> 7;
        if (seed & 1) drop((u8) (seed % 24), 0); /* sometimes one pick by hand first */
        cursor0.xC = 25.0f; cursor0.x10 = 4.0f; /* where the coin would land: the random area */
        for (k = 0; k < 4 && picks() < 4; ++k) {
            frame(HSD_PAD_A, HSD_PAD_A);
            assert(!(HSD_PadCopyStatus[0].trigger & HSD_PAD_A)); /* not the game's drop as well */
        }
        assert(picks() == 4 && !squad_coin_held(0) && buzzes == 0);
        for (j = 0; j < GMSQUAD_EXTRA; ++j) seen[j] = gmSquad_rows[0][j].ckind;
        seen[GMSQUAD_EXTRA] = squad_coin_pick(0);
        for (j = 0; j < GMSQUAD_SIZE; ++j) {
            assert(seen[j] < 24); /* never locked (24), never a non-fighter */
            for (k = j + 1; k < GMSQUAD_SIZE; ++k) assert(seen[j] != seen[k]);
        }
        frame(HSD_PAD_A, HSD_PAD_A); /* complete, the coin down: A is the game's again */
        assert(picks() == 4 && !buzzes && HSD_PadCopyStatus[0].trigger == HSD_PAD_A);
    }
    /* Every roll the same: the pool still never repeats a pick. */
    reset();
    memset(rolls, 0, sizeof rolls);
    cursor0.xC = -28.0f; cursor0.x10 = 4.0f; /* the left random area */
    for (i = 0; i < 4; ++i) frame(HSD_PAD_A, HSD_PAD_A);
    assert(member(0, 0, 0) && gmSquad_rows[0][1].ckind == 1 && gmSquad_rows[0][2].ckind == 2);
    assert(squad_coin_pick(0) == 3);
    /* The other door has fighter 0 first: Random doesn't mirror it while anyone else is left,
     * and takes it when nobody else is. A closed door is no rival. */
    reset();
    memset(rolls, 0, sizeof rolls);
    open_doors[1] = true; players[1].ckind = 0; mnCharSel_803F0DFC.doors[1].costume = 1;
    cursor0.xC = -28.0f; cursor0.x10 = 4.0f;
    frame(HSD_PAD_A, HSD_PAD_A);
    assert(gmSquad_rows[0][0].ckind == 23);
    reset();
    for (i = 0; i < 25; ++i) icons[i].state = i == 0 ? 1 : 0;
    cursor0.xC = -28.0f; cursor0.x10 = 4.0f;
    frame(HSD_PAD_A, HSD_PAD_A);
    assert(gmSquad_rows[0][0].ckind == 0);
    for (i = 0; i < 25; ++i) icons[i].state = i == 24 ? 0 : 1;
    open_doors[1] = false;
    reset();
    frame(HSD_PAD_A, HSD_PAD_A); /* off the random area: the game's own */
    reset();
    cursor0.xC = -28.0f; cursor0.x10 = 4.0f;
    frame(HSD_PAD_A, HSD_PAD_A);
    assert(gmSquad_rows[0][0].ckind == 0);
    /* Nobody left to pick: a buzz, nothing changes. */
    reset();
    for (i = 0; i < 25; ++i) icons[i].state = i < 3 ? 1 : 0;
    drop(0, 0); drop(1, 0); drop(2, 0);
    cursor0.xC = 25.0f; cursor0.x10 = 4.0f;
    frame(HSD_PAD_A, HSD_PAD_A);
    assert(picks() == 3 && buzzes == 1 && squad_coin_held(0));
    for (i = 0; i < 25; ++i) icons[i].state = i == 24 ? 0 : 1;
    /* Off the random area A is the game's own coin drop: untouched. */
    cursor0.xC = 0.0f; cursor0.x10 = 20.0f;
    frame(HSD_PAD_A, HSD_PAD_A);
    assert(HSD_PadCopyStatus[0].trigger == HSD_PAD_A && picks() == 3);

    /* A on a fighter already in the squad (marked on the grid): the error buzz, and the coin never
     * goes down there; the game's A never sees the press. A fighter not picked is the game's drop,
     * and an undo unmarks: A on the fighter it took back is the game's drop again. */
    reset();
    drop(2, 0); drop(9, 0);
    cursor0.xC = 2 - 2.2f; cursor0.x10 = 0.0f; /* the coin in hand over fighter 2's portrait */
    frame(HSD_PAD_A, HSD_PAD_A);
    assert(buzzes == 1 && !(HSD_PadCopyStatus[0].trigger & HSD_PAD_A) && squad_coin_held(0) && picks() == 2);
    cursor0.xC = 5 - 2.2f;
    frame(HSD_PAD_A, HSD_PAD_A);
    assert(buzzes == 1 && HSD_PadCopyStatus[0].trigger == HSD_PAD_A);
    frame(0, 0);
    frame(HSD_PAD_B, HSD_PAD_B); /* undo: 9 goes */
    frame(0, 0);
    assert(picks() == 1 && backs == 1);
    cursor0.xC = 9 - 2.2f;
    frame(HSD_PAD_A, HSD_PAD_A);
    assert(buzzes == 1 && HSD_PadCopyStatus[0].trigger == HSD_PAD_A);
    frame(0, 0);
    member_under = -1;

    /* Recolour: X/Y on one of your members, before or after the coin is down. */
    reset();
    drop(9, 0); drop(14, 0);
    member_under = 0;
    frame(HSD_PAD_X, HSD_PAD_X);
    assert(member(0, 9, 1) && moves == 1 && !(HSD_PadCopyStatus[0].trigger & HSD_PAD_X));
    frame(HSD_PAD_Y, HSD_PAD_Y);
    assert(member(0, 9, 0) && moves == 2 && !(HSD_PadCopyStatus[0].trigger & HSD_PAD_Y));
    frame(HSD_PAD_Y, HSD_PAD_Y); /* wraps to its last colour */
    assert(member(0, 9, (u8) (mu_squad_colors(9) - 1)));
    for (i = 0; i < 20; ++i) {
        frame(HSD_PAD_X, HSD_PAD_X);
        assert(gmSquad_rows[0][0].color < mu_squad_colors(9)); /* always a colour it has */
    }
    budget_ok = false; /* a costume that does not fit: refused, the colour stays */
    { u8 before = gmSquad_rows[0][0].color;
      frame(HSD_PAD_X, HSD_PAD_X);
      assert(gmSquad_rows[0][0].color == before && buzzes == 1); }
    budget_ok = true;
    member_under = -1; /* not on a member: X/Y stay the game's (the coin's own costume) */
    frame(HSD_PAD_X, HSD_PAD_X);
    assert(HSD_PadCopyStatus[0].trigger == HSD_PAD_X);

    /* Z is never the squad's: it is the online screen's disconnect. */
    reset();
    drop(2, 0); drop(9, 0);
    cursor0.xC = 25.0f; cursor0.x10 = 4.0f;
    member_under = 0;
    frame(HSD_PAD_Z, HSD_PAD_Z);
    assert(HSD_PadCopyStatus[0].trigger == HSD_PAD_Z && picks() == 2 && !buzzes && !backs && !moves);
    frame(HSD_PAD_R, HSD_PAD_R); /* nor R or L: the game's */
    frame(HSD_PAD_L, HSD_PAD_L);
    assert(picks() == 2 && !buzzes && !backs && !moves);

    /* A closed door reads nothing. */
    open_doors[0] = false;
    frame(HSD_PAD_B, HSD_PAD_B);
    assert(picks() == 2);
    open_doors[0] = true;

    /* A door that opens on a coin already down (the game's random placement when the door opens,
     * or a pick kept from before): no hand dropped it, so it gets the drop's budget check. Refused,
     * it comes back to the door's pointer with a buzz and the note; admitted, it stays. */
    reset();
    drop(2, 0); drop(9, 0); drop(14, 0);
    budget_ok = false;
    players[0].ckind = 18; mnCharSel_803F0DFC.doors[0].costume = 0;
    coin0.x5 = 0; cursor0.x5 = 2; squad_coin_holder[0] = 0; /* down, with no hand */
    squad_coin_seen[0] = GMSQUAD_NONE;                      /* the closed door forgot it */
    reports = 0;
    frame(0, 0);
    assert(squad_coin_held(0) && cursor0.x5 == 1 && cursor0.x6 == 0 && squad_coin_holder[0] == 1);
    assert(buzzes == 1 && squad_note[0] == SQUAD_NOTE_FRAMES && reports == 1 && picks() == 3);
    frame(0, 0);
    assert(buzzes == 1 && squad_coin_held(0));
    budget_ok = true;
    coin0.x5 = 0; cursor0.x5 = 2; squad_coin_holder[0] = 0; squad_coin_seen[0] = GMSQUAD_NONE;
    frame(0, 0);
    assert(!squad_coin_held(0) && picks() == 4 && buzzes == 1);
    frame(0, 0);
    assert(!squad_coin_held(0) && picks() == 4 && buzzes == 1); /* checked once, not every frame */
    /* The random "?": the game puts the coin on a fighter, or changes the fighter or colour under
     * a coin already down, with no hand. The new pick is checked; no free hand leaves it down,
     * refused once with the note, for the setup trim. */
    budget_ok = false; squad_note[0] = 0;
    players[0].ckind = 20; /* the "?" rolled again under the coin */
    cursor0.x5 = 1;        /* the only pointer is carrying: no free hand */
    frame(0, 0);
    assert(!squad_coin_held(0) && buzzes == 2 && squad_note[0] == SQUAD_NOTE_FRAMES);
    frame(0, 0);
    assert(!squad_coin_held(0) && buzzes == 2);
    cursor0.x5 = 2;
    mnCharSel_803F0DFC.doors[0].costume = 1; players[0].color = 1; /* a colour change counts too */
    frame(0, 0);
    assert(squad_coin_held(0) && buzzes == 3 && picks() == 3);
    budget_ok = true;
    /* Only a squad can refuse: with no members, a coin down on anyone stays. */
    reset();
    budget_ok = false;
    players[0].ckind = 18; coin0.x5 = 0; cursor0.x5 = 2; squad_coin_holder[0] = 0;
    frame(0, 0);
    assert(!squad_coin_held(0) && buzzes == 0);
    budget_ok = true;

    puts("squad four-fighter pick: pick, undo, clear, random without duplicates, recolour, no Z: PASS");
    puts("squad four-fighter pick: random does not mirror the other door: PASS");
    puts("squad four-fighter pick: a refused fourth pick comes back with a note: PASS");
    puts("squad four-fighter pick: a coin no hand dropped (door opening, random) gets the budget check: PASS");
    puts("squad four-fighter pick: A on a fighter already picked buzzes, undo unmarks it: PASS");
    return 0;
}
