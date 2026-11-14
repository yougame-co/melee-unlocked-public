/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#if MU_SQUAD
/* Squad Strike on character select (PC only; included by mncharsel.c, whose
 * statics it reads). See src/melee/gm/gmsquad.h.
 *
 *   - Drop your coin on a fighter as usual, and point at other portraits
 *     and press Z to add them (up to three). The squad fights in the order
 *     it was picked: Z picks made before the coin went down come first.
 *     Carrying a coin, the portrait previews the fighter: pick its colour
 *     with X/Y first, and Z adds it in that colour (Z again on a member
 *     already in the squad gives it the previewed colour). With a free
 *     pointer, Z on a member already in your squad changes its colour.
 *   - X / Y while pointing at a squad member's portrait changes that
 *     member's colour (elsewhere they change your coin pick's, as usual).
 *   - R, or D-pad up (offline only: online the D-pad is quick chat),
 *     removes the last fighter you added. Holding L for 1.5 s clears the
 *     whole squad.
 *   - Z on the random area beside the portraits (where a dropped coin picks
 *     at random) adds a random fighter not yet in the squad, coin in hand
 *     or not; with three added and your coin still in hand, the next press
 *     drops it on a random fighter, so four presses make a random squad.
 *   - A coin in your hand is no pick: the squad shows without it, and
 *     where you drop it is where it joins the order.
 *   - Holding a CPU's coin, Z builds that CPU's squad.
 *   - The error buzz: every other colour is taken, the squad is full, or
 *     there are more than two players (squads are for 1v1s).
 *
 * The online Squad Strike row (one door, squad_fill_mode) picks exactly
 * four fighters, in fight order:
 *   - A picks: the coin goes down on a fighter, which becomes the next
 *     pick, and the coin comes straight back to the hand for the next one.
 *     The fourth pick is where the coin stays down. X/Y recolour the
 *     portrait under the coin before A, and that colour is the pick's.
 *   - The picks show as the fighters' stock icons under the name tag, in
 *     pick order, with dim numbered squares for the places still empty;
 *     each picked fighter's portrait is darkened with its pick number, and
 *     A there gives the error buzz. The bottom line only names the keys
 *     (the full text lineup only when the icons could not load).
 *   - B removes the last pick; holding B (1.5 s) clears the squad. While
 *     there is a pick, B is never the screen's B (no unpick, and holding it
 *     never leaves character select).
 *   - A on the random area adds a random fighter the squad doesn't have.
 *   - No Z: on the online screen Z is disconnect (and cancels a search).
 *   - START searches only once all four are picked (mu_slippi_css.c).
 * Menus stay local; only the locked-in lineup travels (0xE3).
 *
 * Everything here runs on the synced pads in netplay, so both peers build
 * the same squads. */

int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);
#include <string.h>

#include <melee/gm/gm_1601.h>
#include "mu_squad.h"
#include <mu_lobby_wire.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbspdisplay.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/tobj.h>
int mu_console_active(void);

/* The online event CSS has one virtual door/cursor at 0, regardless of
 * the controller used to enter it. Offline/Friends retain their four doors. */
static bool squad_css_kind(void)
{
    return mnCharSel_804D6CB0->match_type == VS_MELEE ||
        (gmSquad_OnlineChosen() && mnCharSel_804D6CB0->match_type == 0xE);
}
static PlayerInitData* squad_player(int door)
{
    return &mnCharSel_804D6CB0->vs.start.players[
        gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : door];
}

static const char* const squad_names[ChKind_Max] = {
    "FALCON", "DK",     "FOX",   "GNW",     "KIRBY", "BOWSER", "LINK",
    "LUIGI",  "MARIO",  "MARTH", "MEWTWO",  "NESS",  "PEACH",  "PIKACHU",
    "ICS",    "PUFF",   "SAMUS", "YOSHI",   "ZELDA", "SHEIK",  "FALCO",
    "YLINK",  "DOC",    "ROY",   "PICHU",   "GANON",
};

/* Colour words per character and costume, from the costume file names
 * (PlFxOr.dat = Fox, orange); "" for the default. Game & Watch's are tints. */
static const char* const squad_colors[26][6] = {
    { "", "GRY", "RED", "WHT", "GRN", "BLU" }, /* Falcon */
    { "", "BLK", "RED", "BLU", "GRN" },        /* DK */
    { "", "ORG", "PUR", "GRN" },               /* Fox */
    { "", "RED", "BLU", "GRN" },               /* Game & Watch */
    { "", "YEL", "BLU", "RED", "GRN", "WHT" }, /* Kirby */
    { "", "RED", "BLU", "BLK" },               /* Bowser */
    { "", "RED", "BLU", "BLK", "WHT" },        /* Link */
    { "", "WHT", "CYN", "PNK" },               /* Luigi */
    { "", "YEL", "BLK", "BLU", "GRN" },        /* Mario */
    { "", "RED", "GRN", "BLK", "WHT" },        /* Marth */
    { "", "RED", "BLU", "GRN" },               /* Mewtwo */
    { "", "YEL", "BLU", "GRN" },               /* Ness */
    { "", "YEL", "WHT", "BLU", "GRN" },        /* Peach */
    { "", "RED", "BLU", "GRN" },               /* Pikachu */
    { "", "GRN", "ORG", "RED" },               /* Ice Climbers */
    { "", "RED", "BLU", "GRN", "YEL" },        /* Jigglypuff */
    { "", "PNK", "BLK", "GRN", "PUR" },        /* Samus */
    { "", "RED", "BLU", "YEL", "PNK", "CYN" }, /* Yoshi */
    { "", "RED", "BLU", "GRN", "WHT" },        /* Zelda */
    { "", "RED", "BLU", "GRN", "WHT" },        /* Sheik */
    { "", "RED", "BLU", "GRN" },               /* Falco */
    { "", "RED", "BLU", "WHT", "BLK" },        /* Young Link */
    { "", "RED", "BLU", "GRN", "BLK" },        /* Dr. Mario */
    { "", "RED", "BLU", "GRN", "YEL" },        /* Roy */
    { "", "RED", "BLU", "GRN" },               /* Pichu */
    { "", "RED", "BLU", "GRN", "PUR" },        /* Ganondorf */
};

static int squad_num_colors(u8 ck)
{
    int n = 0;
    if (ck >= 26) {
        return 1;
    }
    while (n < 6 && squad_colors[ck][n] != NULL) {
        n++;
    }
    return n;
}

/* The canvas character select draws its text on (set in mnCharSel_802640A0)
 * and our lineup banner per door. Both die with the screen. */
static u8 squad_l_held[4];
/* The online row: frames B has been held, and whether this hold is the squad's (it started while
 * there was a pick), so the screen's own B never sees it. */
static u8 squad_b_held[4];
static bool squad_b_owned[4];
/* B down at the start of last frame, and B newly down this frame (squad_b_track), seen every frame
 * even while the squad's keys are not read (searching, a closed door), so a press is one edge. */
static bool squad_b_down[4];
static bool squad_b_edge[4];
/* Per door, the coin's holder last frame (cursor index + 1), 0 while down. */
static u8 squad_coin_holder[4];
/* Per door, the fighter and colour its coin was down on last frame (squad_coin_key; GMSQUAD_NONE:
 * in a hand, none, or the door closed). A pick that appears or changes with no hand dropping it (a
 * door opening on a coin, the game placing a random fighter, its colour changed) is checked like a
 * drop. */
static u8 squad_coin_seen[4];
static int squad_canvas = -1;
static HSD_Text* squad_lobby_label;
static bool squad_heading_hidden;
extern void mu_squad_lobby_label_draw(void* object);
static void squad_lobby_heading(void)
{
    HSD_JObj* heading = NULL;
    if (!mu_lobby_css() || mnCharSel_804D6CC0 == NULL) return;
    lb_80011E24(mnCharSel_804D6CC0, &heading, 36, -1);
    if (heading) {
        if (gmSquad_LobbyChosen()) HSD_JObjSetFlagsAll(heading, JOBJ_HIDDEN);
        else if (squad_heading_hidden) HSD_JObjClearFlagsAll(heading, JOBJ_HIDDEN);
        squad_heading_hidden = gmSquad_LobbyChosen();
    }
    if (!squad_lobby_label && squad_canvas >= 0) {
        squad_lobby_label = HSD_SisLib_803A6754(0, squad_canvas);
        if (squad_lobby_label) {
            HSD_SisLib_803A6B98(squad_lobby_label, 0, 0, " ");
            squad_lobby_label->render_callback = mu_squad_lobby_label_draw;
        }
    }
}
static HSD_Text* squad_texts[4];
static u32 squad_shown[4];
/* The online row: its empty places (squad_slot_places) and the used-fighter marks on the
 * portrait grid (squad_grid_marks), on the same canvas; squad_marks_shown is the lineup the marks
 * show (0: redraw). */
static HSD_Text* squad_slot_texts[GMSQUAD_SIZE];
static HSD_Text* squad_marks[GMSQUAD_SIZE];
static u32 squad_marks_shown;

/* Stock icons from the game's own IfAll.dat, drawn by the CSS camera.
 * Fall back to the native text canvas if the icon model is unavailable. */
#define SQUAD_ICONS_MAX GMSQUAD_SIZE
static HSD_Archive* squad_icon_archive;
static DynamicModelDesc* squad_icon_model;
static HSD_GObj* squad_icons[4][SQUAD_ICONS_MAX];
static u32 squad_icons_shown[4];
/* squad_icons_load has run on this screen with Squad Strike on. */
static bool squad_icons_tried;
/* Per door, the hover squad_preview_member last handled. */
static u8 squad_seen_icon[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static f32 squad_icon_dx, squad_icon_dy, squad_icon_dz;
static f32 squad_icon_scale = 0.9f;
static f32 squad_icon_gap = 2.3f;

static void squad_icons_load(void)
{
    DISC_PTR(DynamicModelDesc)* scene_models = NULL;

    squad_icon_archive = NULL;
    squad_icon_model = NULL;
    if (mnCharSel_804D6CB0 == NULL ||
        !squad_css_kind() || !gmSquad_ModeAllowed() || mnCharSel_804D6CB0->vs.start.rules.is_teams)
    {
        return; /* tried again once Squad Strike is switched on */
    }
    squad_icons_tried = true;
    squad_icon_dx = squad_icon_dy = squad_icon_dz = 0.0f;
    squad_icon_scale = 0.9f;
    squad_icon_gap = 2.3f;
    /* Returns whether it was preloaded, not whether it loaded. */
    lbArchive_80016F80(&squad_icon_archive, "IfAll");
    if (squad_icon_archive == NULL) {
        squad_icon_archive = NULL;
        OSReport("squad: IfAll not loaded, using the text banner\n");
        return;
    }
    lbArchive_LoadSections(squad_icon_archive, (void**) &scene_models, "Stc_scemdls",
                           NULL);
    if (scene_models != NULL) {
        squad_icon_model = DP(scene_models[0]);
    }
    if (squad_icon_model == NULL) {
        OSReport("squad: no stock icon model, using the text banner\n");
    } else {
        /* Logged so a player's log tells a load failure from a row that could not be placed. */
        OSReport("squad: stock icons loaded\n");
    }
}

/* Online row: the lineup the last "squad: picks" line showed (count, then kind/colour). */
static u8 squad_logged[1 + 2 * GMSQUAD_SIZE];
static bool squad_logged_valid;
/* squadLineup: frames since the agent lineup was picked, while its START press is due; else -1. */
static int squad_agent_wait = -1;
/* Online row: this visit's opening coin check has run (mnCharSel_SquadFrame). */
static bool squad_open_checked;

static void mnCharSel_SquadReset(int canvas)
{
    int i, j;
    gmSquad_Disable();
    squad_logged_valid = false;
    squad_agent_wait = -1;
    squad_canvas = canvas;
    squad_lobby_label = NULL;
    squad_heading_hidden = false;
    for (i = 0; i < GMSQUAD_SIZE; i++) {
        squad_slot_texts[i] = NULL; /* freed with the last screen's canvas */
        squad_marks[i] = NULL;
    }
    squad_marks_shown = 0;
    for (i = 0; i < 4; i++) {
        squad_texts[i] = NULL;
        squad_shown[i] = 0;
        squad_icons_shown[i] = 0;
        squad_seen_icon[i] = 0xFF;
        for (j = 0; j < SQUAD_ICONS_MAX; j++) {
            squad_icons[i][j] = NULL; /* freed with the last screen */
        }
    }
    memset(squad_l_held, 0, sizeof(squad_l_held));
    memset(squad_b_held, 0, sizeof(squad_b_held));
    memset(squad_b_owned, 0, sizeof(squad_b_owned));
    memset(squad_b_down, 1, sizeof(squad_b_down)); /* a B held into the screen is no press */
    memset(squad_b_edge, 0, sizeof(squad_b_edge));
    memset(squad_coin_holder, 0, sizeof(squad_coin_holder));
    memset(squad_coin_seen, GMSQUAD_NONE, sizeof(squad_coin_seen));
    squad_icons_tried = false;
    squad_open_checked = false;
}

/* The character and colour a door's portrait shows right now. While a coin
 * is carried, that is the portrait under it (X/Y recolour it there too), not
 * the coin pick. Returns false if the door shows no character. */
static bool squad_door_shows(int port, u8* ck, u8* color)
{
    struct CSSDoor* door = &mnCharSel_803F0DFC.doors[port];
    if (door->sel_icon >= 0x19) {
        return false;
    }
    *ck = icons[door->sel_icon].char_kind;
    *color = door->costume;
    return *ck < 26;
}

/* A coin pick's colour as character select has it right now: the door's
 * costume when the door shows the coin pick (X/Y change it at once;
 * players[].color only follows on leaving), else the last colour the game
 * recorded. Always a colour that character has, so its stock icon is its
 * own (an out-of-range colour picked another character's icon frame): past
 * the fighter's last, its last (mu_squad_color, the Friends lobby's clamp). */
static u8 squad_coin_color(int port)
{
    PlayerInitData* pl = squad_player(port);
    u8 ck = (u8) pl->ckind;
    u8 shown_ck, shown_color;
    u8 color = pl->color;
    if (squad_door_shows(port, &shown_ck, &shown_color) && shown_ck == ck) {
        color = shown_color;
    }
    if (ck < 26) {
        color = mu_squad_color(ck, color);
    }
    return color;
}

static bool squad_door_open(int port)
{
    u8 type;
    if (gmSquad_OnlineChosen() && port != 0) return false;
    type = squad_player(port)->slot_type;
    return type == Gm_PKind_Human || type == Gm_PKind_Cpu;
}

/* The door's coin is in someone's hand. While it is, the game still keeps
 * the character it was last on in players[].ckind, but that is no longer a
 * pick: the squad has no coin fighter until the coin goes down again. */
static bool squad_coin_held(int port)
{
    return mnCharSel_804A0BD0[port] != NULL && mnCharSel_804A0BD0[port]->x5 != 0;
}

/* The door's coin pick, or GMSQUAD_NONE while it has none (coin in hand). */
static u8 squad_coin_pick(int port)
{
    u8 ck = (u8) squad_player(port)->ckind;
    return ck < 26 && !squad_coin_held(port) ? ck : GMSQUAD_NONE;
}

static int squad_players(void)
{
    int n = 0;
    int p;
    for (p = 0; p < 4; p++) {
        if (squad_door_open(p)) {
            n++;
        }
    }
    return n;
}

/* Another port has this character in this colour (coin pick or member). */
static bool squad_clash(int port, u8 ck, u8 color)
{
    if (gmSquad_LobbyChosen()) return false; /* the frozen setup resolves costume indices */
    int p, i;
    for (p = 0; p < 4; p++) {
        PlayerInitData* pl;
        if (p == port || !squad_door_open(p)) {
            continue;
        }
        pl = squad_player(p);
        if ((u8) pl->ckind == ck && squad_coin_color(p) == color) {
            return true;
        }
        for (i = 0; i < GMSQUAD_EXTRA; i++) {
            if (gmSquad_rows[p][i].ckind == ck &&
                gmSquad_rows[p][i].color == color)
            {
                return true;
            }
        }
    }
    return false;
}

/* Give a member the first colour from its own (+step) on that nobody else
 * has. Returns true if its colour changed; false if none is free. */
static bool squad_fix_color(int port, int index, int step)
{
    GmSquadMember* m = &gmSquad_rows[port][index];
    int n = squad_num_colors(m->ckind);
    int tries;
    if (m->ckind >= ChKind_Max) {
        return false;
    }
    for (tries = 0; tries < n; tries++) {
        u8 c = (u8) ((m->color + step + tries) % n);
        if (!squad_clash(port, m->ckind, c)) {
            if (c == m->color) {
                return false;
            }
            m->color = c;
            return true;
        }
    }
    return false;
}

static bool squad_budget(int coin_port, u8 coin_ck, u8 coin_color);

/* A pick the memory budget refused says so on its door for 3 s: "too big to load". Never a
 * silent trim after START: every pick (member, colour, the coin) is checked as it is made, with
 * the facts both machines share in Friends and online (mu_squad.c squad_fixed_budget). */
#define SQUAD_NOTE_FRAMES 180
static u8 squad_note[4];
/* A refusal note is up: the banners run for it even before any squad or icon exists (the very
 * first add refused offline). */
static bool squad_any_note(void)
{
    return squad_note[0] || squad_note[1] || squad_note[2] || squad_note[3];
}
static bool squad_fits(int target, int coin_port, u8 coin_ck, u8 coin_color)
{
    if (squad_budget(coin_port, coin_ck, coin_color)) return true;
    if (target >= 0 && target < 4) squad_note[target] = SQUAD_NOTE_FRAMES;
    OSReport("squad: P%d pick refused, too big to load\n", target + 1);
    return false;
}

/* Move a member to the next (dir 1) or previous (dir -1) colour nobody else
 * has. Returns false if no other colour is free. */
static bool squad_cycle_color(int port, int index, int dir)
{
    GmSquadMember* m = &gmSquad_rows[port][index];
    int n = squad_num_colors(m->ckind);
    int t;
    u8 old = m->color;
    if (m->ckind >= ChKind_Max) {
        return false;
    }
    for (t = 1; t < n; t++) {
        u8 c = (u8) (((int) m->color + dir * t + n * t) % n);
        if (!squad_clash(port, m->ckind, c)) {
            m->color = c;
            if (!squad_fits(port, -1, GMSQUAD_NONE, 0)) { m->color = old; return false; }
            return true;
        }
    }
    return false;
}

static bool squad_remove_last(int port)
{
    int i;
    for (i = GMSQUAD_EXTRA - 1; i >= 0; i--) {
        if (gmSquad_rows[port][i].ckind != GMSQUAD_NONE) {
            gmSquad_rows[port][i].ckind = GMSQUAD_NONE;
            gmSquad_rows[port][i].color = 0;
            return true;
        }
    }
    return false;
}

/* The portrait under the cursor's coin position, or -1. */
static int squad_hovered(struct CSSCursorData* cursor)
{
    /* Where a coin dropped from the pointer would land. */
    f32 x = cursor->xC + 2.7f;
    f32 y = cursor->x10 - 2.0f;
    int i;
    for (i = 0; i < 25; i++) {
        CSSIcon* icon = &icons[i];
        if (icon->state < 1) {
            continue;
        }
        if (x > icon->bound_l && x < icon->bound_r && y < icon->bound_u &&
            y > icon->bound_d)
        {
            return icon->char_kind;
        }
    }
    return -1;
}

/* Which squad member this port's free pointer is on: its own first, else a
 * CPU's (whose squad this player builds by holding the CPU's coin). Returns
 * the member's index and sets *owner to its port, or returns -1. */
static int squad_member_under_cursor(int port, int* owner)
{
    struct CSSCursorData* cursor;
    int ck, i, q;
    if (gmSquad_OnlineChosen() && !gmSquad_OnlineEditable()) return -1;
    if (port < 0 || port >= 4 || mnCharSel_804D6CB0 == NULL ||
        !squad_css_kind() || (mnCharSel_804D6CF5 == 1 && !gmSquad_OnlineChosen()) ||
        !gmSquad_ModeAllowed() || mnCharSel_804D6CB0->vs.start.rules.is_teams)
    {
        return -1;
    }
    cursor = mnCharSel_804A0BC0[port];
    if (cursor == NULL || cursor->x5 != 2) {
        return -1;
    }
    ck = squad_hovered(cursor);
    if (ck < 0) {
        return -1;
    }
    for (q = 0; q < 4; q++) {
        int who = (port + q) % 4; /* own squad first */
        if (who != port && mnCharSel_803F0DFC.doors[who].p_kind != 1) {
            continue; /* other humans recolour their own members */
        }
        for (i = 0; i < GMSQUAD_EXTRA; i++) {
            if (gmSquad_rows[who][i].ckind == (u8) ck) {
                *owner = who;
                return i;
            }
        }
    }
    return -1;
}

/* mnCharSel_CostumeChange asks: is this port's X/Y ours this frame? */
static bool mnCharSel_SquadTakesXY(int port)
{
    int owner;
    return squad_member_under_cursor(port, &owner) >= 0;
}

static bool squad_fill_mode(void);

static bool mnCharSel_SquadTakesLR(int port)
{
    if (gmSquad_OnlineChosen() && !gmSquad_OnlineEditable()) return false;
    if (squad_fill_mode()) return false; /* the online row's keys are A and B: L/R stay the game's */
    struct CSSCursorData* cursor;
    int target;
    if (!gmSquad_ModeAllowed() || port < 0 || port >= 4) return false;
    cursor = mnCharSel_804A0BC0[port];
    if (!cursor) return false;
    target = cursor->x5 == 1 ? cursor->x6 : port;
    return target >= 0 && target < 4 && gmSquad_Count(target) > 0 &&
           (HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].button & (HSD_PAD_L | HSD_PAD_R)) != 0 &&
           !(HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].trigger & (HSD_PAD_X | HSD_PAD_Y));
}

static bool squad_budget(int coin_port, u8 coin_ck, u8 coin_color)
{
    GmSquadMember members[GMSQUAD_PORTS * GMSQUAD_SIZE];
    int p, count = 0;
    if (gmSquad_LobbyChosen()) {
        const MuLobbyView* v = mu_lobby_read();
        if (!v || v->my_seat >= 4) return false;
        for (p = 0; p < GMSQUAD_PORTS; ++p) {
            const MuLobbySeat* seat = &v->seats[p];
            int i;
            if (seat->kind != MU_LOBBY_KIND_HUMAN) continue;
            if (p == v->my_seat) {
                count += gmSquad_Lineup(p, p == coin_port ? coin_ck : squad_coin_pick(p),
                    p == coin_port ? coin_color : squad_coin_color(p), &members[count]);
            } else for (i = 0; i < seat->lineup_count; ++i) {
                members[count].ckind = seat->lineup[i][0];
                members[count++].color = mu_squad_color(seat->lineup[i][0], seat->lineup[i][1]);
            }
        }
        return gmSquad_Budget(members, count, -1);
    }
    for (p = 0; p < GMSQUAD_PORTS; ++p) {
        if (!squad_door_open(p)) continue;
        count += gmSquad_Lineup(p, p == coin_port ? coin_ck : squad_coin_pick(p),
            p == coin_port ? coin_color : squad_coin_color(p), &members[count]);
    }
    return gmSquad_Budget(members, count, -1);
}

static void squad_add(int target, u8 ck)
{
    int i;
    u8 shown_ck, shown_color;
    /* Carrying a coin over the portrait, the door previews this fighter in
     * the colour X/Y chose: that is the colour Z adds it in. */
    bool preview = squad_door_shows(target, &shown_ck, &shown_color) &&
                   shown_ck == ck && shown_color < squad_num_colors(ck);

    if (!gmSquad_LobbyChosen() && squad_players() > 2) { lbAudioAx_80024030(3); return; }
    if (squad_coin_pick(target) == ck) {
        lbAudioAx_80024030(3); /* already your coin pick */
        return;
    }
    for (i = 0; i < GMSQUAD_EXTRA; i++) {
        if (gmSquad_rows[target][i].ckind == ck) {
            u8 old = gmSquad_rows[target][i].color;
            bool changed;
            if (preview && old != shown_color && !squad_clash(target, ck, shown_color)) {
                gmSquad_rows[target][i].color = shown_color;
                changed = true;
            } else changed = squad_fix_color(target, i, 1);
            if (changed && squad_fits(target, -1, GMSQUAD_NONE, 0)) sfxMove();
            else {
                gmSquad_rows[target][i].color = old;
                lbAudioAx_80024030(3);
            }
            return;
        }
    }
    for (i = 0; i < GMSQUAD_EXTRA; i++) {
        if (gmSquad_rows[target][i].ckind == GMSQUAD_NONE) {
            gmSquad_rows[target][i].ckind = ck;
            gmSquad_rows[target][i].color = preview ? shown_color : 0;
            squad_fix_color(target, i, 0);
            if (squad_clash(target, ck, gmSquad_rows[target][i].color)) {
                gmSquad_rows[target][i].ckind = GMSQUAD_NONE;
                lbAudioAx_80024030(3);
                return;
            }
            if (!squad_fits(target, -1, GMSQUAD_NONE, 0)) {
                gmSquad_rows[target][i].ckind = GMSQUAD_NONE;
                lbAudioAx_80024030(3);
                return;
            }
            sfxForward();
            return;
        }
    }
    lbAudioAx_80024030(3); /* squad full */
}

/* A new member in a given colour, at the next free slot. False (nothing
 * changes) if the squad already has that fighter or is full, the colour
 * clashes, or it would break the memory budget. */
static bool squad_add_member(int target, u8 ck, u8 color)
{
    int i;
    for (i = 0; i < GMSQUAD_EXTRA; i++) {
        if (gmSquad_rows[target][i].ckind == ck) return false;
    }
    for (i = 0; i < GMSQUAD_EXTRA; i++) {
        GmSquadMember* m = &gmSquad_rows[target][i];
        if (m->ckind != GMSQUAD_NONE) continue;
        m->ckind = ck;
        m->color = mu_squad_color(ck, color);
        squad_fix_color(target, i, 0);
        if (squad_clash(target, ck, m->color) || !squad_fits(target, -1, GMSQUAD_NONE, 0)) {
            m->ckind = GMSQUAD_NONE;
            m->color = 0;
            return false;
        }
        return true;
    }
    return false;
}

/* The online Squad Strike row: every coin drop fills the next of the four
 * slots, in fight order. The first three drops become members and the coin
 * comes straight back to the hand; the fourth stays down as the last pick. */
static bool squad_fill_mode(void)
{
    return gmSquad_LobbyChosen() || (gmSquad_OnlineChosen() && gmSquad_OnlineEditable());
}

/* Put the door's coin in cursor cport's hand, exactly as an A press on the
 * coin does (mnCharSel_Scene_OnFrame's pickup). */
static void squad_regrab(int door, int cport)
{
    struct CSSCharModel* coin = mnCharSel_804A0BD0[door];
    struct CSSCursorData* cursor;
    if (coin == NULL || cport < 0 || cport >= 4) return;
    cursor = mnCharSel_804A0BC0[cport];
    if (cursor == NULL) return;
    coin->x5 = (s8) (cport + 1);
    cursor->x5 = 1;
    cursor->x6 = (u8) door;
    mnCharSel_803F0DFC.doors[door].sel_icon = 0xD;
    HSD_GObjGXLink_803909D8(coin->gobj, cursor->gobj);
    cursor->xC = coin->x8 - 2.7f;
    cursor->x10 = coin->xC + 2.0f;
    squad_coin_holder[door] = (u8) (cport + 1);
}

/* The hand a refused coin that no hand dropped goes to: the door's own pointer, else the first
 * free one in port order (not carrying, not hidden); -1 if none (the setup trim still holds). */
static int squad_free_hand(int door)
{
    int i;
    for (i = -1; i < 4; ++i) {
        const int c = i < 0 ? door : i;
        struct CSSCursorData* cursor = c >= 0 && c < 4 ? mnCharSel_804A0BC0[c] : NULL;
        if (cursor != NULL && cursor->x5 != 1 && cursor->x5 != 3) return c;
    }
    return -1;
}

/* Runs once a frame per open door, before the buttons: a coin that went down
 * since last frame fills the next slot. A pick the coin takes with no hand dropping it (the door
 * opened on a coin already down; the game put the coin on a random fighter, the "?"; its colour
 * changed) gets the same budget check: refused, it comes back to a free hand with the note. */
static u8 squad_coin_key(int p)
{
    const u8 ck = squad_coin_pick(p);
    return ck == GMSQUAD_NONE ? GMSQUAD_NONE : (u8) (ck * 8 + (squad_coin_color(p) & 7));
}
static void squad_fill_on_drop(int p)
{
    struct CSSCharModel* coin = mnCharSel_804A0BD0[p];
    u8 was = squad_coin_holder[p];
    u8 seen = squad_coin_seen[p];
    u8 ck;
    int i;
    squad_coin_holder[p] = coin != NULL && coin->x5 > 0 ? (u8) coin->x5 : 0;
    ck = squad_coin_pick(p);
    squad_coin_seen[p] = squad_coin_key(p);
    if (was == 0 || squad_coin_holder[p] != 0) {
        if (squad_coin_holder[p] == 0 && ck != GMSQUAD_NONE && squad_coin_seen[p] != seen && gmSquad_Any() &&
            !squad_fits(p, -1, GMSQUAD_NONE, 0)) {
            const int hand = squad_free_hand(p);
            lbAudioAx_80024030(3);
            if (hand >= 0) {
                squad_regrab(p, hand);
                squad_coin_seen[p] = GMSQUAD_NONE;
            }
        }
        return;
    }
    if (ck == GMSQUAD_NONE) return;
    if (!squad_fill_mode()) {
        /* Offline and Friends: the game's own coin drop is a pick too. Onto squads that can't load
         * together it comes back to the hand, refused and said, never trimmed after START. */
        if (gmSquad_Any() && !squad_fits(p, -1, GMSQUAD_NONE, 0)) {
            lbAudioAx_80024030(3);
            squad_regrab(p, was - 1);
        }
        return;
    }
    for (i = 0; i < GMSQUAD_EXTRA; i++) {
        if (gmSquad_rows[p][i].ckind == ck) {
            /* Already in the squad: the coin comes back for another pick. */
            lbAudioAx_80024030(3);
            squad_regrab(p, was - 1);
            return;
        }
    }
    if (gmSquad_Count(p) >= GMSQUAD_EXTRA) {
        /* The fourth pick stays down, after the same budget check as the others. */
        if (!squad_fits(p, -1, GMSQUAD_NONE, 0)) {
            lbAudioAx_80024030(3);
            squad_regrab(p, was - 1);
        }
        return;
    }
    if (squad_add_member(p, ck, squad_coin_color(p))) sfxForward();
    else lbAudioAx_80024030(3);
    squad_regrab(p, was - 1);
}

/* Frames L (offline) or B (the online row) is held to clear a squad: 1.5 s. */
#define SQUAD_CLEAR_HOLD 90

/* The random area either side of the portraits' bottom row: a coin dropped
 * there picks at random (mncharsel.c, the A press). Same coordinates as
 * squad_hovered: where the coin would land. */
static bool squad_on_random(struct CSSCursorData* cursor)
{
    f32 x = cursor->xC + 2.7f;
    f32 y = cursor->x10 - 2.0f;
    return y < 6.0f && y > -1.0f &&
           ((x > -30.0f && x < -24.4f) || (x > 24.4f && x < 30.2f));
}

/* The same synced RNG stream as the game's random coin pick. */
static u32 squad_random(int port, int salt)
{
    return gmSquad_Random(port, salt);
}

/* The portrait icon of a fighter the CSS offers (unlocked), or -1. */
static int squad_icon_of(u8 ck)
{
    int icon;
    for (icon = 0; icon < 25; ++icon) {
        if (icons[icon].char_kind == ck) return icons[icon].state < 1 ? -1 : icon;
    }
    return -1;
}

/* Put the coin cursor carries down on fighter ck in colour, after the same clash
 * and budget checks as any pick. False (nothing changes) if either refuses. */
static bool squad_place_coin(int target, struct CSSCursorData* cursor, u8 ck, u8 color)
{
    struct CSSCharModel* coin_model = mnCharSel_804A0BD0[target];
    int icon = squad_icon_of(ck);
    if (coin_model == NULL || icon < 0 || color >= squad_num_colors(ck) ||
        squad_clash(target, ck, color) || !squad_fits(target, target, ck, color)) {
        return false;
    }
    /* Same coin placement as mnCharSel_8025FB50, after admission. sel_icon_prev too, or
     * mnCharSel_8025DB34 takes this as a new hover and resets the colour to the first free one. */
    squad_player(target)->ckind = ck;
    mnCharSel_803F0DFC.doors[target].sel_icon = (u8) icon;
    mnCharSel_803F0DFC.doors[target].sel_icon_prev = (u8) icon;
    mnCharSel_803F0DFC.doors[target].costume = color;
    coin_model->x5 = 0;
    coin_model->x8 = 3.4f + icons[icon].bound_l;
    coin_model->xC = -3.0f + icons[icon].bound_u;
    HSD_GObjGXLink_803909D8(coin_model->gobj,
        mnCharSel_804A0BC0[mnCharSel_804D6CF5 - 1]->gobj);
    mnCharSel_8025DB34((u8) target);
    cursor->x5 = 2;
    lbAudioAx_800237A8(0xB8, 0x7F, 0x40);
    return true;
}

/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Includes a fix from v1.0.1; the author's confirmation for that version is pending.
 * Who the player facing this door has at this place in their squad, so Random doesn't
 * mirror them. Offline only: online each side picks without seeing the other. */
static u8 squad_rival_at(int target, int pos)
{
    GmSquadMember line[GMSQUAD_SIZE];
    int p, n;
    for (p = 0; p < 4; p++) {
        if (p == target || !squad_door_open(p)) {
            continue;
        }
        n = gmSquad_Lineup(p, squad_coin_pick(p), squad_coin_color(p), line);
        return pos < n ? line[pos].ckind : GMSQUAD_NONE;
    }
    return GMSQUAD_NONE;
}

/* Z on the random area (A on the online row): a random fighter the squad
 * does not have yet joins it, coin in hand or not. With the member places
 * full and your own coin still in hand, the next press drops the coin on a
 * random fighter too, so four presses make a fully random squad. Never a
 * fighter the squad already has: the pool leaves out every member and the
 * coin pick. */
static void squad_random_pick(int target, struct CSSCursorData* cursor)
{
    u8 pool[25];
    u8 coin = squad_coin_pick(target);
    GmSquadMember line[GMSQUAD_SIZE];
    int pos = gmSquad_Lineup(target, coin, 0, line); /* where this pick goes */
    u8 rival = squad_rival_at(target, pos);
    int n = 0, i, k;

    for (i = 0; i < 25; i++) {
        u8 ck = icons[i].char_kind;
        bool have = icons[i].state < 1 || ck >= 26 || ck == coin;
        for (k = 0; k < GMSQUAD_EXTRA && !have; k++) {
            have = gmSquad_rows[target][k].ckind == ck;
        }
        if (!have) {
            pool[n++] = ck;
        }
    }
    if (n > 1) {
        for (i = 0; i < n; i++) {
            if (pool[i] == rival) {
                pool[i] = pool[--n];
                break;
            }
        }
    }
    if (n == 0) {
        lbAudioAx_80024030(3); /* nobody left */
        return;
    }
    if (gmSquad_Count(target) < GMSQUAD_EXTRA) {
        squad_add(target, pool[squad_random(target, gmSquad_Count(target)) % (u32) n]);
        return;
    }
    if (cursor->x5 == 1 && cursor->x6 == target && squad_coin_held(target)) {
        u8 ck = pool[squad_random(target, GMSQUAD_EXTRA) % (u32) n];
        u8 color = 0;
        while (color < squad_num_colors(ck) && squad_clash(target, ck, color)) ++color;
        if (!squad_place_coin(target, cursor, ck, color)) lbAudioAx_80024030(3);
        return;
    }
    lbAudioAx_80024030(3); /* squad full */
}

/* Pointing at one of your members with a free pointer: X/Y recolour it, like
 * the coin pick (mnCharSel_CostumeChange leaves these presses to us). True
 * when the press was the squad's. */
static bool squad_xy(int port, u32 pressed)
{
    int owner = port;
    int index;
    if (!(pressed & (HSD_PAD_X | HSD_PAD_Y))) return false;
    index = squad_member_under_cursor(port, &owner);
    if (index < 0) return false;
    HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].trigger &= ~(HSD_PAD_X | HSD_PAD_Y);
    if (squad_cycle_color(owner, index, (pressed & HSD_PAD_X) ? 1 : -1)) {
        sfxMove();
    } else {
        lbAudioAx_80024030(3);
    }
    return true;
}

/* The online row's keys (squad_fill_mode; one door, 0, on the local pad).
 * A on a portrait is the game's own coin drop, which squad_fill_on_drop
 * turns into the next pick. Here: B removes the last pick (the fourth, the
 * coin that is down, comes back to the hand; else the last member goes),
 * holding B clears every pick, A on the random area adds a random fighter
 * the squad doesn't have. B is always the squad's press here (the game's B
 * would put a carried coin back down on the last fighter, a pick nobody
 * made), and a hold that starts on a pick is the squad's to the end, so the
 * screen's hold-B exit (mnCharSel cursor think, B held 30 frames) only runs
 * from an empty squad. Z is never read: it is the online screen's
 * disconnect. */
static void squad_fill_buttons(int port, struct CSSCursorData* cursor, u32 pressed)
{
    const int pad = gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port;
    const bool picks = gmSquad_Count(port) > 0 || squad_coin_pick(port) != GMSQUAD_NONE;
    /* B's press is its edge from squad_b_track, never the pad's trigger bit: hiding B from .button
     * below makes the next frame's trigger (HSD_PadRenewCopyStatus derives it from the edited
     * status) report B as newly pressed again, so a tap undid every pick. */
    const bool b_press = squad_b_edge[port];
    squad_b_edge[port] = false;
    if (HSD_PadCopyStatus[pad].button & HSD_PAD_B) {
        if (b_press) squad_b_owned[port] = picks;
        if (squad_b_owned[port] && squad_b_held[port] < 0xFF) squad_b_held[port]++;
    }
    pressed = b_press ? (pressed | HSD_PAD_B) : (pressed & ~HSD_PAD_B);
    HSD_PadCopyStatus[pad].trigger &= ~HSD_PAD_B;
    if (squad_b_owned[port]) HSD_PadCopyStatus[pad].button &= ~HSD_PAD_B;
    if (!squad_door_open(port)) return;
    if (squad_b_owned[port] && squad_b_held[port] == SQUAD_CLEAR_HOLD) {
        /* Held for 1.5 s: every pick goes, once per hold. */
        if (squad_coin_pick(port) != GMSQUAD_NONE && cursor->x5 != 1) squad_regrab(port, port);
        if (gmSquad_Count(port) != 0 || squad_coin_pick(port) == GMSQUAD_NONE) {
            gmSquad_Clear(port);
            sfxBack();
        }
        return;
    }
    if (pressed & HSD_PAD_B) {
        if (squad_coin_pick(port) != GMSQUAD_NONE && cursor->x5 != 1) {
            /* The fourth pick, with the pointer anywhere but carrying: the coin comes back. */
            squad_regrab(port, port);
            sfxBack();
        } else if (squad_coin_pick(port) == GMSQUAD_NONE && squad_remove_last(port)) {
            sfxBack();
        }
        return;
    }
    if (squad_xy(port, pressed)) return;
    if ((pressed & HSD_PAD_A) && cursor->x5 == 1 && cursor->x6 == port && squad_coin_held(port)) {
        /* A on a fighter already in the squad (marked on the grid): the error buzz, and the coin
         * never goes down there (no drop, no announcer). squad_fill_on_drop still refuses one. */
        const int ck = squad_hovered(cursor);
        int i;
        for (i = 0; ck >= 0 && ck < 26 && i < GMSQUAD_EXTRA; i++) {
            if (gmSquad_rows[port][i].ckind == (u8) ck) {
                HSD_PadCopyStatus[pad].trigger &= ~HSD_PAD_A;
                lbAudioAx_80024030(3);
                return;
            }
        }
    }
    if ((pressed & HSD_PAD_A) && cursor->x5 == 1 && cursor->x6 == port &&
        squad_coin_held(port) && squad_on_random(cursor))
    {
        HSD_PadCopyStatus[pad].trigger &= ~HSD_PAD_A;
        if (!gmSquad_LobbyChosen() && squad_players() > 2) { lbAudioAx_80024030(3); return; }
        squad_random_pick(port, cursor);
    }
}

static void squad_buttons(int port)
{
    if (gmSquad_OnlineChosen() && !gmSquad_OnlineEditable()) return;
    struct CSSCursorData* cursor = mnCharSel_804A0BC0[port];
    u32 pressed;
    int target;

    if (cursor == NULL) {
        return;
    }
    pressed = HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].trigger;
    if (squad_fill_mode()) {
        squad_fill_buttons(port, cursor, pressed);
        return;
    }
    if (!(HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].button & HSD_PAD_L)) {
        squad_l_held[port] = 0;
    } else if (squad_l_held[port] < 0xFF) {
        squad_l_held[port]++;
    }
    if (squad_xy(port, pressed)) return;
    /* The initial L press starts the clear timer and belongs to the squad,
     * even though the clear action will not fire for another 89 bodies. */
    target = cursor->x5 == 1 ? cursor->x6 : port;
    if (target >= 0 && target < 4 && gmSquad_Count(target) > 0 &&
        (HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].button & HSD_PAD_L))
        HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].trigger &= ~HSD_PAD_L;
    if (!(pressed & (HSD_PAD_Z | HSD_PAD_R | HSD_PAD_DPADUP)) &&
        squad_l_held[port] != SQUAD_CLEAR_HOLD)
    {
        return;
    }
    if (cursor->x5 == 2) { /* free pointer: your own squad */
        target = port;
    } else if (cursor->x5 == 1 && cursor->x6 <= 3) {
        target = cursor->x6; /* holding a coin: that coin's squad */
    } else {
        return;
    }
    if (!squad_door_open(target)) {
        return;
    }
    /* Reserve handled presses for this frame. In particular, removing the
     * final member must not also cycle a skin or 20XX shield color later in
     * the same frame; Z must not also toggle a hidden portrait. Synced pads
     * are renewed normally on the next body. An empty squad keeps L/R and
     * the D-pad's existing behavior until the player explicitly picks Z. */
    if (gmSquad_Count(target) > 0) {
        u32 consumed = HSD_PAD_L | HSD_PAD_R;
        if (!mu_console_active() && !gmSquad_OnlineChosen()) consumed |= HSD_PAD_DPADUP;
        HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].trigger &= ~consumed;
    }
    if (squad_l_held[port] == SQUAD_CLEAR_HOLD) {
        /* Held for 1.5 s: the whole squad goes, once per hold. */
        if (gmSquad_Count(target) != 0) {
            gmSquad_Clear(target);
            sfxBack();
        }
        return;
    }
    /* Friends reserves the D-pad; removal there uses R only. */
    if ((pressed & HSD_PAD_R) || ((pressed & HSD_PAD_DPADUP) && !mu_console_active() && !gmSquad_OnlineChosen())) {
        if (gmSquad_Count(target) == 0) return;
        if (squad_remove_last(target)) {
            sfxBack();
        } else {
            lbAudioAx_80024030(3);
        }
        return;
    }
    if (pressed & HSD_PAD_DPADUP) {
        return;
    }
    if (!(pressed & HSD_PAD_Z)) return;
    if (squad_on_random(cursor)) {
        HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].trigger &= ~HSD_PAD_Z;
        if (!gmSquad_LobbyChosen() && squad_players() > 2) { lbAudioAx_80024030(3); return; }
        squad_random_pick(target, cursor);
        return;
    }
    {
        int ck = squad_hovered(cursor);
        if (ck >= 0 && ck < 26) {
            HSD_PadCopyStatus[gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : port].trigger &= ~HSD_PAD_Z;
            squad_add(target, (u8) ck);
        }
    }
}

/* The online row's banner when the stock icons are unavailable (squad_icons_load's text
 * fallback): four numbered slots in fight order and how many are filled.
 * "Pick 4 fighters 1/4: 1 FOX 2 - 3 - 4 -". */
static void squad_slots_line(int port, char* out, size_t size)
{
    GmSquadMember order[GMSQUAD_SIZE];
    int i, n, used;
    n = gmSquad_Lineup(port, squad_coin_pick(port), squad_coin_color(port), order);
    used = n >= GMSQUAD_SIZE ? snprintf(out, size, "Squad ready %d/%d:", n, GMSQUAD_SIZE)
                             : snprintf(out, size, "Pick %d fighters %d/%d:", GMSQUAD_SIZE, n, GMSQUAD_SIZE);
    for (i = 0; i < GMSQUAD_SIZE && used > 0 && used < (int) size; i++) {
        const char* name = i < n && order[i].ckind < 26 ? squad_names[order[i].ckind] : "-";
        int wrote = snprintf(out + used, size - (size_t) used, "  %d %s", i + 1, name);
        if (wrote < 0) return;
        used += wrote;
    }
}

static void squad_line(int port, char* out, size_t size)
{
    GmSquadMember order[GMSQUAD_SIZE];
    int i, n, used = 0;
    out[0] = '\0';
    if (port >= 0 && port < 4 && squad_note[port] && gmSquad_ModeAllowed()) {
        /* A refused pick (squad_fits): why, instead of the lineup, for SQUAD_NOTE_FRAMES. */
        snprintf(out, size, gmSquad_OnlineChosen() ? "Squad too big to load online: pick a lighter fighter."
                                                   : "Squads too big to load together: pick a lighter fighter.");
        return;
    }
    if ((gmSquad_OnlineChosen() || gmSquad_LobbyChosen()) && gmSquad_ModeAllowed()) {
        squad_slots_line(port, out, size);
        return;
    }
    if (!gmSquad_ModeAllowed() || gmSquad_Count(port) == 0) return;
    n = gmSquad_Lineup(port, squad_coin_pick(port), squad_coin_color(port), order);
    for (i = 0; i < n && used < (int) size; i++) {
        GmSquadMember* m = &order[i];
        const char* word;
        int wrote;
        if (m->ckind >= 26) continue;
        word = m->color < squad_num_colors(m->ckind) ?
               squad_colors[m->ckind][m->color] : "";
        wrote = snprintf(out + used, size - (size_t) used, "%s%s%s%s",
                         i ? " " : "", word, word[0] ? " " : "", squad_names[m->ckind]);
        if (wrote < 0) break;
        used += wrote;
    }
}

/* One stock icon, as the HUD makes them (ifStock_802F96D0), drawn by the
 * character select camera with the character models. */
/* y is a reference height (where the first version put the model, at
 * nametag + 2.2); rise is how far the icon's centre goes above the nametag's
 * centre. The first version's icons (scale 0.8) measured 10.1 units above
 * it, which pins the nametag's centre relative to the model whatever the
 * icon's offset inside the model is. With centre set, rise is unused and the
 * icon's own centre goes exactly to (x, y) (the online row, under the tag). */
static HSD_GObj* squad_icon_create(u8 ck, u8 color, f32 x, f32 y, f32 z,
                                   f32 rise, bool centre)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    HSD_JObj* icon = NULL;
    HSD_TObj* tobj;

    gobj = GObj_Create(4, 5, 0x80);
    if (gobj == NULL) {
        return NULL;
    }
    jobj = HSD_JObjLoadJoint(DP(squad_icon_model->joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 2, 0x80);
    gm_8016895C(jobj, squad_icon_model, 0);
    lb_80011E24(jobj, &icon, 1, -1);
    HSD_JObjSetTranslateX(jobj, x);
    HSD_JObjSetTranslateY(jobj, y);
    HSD_JObjSetTranslateZ(jobj, z);
    HSD_JObjSetScaleX(jobj, squad_icon_scale);
    HSD_JObjSetScaleY(jobj, squad_icon_scale);
    HSD_JObjSetScaleZ(jobj, squad_icon_scale);
    HSD_JObjSetFlagsAll(jobj, JOBJ_HIDDEN);
    HSD_JObjReqAnimAll(jobj, 0.0f);
    if (icon != NULL) {
        HSD_JObjClearFlags(icon, JOBJ_HIDDEN);
        if (icon->u.dobj != NULL && icon->u.dobj->mobj != NULL &&
            (tobj = icon->u.dobj->mobj->tobj) != NULL)
        {
            HSD_TObjReqAnimAll(tobj, gm_80168B34(ck, 0, color));
            if (tobj->aobj != NULL) {
                HSD_AObjSetRate(tobj->aobj, 0.0f);
            }
        }
    }
    HSD_JObjAnimAll(jobj);
    /* The icon sits off its model's origin (the HUD lays several out in a
     * row from one origin): move the model so the icon itself is centred on
     * (x, y). */
    if (icon != NULL) {
        Vec3 at;
        lb_8000B1CC(icon, NULL, &at);
        HSD_JObjSetTranslateX(jobj, x + (x - at.x));
        if (centre) {
            HSD_JObjSetTranslateY(jobj, y + (y - at.y));
        } else {
            f32 off = (at.y - y) / squad_icon_scale; /* icon within model */
            f32 tag_centre = y + off * 0.8f - 10.1f;
            HSD_JObjSetTranslateY(jobj, y + (tag_centre + rise - at.y));
        }
        HSD_JObjAnimAll(jobj);
    }
    return gobj;
}

/* The online row's places (one door, the 1P layout): a centre under the name
 * tag, where the username shows. The tag text box gives x and the tag's
 * bottom edge (text y runs down: world y = -pos_y, as vanilla places it from
 * the tag joint); the tag box joint gives the menu's depth, so the icons sit
 * just in front of it. False if the tag is not built yet. */
#define SQUAD_ROW_DROP 1.5f /* tag bottom to icon centre: icon half-height + a gap */
static bool squad_online_row_spot(f32* x, f32* y, f32* z)
{
    CSSTagData* tag = mnCharSel_803F0E8C[0].data;
    HSD_JObj* box = NULL;
    Vec3 at;
    if (tag == NULL || tag->text == NULL) {
        return false;
    }
    *x = tag->text->pos_x + tag->text->box_size_x * tag->text->font_size.x / 2.0f;
    *y = -(tag->text->pos_y + tag->text->box_size_y * tag->text->font_size.y) - SQUAD_ROW_DROP;
    *z = tag->text->pos_z + 0.5f;
    lb_80011E24(mnCharSel_804D6CC0, &box,
                mnCharSel_804D6CF5 == 1 ? mnCharSel_803F0EBC.tag_box_joint
                                        : mnCharSel_803F0E8C[0].name_jointl, -1);
    if (box != NULL) {
        lb_8000B1CC(box, NULL, &at);
        *z = at.z + 0.5f;
    }
    return true;
}

/* The online row's empty places: a dim square with the place's number, so
 * the four places read as four before anything is picked. Drawn on the
 * character select text canvas (link 1, after the menu, no depth test), so
 * nothing on the menu covers them; filled places hide theirs. */
#define SQUAD_SLOT_FONT 0.045f
#define SQUAD_SLOT_PX 44.0f /* 2 units at SQUAD_SLOT_FONT: one stock icon */
static void squad_slot_places(const u8* cks, int n, f32 x0, f32 gap, f32 cy)
{
    static GXColor dim = { 0x90, 0x90, 0x90, 0xFF };
    const f32 half = SQUAD_SLOT_PX * SQUAD_SLOT_FONT / 2.0f;
    int i;
    for (i = 0; i < GMSQUAD_SIZE; i++) {
        HSD_Text* text = squad_slot_texts[i];
        if (i >= n || cks[i] != GMSQUAD_NONE || squad_canvas < 0) {
            if (text != NULL) text->hidden = 1;
            continue;
        }
        if (text == NULL) {
            text = squad_slot_texts[i] = HSD_SisLib_803A6754(0, squad_canvas);
            if (text == NULL) continue;
            text->font_size.x = text->font_size.y = SQUAD_SLOT_FONT;
            text->box_size_x = text->box_size_y = SQUAD_SLOT_PX;
            text->default_fitting = 1;
            text->default_alignment = 1;
            text->bg_color.r = text->bg_color.g = text->bg_color.b = 0x18;
            text->bg_color.a = 0xA0;
            HSD_SisLib_803A6B98(text, SQUAD_SLOT_PX / 2.0f, 6.0f, "%d", i + 1);
            HSD_SisLib_803A74F0(text, 0, &dim);
        }
        text->pos_x = x0 + gap * i - half;
        text->pos_y = -(cy + half);
        text->pos_z = 0.0f;
        text->hidden = 0;
    }
}

/* A row of stock icons above door p's nametag: coin pick, then members.
 * The online row's four places go under its name tag instead. */
static bool squad_icon_row(int p, const MuLobbyView* v)
{
    u8 cks[SQUAD_ICONS_MAX];
    u8 colors[SQUAD_ICONS_MAX];
    int n = 0;
    int i;
    u32 key = 2166136261u;
    HSD_JObj* name_jobj = NULL;
    Vec3 pos;
    f32 x0;

    if (squad_icon_model == NULL && !squad_icons_tried) {
        /* The screen opened with Squad Strike off (switched on since, from
         * the in-game menu): load the icons now. */
        squad_icons_load();
    }
    if (squad_icon_model == NULL || mnCharSel_804D6CC0 == NULL) {
        return false;
    }
    if (gmSquad_LobbyChosen()) {
        if (v && p != v->my_seat) {
            const MuLobbySeat* seat = &v->seats[p];
            for (i = 0; i < seat->lineup_count; ++i) {
                cks[n] = seat->lineup[i][0]; colors[n++] = mu_squad_color(cks[i], seat->lineup[i][1]);
            }
        } else {
            GmSquadMember order[GMSQUAD_SIZE];
            const int count = gmSquad_Lineup(p, squad_coin_pick(p), squad_coin_color(p), order);
            for (i = 0; i < count; ++i) { cks[n] = order[i].ckind; colors[n++] = order[i].color; }
        }
    } else if (gmSquad_ModeAllowed() && (gmSquad_Count(p) != 0 || gmSquad_OnlineChosen())) {
        /* In fighting order, the coin pick among the others. */
        GmSquadMember order[GMSQUAD_SIZE];
        int count = gmSquad_Lineup(p, squad_coin_pick(p),
                                   squad_coin_color(p), order);
        for (i = 0; i < count && n < SQUAD_ICONS_MAX; i++) {
            cks[n] = order[i].ckind;
            colors[n] = order[i].color;
            n++;
        }
        /* The online row always shows four places: empty ones stay empty
         * in their own slot, so slots 1 to 4 never move. */
        while (gmSquad_OnlineChosen() && n < SQUAD_ICONS_MAX) {
            cks[n] = GMSQUAD_NONE;
            colors[n] = 0;
            n++;
        }
    }
    for (i = 0; i < n; i++) {
        key = (key ^ cks[i]) * 16777619u;
        key = (key ^ colors[i]) * 16777619u;
    }
    key = (key ^ (u32) n) * 16777619u;
    key = (key ^ mnCharSel_803F0DFC.doors[p].p_kind) * 16777619u;
    key = (key ^ gmMainLib_GetGameRules()->handicap) * 16777619u;
    key |= 1;
    if (key == squad_icons_shown[p]) {
        return true;
    }
    squad_icons_shown[p] = key;

    for (i = 0; i < SQUAD_ICONS_MAX; i++) {
        if (squad_icons[p][i] != NULL) {
            HSD_GObjFree(squad_icons[p][i]);
            squad_icons[p][i] = NULL;
        }
    }
    if (n == 0) {
        squad_slot_places(cks, 0, 0.0f, 0.0f, 0.0f);
        return true;
    }
    if (gmSquad_OnlineChosen()) {
        /* Pick order left to right, under the username. */
        f32 cx, cy, cz;
        if (!squad_online_row_spot(&cx, &cy, &cz)) {
            static int logged_spot;
            if (!logged_spot) {
                logged_spot = 1;
                OSReport("squad: name tag not built yet, picks shown as text for now\n");
            }
            squad_icons_shown[p] = 0; /* try again next frame */
            return false;
        }
        x0 = cx + squad_icon_dx - squad_icon_gap * (n - 1) / 2.0f;
        for (i = 0; i < n; i++) {
            if (cks[i] != GMSQUAD_NONE) {
                squad_icons[p][i] = squad_icon_create(cks[i], colors[i], x0 + squad_icon_gap * i,
                                                      cy + squad_icon_dy, cz + squad_icon_dz, 0.0f, true);
            }
        }
        squad_slot_places(cks, n, x0, squad_icon_gap, cy + squad_icon_dy);
        {
            static int logged_online;
            if (logged_online < 2) {
                logged_online++;
                OSReport("squad: online picks row under the name tag at (%.2f, %.2f, %.2f)\n", cx, cy, cz);
            }
        }
        return true;
    }

    /* The row sits just above the nametag (whose joint is at the tag's
     * left end, vertically centred), or above the CPU level / handicap
     * slider when the door shows one, so it covers as little of the
     * character art as it can. Measured on a 2560x1440 screen: 1 unit is
     * about 14 px of a 1456-wide frame. */
    lb_80011E24(mnCharSel_804D6CC0, &name_jobj,
                mnCharSel_803F0E8C[p].name_jointl, -1);
    if (name_jobj == NULL) {
        return false;
    }
    lb_8000B1CC(name_jobj, NULL, &pos);
    {
        f32 above = 2.4f; /* tag half-height + icon half-height + a gap */
        bool cpu = mnCharSel_803F0DFC.doors[p].p_kind == 1;
        bool handicap = gmMainLib_GetGameRules()->handicap != 0;
        if (cpu || handicap) {
            above += 4.2f; /* "CPU Level" / handicap slider */
        }
        if (cpu && handicap) {
            above += 2.6f; /* both sliders */
        }
        x0 = pos.x + 4.4f + squad_icon_dx - squad_icon_gap * (n - 1) / 2.0f;
        for (i = 0; i < n; i++) {
            if (cks[i] == GMSQUAD_NONE) {
                continue; /* no coin pick yet: leave its place empty */
            }
            squad_icons[p][i] = squad_icon_create(
                cks[i], colors[i], x0 + squad_icon_gap * i,
                pos.y + 2.2f, pos.z + 0.5f + squad_icon_dz,
                above + squad_icon_dy, false);
        }
    }
    {
        static int logged;
        if (logged < 4) {
            logged++;
            OSReport("squad: icons for door %d at nametag (%.2f, %.2f, %.2f)"
                     "\n",
                     p, pos.x, pos.y, pos.z);
        }
    }
    return true;
}

/* The online row marks every fighter already picked on the portrait grid: the portrait goes dark
 * and shows its pick number, so nobody tries the same fighter twice (A there buzzes,
 * squad_fill_buttons). Undo and clear take the marks back. Drawn on the text canvas over the
 * menu (link 1, no depth test), under the cursor and the coins (links 3 and 4). Display only. */
#define SQUAD_MARK_FONT 0.06f
static void squad_grid_marks(int p)
{
    static GXColor number = { 0xFF, 0xE0, 0x60, 0xFF };
    GmSquadMember order[GMSQUAD_SIZE];
    u32 key = 2166136261u;
    int i, n = 0;
    if (squad_canvas < 0) return;
    if (gmSquad_ModeAllowed() && (gmSquad_OnlineChosen() || gmSquad_LobbyChosen())) {
        n = gmSquad_Lineup(p, squad_coin_pick(p), squad_coin_color(p), order);
    }
    for (i = 0; i < n; i++) key = (key ^ order[i].ckind) * 16777619u;
    key = ((key ^ (u32) n) * 16777619u) | 1;
    if (key == squad_marks_shown) return;
    squad_marks_shown = key; /* cleared below if a mark could not be made: retry next frame */
    for (i = 0; i < GMSQUAD_SIZE; i++) {
        HSD_Text* text = squad_marks[i];
        const int icon = i < n && order[i].ckind < 26 ? squad_icon_of(order[i].ckind) : -1;
        f32 w, h;
        if (icon < 0) { /* no pick here, or a fighter with no portrait of its own */
            if (text != NULL) text->hidden = 1;
            continue;
        }
        if (text == NULL) {
            text = squad_marks[i] = HSD_SisLib_803A6754(0, squad_canvas);
            if (text == NULL) {
                squad_marks_shown = 0;
                continue;
            }
            text->font_size.x = text->font_size.y = SQUAD_MARK_FONT;
            text->default_fitting = 1;
            text->default_alignment = 1;
            text->bg_color.r = text->bg_color.g = text->bg_color.b = 0;
            text->bg_color.a = 0xA8;
            HSD_SisLib_803A6B98(text, 0.0f, 0.0f, "%d", i + 1);
            HSD_SisLib_803A74F0(text, 0, &number);
        }
        /* The portrait's box (its pick bounds, world units) in text units: y runs down. */
        w = icons[icon].bound_r - icons[icon].bound_l;
        h = icons[icon].bound_u - icons[icon].bound_d;
        text->pos_x = icons[icon].bound_l;
        text->pos_y = -icons[icon].bound_u;
        text->pos_z = 0.0f;
        text->box_size_x = w / SQUAD_MARK_FONT;
        text->box_size_y = h / SQUAD_MARK_FONT;
        HSD_SisLib_803A746C(text, 0, text->box_size_x / 2.0f, text->box_size_y / 2.0f - 16.0f);
        HSD_SisLib_803A70A0(text, 0, "%d", i + 1);
        text->hidden = 0;
    }
}

/* One banner per door, just under its nametag: "MARTH ORG FOX RED FALCO".
 * With stock icons available, the icons replace it. */
static void squad_banners(void)
{
    int p;
    for (p = 0; p < 4; p++) {
        if (squad_note[p]) --squad_note[p];
    }
    if (mu_lobby_css()) {
        const MuLobbyView* v = mu_lobby_read();
        for (p = 0; p < 4; ++p) squad_icon_row(p, v);
        if (v && v->my_seat < 4) squad_grid_marks(v->my_seat);
        return;
    }

    if (squad_canvas >= 0 && gmSquad_OnlineChosen()) {
        char line[128];
        HSD_Text* text = squad_texts[0];
        if (!text) {
            text = squad_texts[0] = HSD_SisLib_803A6754(0, squad_canvas);
            if (!text) return;
            text->pos_x = -27.0f;
            text->pos_y = 26.0f;
            text->pos_z = 0.0f;
            text->font_size.x = text->font_size.y = 0.055f;
            text->box_size_x = 900.0f;
            text->box_size_y = 40.0f;
            text->default_fitting = 1;
            text->default_alignment = 0;
            HSD_SisLib_803A6B98(text, 0.0f, 0.0f, "");
        }
        /* The picks as fighter icons under the name tag and used fighters marked on the grid, so
         * the bottom line is empty: the panel already says what START does. It shows only a
         * refused pick's reason, or the lineup as text if the icons couldn't load. */
        if (squad_icon_row(0, NULL) && !squad_note[0]) line[0] = '\0';
        else squad_line(0, line, sizeof line);
        squad_grid_marks(0);
        HSD_SisLib_803A70A0(text, 0, "%s", line);
        return;
    }
    if (squad_canvas < 0 || mnCharSel_804D6CF5 == 1) {
        return;
    }
    for (p = 0; p < 4; p++) {
        char line[128];
        u32 key;
        size_t k;
        HSD_Text* text = squad_texts[p];
        if (squad_icon_row(p, NULL) && !squad_note[p]) {
            /* Icons drawn: no text banner (a refusal note that just ended is wiped). */
            if (text != NULL && squad_shown[p] != 0) {
                text->bg_color.a = 0;
                HSD_SisLib_803A70A0(text, 0, "%s", "");
                squad_shown[p] = 0;
            }
            continue;
        }
        if (text == NULL) {
            CSSTagData* tag = mnCharSel_803F0E8C[p].data;
            if (tag == NULL || tag->text == NULL) {
                continue;
            }
            text = squad_texts[p] = HSD_SisLib_803A6754(0, squad_canvas);
            if (text == NULL) {
                continue;
            }
            text->pos_x = tag->text->pos_x - 1.86f;
            text->pos_y = tag->text->pos_y - 2.6f;
            text->pos_z = tag->text->pos_z;
            text->box_size_x = 288.89f;
            text->box_size_y = 36.0f;
            text->font_size.x = 0.045f;
            text->font_size.y = 0.045f;
            text->default_fitting = 1;
            text->default_alignment = 1;
            text->x4C = 1;
            text->bg_color.r = text->bg_color.g = text->bg_color.b = 0;
            text->bg_color.a = 0;
            HSD_SisLib_803A6B98(text, 144.44f, 0.0f, "");
            squad_shown[p] = 0;
        }
        squad_line(p, line, sizeof(line));
        key = 2166136261u;
        for (k = 0; line[k] != '\0'; k++) {
            key = (key ^ (u8) line[k]) * 16777619u;
        }
        key |= 1;
        if (key == squad_shown[p]) {
            continue;
        }
        squad_shown[p] = key;
        if (line[0] != '\0') {
            text->bg_color.r = text->bg_color.g = text->bg_color.b = 0x10;
            text->bg_color.a = 0xB8;
        } else {
            text->bg_color.a = 0;
        }
        HSD_SisLib_803A70A0(text, 0, "%s", line);
    }
}

/* Carrying a coin onto a fighter already in that squad: the portrait
 * previews the member in its own colour (the game starts every new hover on
 * the first free colour), so X/Y and Z carry on from it. Runs once per new
 * hover, the frame after the game takes it, so X/Y still work after. */
static void squad_preview_member(int p)
{
    struct CSSDoor* door = &mnCharSel_803F0DFC.doors[p];
    PlayerInitData* pl = squad_player(p);
    u8 ck, color;
    int i;

    if (door->sel_icon == squad_seen_icon[p] ||
        door->sel_icon != door->sel_icon_prev)
    {
        return; /* no new hover, or the game hasn't taken it yet */
    }
    squad_seen_icon[p] = door->sel_icon;
    if (!squad_door_shows(p, &ck, &color) || ck == (u8) pl->ckind) {
        return; /* nothing shown, or it's the coin pick */
    }
    for (i = 0; i < GMSQUAD_EXTRA; i++) {
        GmSquadMember* m = &gmSquad_rows[p][i];
        if (m->ckind == ck && m->color != color &&
            m->color < squad_num_colors(ck))
        {
            door->costume = m->color;
            mnCharSel_8025DB34((u8) p);
            return;
        }
    }
}

/* One stock per fighter of the biggest squad, as the match will give
 * everyone; the player's own stock count comes back when the last squad is
 * gone. */
static int saved_stocks = -1;
static int squad_applied_stocks = -1;

static void squad_restore_stocks(void)
{
    GameRules* rules = gmMainLib_GetGameRules();
    rules->stock_count = (u8) gmSquad_StockRule(false, rules->stock_count, 0,
        &saved_stocks, &squad_applied_stocks);
}

static void squad_sync_stocks(void)
{
    GameRules* rules = gmMainLib_GetGameRules();
    if (gmSquad_OnlineChosen() || mu_lobby_css()) return; /* online stocks come from exchanged lineups */
    bool active = gmSquad_ModeAllowed() &&
        !mnCharSel_804D6CB0->vs.start.rules.is_teams &&
        gmSquad_Any() && rules->mode == Mode_Stock;
    rules->stock_count = (u8) gmSquad_StockRule(active, rules->stock_count,
        gmSquad_StocksFor(), &saved_stocks, &squad_applied_stocks);
}

int gmSquad_CssPreview(u8* coin_ck, u8* coin_color, GmSquadMember out[GMSQUAD_SIZE]);

/* The online row's picks, logged when they change: "squad: picks P1 2/4 2:0,9:1" (the
 * controller port picking, how many of the four are picked, then kind:colour in fight order).
 * Kinds are CSS character kinds, as the lineup goes on the wire. */
static void squad_log_picks(void)
{
    GmSquadMember order[GMSQUAD_SIZE];
    u8 now[sizeof squad_logged], ck, color;
    char text[64];
    int i, n, used;
    memset(now, 0, sizeof now);
    n = gmSquad_CssPreview(&ck, &color, order);
    now[0] = (u8) n;
    for (i = 0; i < n; i++) {
        now[1 + 2 * i] = order[i].ckind;
        now[2 + 2 * i] = order[i].color;
    }
    if (squad_logged_valid && memcmp(now, squad_logged, sizeof now) == 0) return;
    memcpy(squad_logged, now, sizeof now);
    squad_logged_valid = true;
    used = snprintf(text, sizeof text, "squad: picks P%d %d/%d", (int) mnCharSel_804D6CF0 + 1, n,
                    GMSQUAD_SIZE);
    for (i = 0; i < n && used > 0 && used < (int) sizeof text; i++) {
        int wrote = snprintf(text + used, sizeof text - (size_t) used, "%s%u:%u", i ? "," : " ",
                             order[i].ckind, order[i].color);
        if (wrote < 0) break;
        used += wrote;
    }
    OSReport("%s\n", text);
}

/* squadLineup, the agent-test key (only agent-test hook hosts answer it): the host's lineup for
 * this visit is picked the way a player picks it, three members and then the coin, and START is
 * pressed once character select shows the fighter chosen and takes START (its entry delay is
 * over). Lock-in then runs the player's path, mu_slippi_css_inputs to gmSquad_OnlineSelect. A
 * press that does not lock in (chat open, say) is repeated a few times. Online fill mode only
 * (one door, door 0). */
#define SQUAD_AGENT_START_AFTER 10
#define SQUAD_AGENT_START_AGAIN 30
#define SQUAD_AGENT_START_PRESSES 3
#define SQUAD_AGENT_START_WAIT 180
static int squad_agent_presses, squad_agent_next;
static void squad_agent_lineup(void)
{
    struct CSSCursorData* cursor = mnCharSel_804A0BC0[0];
    MuSquadSide side;
    GmSquadMember order[GMSQUAD_SIZE];
    u8 ck, color;
    int i, n;
    if (!squad_fill_mode() || cursor == NULL || mnCharSel_804A0BD0[0] == NULL) return;
    if (squad_agent_wait < 0) {
        if (mnCharSel_804D6CF6 != 0 || !gmSquad_OnlineAgentLineup(&side)) return;
        squad_agent_presses = 0;
        squad_agent_next = SQUAD_AGENT_START_AFTER;
        gmSquad_Clear(0);
        if (!squad_coin_held(0)) squad_regrab(0, 0);
        for (i = 0; i < GMSQUAD_SIZE; i++) {
            const MuSquadFighter* m = &side.members[i];
            /* The last pick is where the coin goes down; the first three are members. */
            bool picked = i < GMSQUAD_EXTRA ?
                squad_icon_of(m->kind) >= 0 && squad_add_member(0, m->kind, m->color) &&
                    gmSquad_rows[0][i].color == m->color :
                squad_place_coin(0, cursor, m->kind, m->color);
            if (!picked) {
                gmSquad_Clear(0); /* no half lineup left behind */
                OSReport("squad: agent lineup refused at pick %d\n", i + 1);
                return;
            }
        }
        squad_agent_wait = 0;
        return;
    }
    if (gmSquad_OnlineSearchReady()) {
        squad_agent_wait = -1; /* locked in ("squad: locked in" said so) */
        return;
    }
    n = gmSquad_CssPreview(&ck, &color, order);
    ++squad_agent_wait;
    if (n == GMSQUAD_SIZE && mnCharSel_804D6CF7 != 0 && mnCharSel_804D6CF2 == 0 &&
        squad_agent_presses < SQUAD_AGENT_START_PRESSES && squad_agent_wait >= squad_agent_next) {
        /* Seen by HandleInputsOnCSS this frame: the scene's OnFrame runs before the gobj procs. */
        HSD_PadCopyStatus[(u8) mnCharSel_804D6CF0].trigger |= HSD_PAD_START;
        OSReport("squad: agent lineup START %d\n", ++squad_agent_presses);
        squad_agent_next = squad_agent_wait + SQUAD_AGENT_START_AGAIN;
    } else if (squad_agent_wait > SQUAD_AGENT_START_WAIT) {
        OSReport("squad: agent lineup not locked in, %d/%d fighters picked, START pressed %d times\n",
                 n, GMSQUAD_SIZE, squad_agent_presses);
        squad_agent_wait = -1;
    }
}

/* B's edge and release, from the pad status as the frame starts (before anything here hides B).
 * A hold that began while the squad's keys were not read is never the squad's press, and a
 * release nobody else saw still ends the hold. */
static void squad_b_track(void)
{
    int p;
    for (p = 0; p < 4; p++) {
        const int pad = gmSquad_OnlineChosen() ? mnCharSel_804D6CF0 : p;
        const bool down = (HSD_PadCopyStatus[pad].button & HSD_PAD_B) != 0;
        squad_b_edge[p] = down && !squad_b_down[p];
        squad_b_down[p] = down;
        if (!down) {
            squad_b_held[p] = 0;
            squad_b_owned[p] = false;
        }
    }
}

/* Runs at the top of mnCharSel_Scene_OnFrame, before the game hands this
 * frame's coin picks to the preloader. */
static void mnCharSel_SquadFrame(void)
{
    int p, i;

    if (mnCharSel_804D6CB0 == NULL) {
        return;
    }
    squad_b_track();
    if (mu_lobby_css()) {
        const MuLobbyView* v = mu_lobby_read();
        if (!gmSquad_LobbyChosen() || !v || v->my_seat >= 4) {
            if (squad_icons_tried) squad_banners();
            return;
        }
        p = v->my_seat;
        if (!(v->flags & MU_LOBBY_F_STARTED) && mnCharSel_804D6CF6 == 0) {
            squad_fill_on_drop(p);
            squad_buttons(p);
            gmSquad_TrackCoin(p, squad_coin_pick(p) != GMSQUAD_NONE);
        }
        squad_banners();
        return;
    }
    if (!squad_css_kind() || (mnCharSel_804D6CF5 == 1 && !gmSquad_OnlineChosen())) {
        return;
    }
    if (!gmSquad_ModeAllowed() || mnCharSel_804D6CB0->vs.start.rules.is_teams) {
        /* Leaving stock singles drops the lineup and restores the stock rule. */
        gmSquad_ClearAll();
        lbDvd_GetPreloadCacheScene()->game_cache.mu_squad_key = gmSquad_Any() ? gmSquad_Key() : 0;
        squad_sync_stocks();
        if (squad_icons_tried) squad_banners();
        return;
    }

    if (gmSquad_OnlineChosen() && !gmSquad_OnlineEditable()) {
        squad_log_picks();
        squad_banners();
        return;
    }
    /* A visit with no picks yet opens with the coin in the hand. Coming back to the screen, the
     * game sets the coin down on the character it kept (C. Falcon, kind 0, after a back-out),
     * which read as a first pick nobody made. A lineup carried into this visit keeps its coin. */
    if (gmSquad_OnlineChosen() && !squad_open_checked && mnCharSel_804A0BC0[0] != NULL &&
        mnCharSel_804A0BD0[0] != NULL) {
        bool members = false;
        squad_open_checked = true;
        for (i = 0; i < GMSQUAD_EXTRA; i++) members |= gmSquad_rows[0][i].ckind != GMSQUAD_NONE;
        if (!members && squad_coin_pick(0) != GMSQUAD_NONE) {
            squad_regrab(0, 0);
            OSReport("squad: visit opens with the coin in hand\n");
        }
    }
    if (gmSquad_OnlineChosen()) squad_agent_lineup();
    for (p = 0; p < 4; p++) {
        if (!squad_door_open(p)) {
            gmSquad_Clear(p); /* a closed door has no squad */
            squad_coin_seen[p] = GMSQUAD_NONE; /* opening on a coin is a new pick */
            continue;
        }
        squad_preview_member(p);
        if (mnCharSel_804D6CF6 == 0) {
            squad_fill_on_drop(p);
            squad_buttons(p);
        }
        /* Where the coin pick falls in the order fighters were picked. */
        gmSquad_TrackCoin(p, squad_coin_pick(p) != GMSQUAD_NONE);
    }

    if (squad_players() > 2 && gmSquad_Any()) {
        gmSquad_ClearAll(); /* a third player joined: squads are for 1v1s */
        lbAudioAx_80024030(3);
    }

    /* No two fighters may share a character and colour: when a coin pick or
     * another member moves onto a member's, that member's colour moves on. */
    for (p = 0; p < 4; p++) {
        for (i = 0; i < GMSQUAD_EXTRA; i++) {
            squad_fix_color(p, i, 0);
        }
    }

    lbDvd_GetPreloadCacheScene()->game_cache.mu_squad_key = gmSquad_Any() ? gmSquad_Key() : 0;

    squad_sync_stocks();
    if (gmSquad_OnlineChosen()) squad_log_picks();
    if (gmSquad_Any() || squad_icons_tried || gmSquad_OnlineChosen() || squad_any_note()) squad_banners();
}

/* A deliberate return to the main menu leaves normal VS rules behind.
 * Name/rules modals and travel to SSS retain the squad for this match. */
static void mnCharSel_SquadExit(void)
{
    /* Rules modals must edit the user's setting, including when it happens
     * to equal the squad count. Match setup assigns squad stocks itself. */
    squad_restore_stocks();
    if (mnCharSel_804D6CF6 == CSSPendingSceneChange_2) {
        gmSquad_ClearAll();
        gmSquad_Disable();
        squad_sync_stocks();
        lbDvd_GetPreloadCacheScene()->game_cache.mu_squad_key = 0;
    }
}

/* Called only at the checked Friends admission boundary, never probe captures.
 * Capture only reads the saved stock rule; both canonical CSS applies clear
 * their rows without changing the 256-byte MuCssState or its version. */
static void mnCharSel_SquadJoin(void)
{
    gmSquad_ClearAll();
    gmSquad_Disable();
    memset(squad_l_held, 0, sizeof(squad_l_held));
    memset(squad_b_held, 0, sizeof(squad_b_held));
    memset(squad_b_owned, 0, sizeof(squad_b_owned));
    memset(squad_b_down, 1, sizeof(squad_b_down)); /* a B held into the screen is no press */
    memset(squad_b_edge, 0, sizeof(squad_b_edge));
    memset(squad_seen_icon, 0xFF, sizeof(squad_seen_icon));
    memset(squad_icons_shown, 0, sizeof(squad_icons_shown));
    memset(squad_shown, 0, sizeof(squad_shown));
    squad_marks_shown = 0;
    memset(squad_note, 0, sizeof(squad_note));
    squad_sync_stocks();
    lbDvd_GetPreloadCacheScene()->game_cache.mu_squad_key = gmSquad_Any() ? gmSquad_Key() : 0;
    if (squad_icons_tried) squad_banners();
    OSReport("squad: cleared on join\n");
}

/* The live pick preview (shim/mu_slippi_css.c, display only): the online door's coin pick
 * (GMSQUAD_NONE while carried) and its colour, and the lineup as it stands. Returns how many
 * members out holds (0-4). */
int gmSquad_CssPreview(u8* coin_ck, u8* coin_color, GmSquadMember out[GMSQUAD_SIZE])
{
    *coin_ck = squad_coin_pick(0);
    *coin_color = squad_coin_color(0);
    return gmSquad_Lineup(0, *coin_ck, *coin_color, out);
}

/* For the css-state line (shim/mu_css_state.c): how many fighters the online Squad Strike CSS
 * has picked, or -1 when that CSS is not the one open. */
int gmSquad_CssPickCount(void)
{
    GmSquadMember lineup[GMSQUAD_SIZE];
    u8 ck, color;
    if (!gmSquad_OnlineChosen() || mnCharSel_804D6CB0 == NULL || !squad_css_kind()) return -1;
    return gmSquad_CssPreview(&ck, &color, lineup);
}

#endif
