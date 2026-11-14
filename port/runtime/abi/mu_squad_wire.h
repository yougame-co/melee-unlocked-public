/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#ifndef MU_SQUAD_WIRE_H
#define MU_SQUAD_WIRE_H
#if MU_SQUAD
#include <stddef.h>
#include <string.h>

#define MU_SQUAD_TEST_SETUP 36 /* harness-only practice operation: count, kind/color pairs */
#define MU_SQUAD_CAP 0x10
/* MU_BUILD caps bit, friend rooms only: this game joins Squad Strike when the friend picked it
 * (its client turns squads on as soon as the friend's build arrives with MU_SQUAD_CAP). Without
 * either bit the inviter's game says the friend's game can't play it; a later build that drops
 * both bits is a refusal (that game can't load Squad Strike). 0x20, 0x40 are taken. */
#define MU_SQUAD_FRIEND_CAP 0x80
#define MU_SQUAD_COMMAND 0xEC /* private game/host ABI, never a YouGame bridge key */
/* Friend rooms: {op, action}, 1 byte back.
 * QUERY: does this friend room play Squad Strike? MU_SQUAD_ROOM_PLAIN, MU_SQUAD_ROOM_SQUAD (our
 *   pick for this room, or the friend's pick), or MU_SQUAD_ROOM_WAITING (the friend's build has
 *   not arrived yet, so nobody may lock in: it may still turn the room into Squad Strike).
 * INVITE: "Play a friend" on the Squad Strike row. The host opens the friend picker and ties
 *   Squad Strike to the room that picker returns (1 = opening).
 * REFUSE: this game can't load Squad Strike (memory, disc identity): tell the friend now
 *   (1 = told).
 * INTENT: 1 while the "Play a friend" picker is still opening or open; 0 once it closed without
 *   a room, or any other room was joined. */
#define MU_SQUAD_OP_FRIEND 11
#define MU_SQUAD_FRIEND_QUERY 0
#define MU_SQUAD_FRIEND_INVITE 1
#define MU_SQUAD_FRIEND_REFUSE 2
#define MU_SQUAD_FRIEND_INTENT 3
#define MU_SQUAD_ROOM_PLAIN 0
#define MU_SQUAD_ROOM_SQUAD 1
#define MU_SQUAD_ROOM_WAITING 2
/* Next match's replay policy, sent before Game Start: {op, squad_live (0 or 1)}.
 * Host-local only; never sent to a peer or appended to the replay stream. */
#define MU_SQUAD_OP_REPLAY_MATCH 8
/* Agent test lineup query, sent with no other bytes each time the online squad CSS opens. The
 * answer is 2 * MU_SQUAD_LINEUP bytes {kind, color} in fight order, or nothing. Host-local only:
 * answered before Slippi dispatch, never sent to a peer. Only agent-test hook builds answer. */
#define MU_SQUAD_OP_AGENT_LINEUP 6
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
/* Harness observations only; private host ABI, never passed to Slippi dispatch. */
#define MU_SQUAD_OP_DIGEST_FRAME 9
#define MU_SQUAD_OP_DIGEST_END 10
/* Friends' synced select (a console session): costumes and hats are charged at their fighter's
 * fixed size there too, so while squads are in play the host opens the standard copy for a skin
 * over it, as in an online squad match. {op, OFF or ARM}, 1 byte back: OFF 1; ARM 0 the host has
 * no facts yet (send them, then ARM again), 1 armed, 2 the disc's own copy of a costume is over its
 * size, 3 a mod's (Friends squads are then refused). {op, FACT, use, kind (4), bytes (4), name NUL}:
 * one costume or hat fact, laid out as op 3's from its second byte. Host-local, never sent to a
 * peer. */
/* Frozen Friends setup mode, one byte back; independent of the select's last choice. */
#define MU_SQUAD_OP_LOBBY_MODE 13
#define MU_SQUAD_OP_CONSOLE 12
#define MU_SQUAD_CONSOLE_OFF 0
#define MU_SQUAD_CONSOLE_ARM 1
#define MU_SQUAD_CONSOLE_FACT 3
/* Version 3: every side is exactly MU_SQUAD_LINEUP = 4 fighters (version 2 was exactly 3, version 1
 * allowed 1 to 4). Another version's 0xE3 is refused (mu_squad_decode), and the squad budget identity
 * names the lineup size too (slippi_online.cpp, op 2), so a 3-fighter build never shares a pool. */
#define MU_SQUAD_VERSION 3
#define MU_SQUAD_LINEUP 4
#define MU_SQUAD_WIRE_MAX (4 + 4 * MU_SQUAD_LINEUP) /* excludes the 0xE3 message id */
/* Online admission floors (also the synced Friends select's, mu_squad.c squad_fixed_budget).
 * AllM is the doubled main heap every native host boots with (mu_squad_main_heap: both measured
 * 40 MiB MEM1 layouts, source_host.cpp, main_source.cpp and linux host_services.h, grow
 * lbheap.c's 0x64B400 to 0xC96800). The original 0x64B400 held about four fighters' files: on the
 * vanilla disc it trimmed 99.9% of random 4v4 squads and 60-98% of common competitive ones,
 * depending on the stage (each fighter's main file, effect file and costume constant, plus the
 * stage). At 0xC96800 the worst 4v4 needs 10806304 bytes with the select's largest-stage
 * reservation, 2.3 MB under it.
 * AllA covers the worst vanilla 4v4 by animation over every fighter (Kirby, Link, Marth, Ness vs
 * Peach, Zelda with Sheik's animations, Young Link, Roy: 16590272 bytes; Ice Climbers, who load
 * Popo's and Nana's, need 1212192) with 1235520 bytes to spare, so no 4v4 trims on the vanilla
 * disc. Squad builds back 40 MiB of ARAM (MU_HOST_ARAM_BYTES, mu_host_aram.h;
 * 32 MiB booted 15411104 bytes of AllA on a retail disc and 13313952 with mod assets, under the
 * worst 4v4). At 40 MiB they boot with 23799712 (retail) and 21702560 (mod assets) bytes of AllA,
 * 5973920 and 3876768 above this floor. A lineup over it (mod archives, longer files) still trims
 * (gmSquad_TrimOnline), and the pick-time refusal and trim notice stay as the backstop.
 * Every side uses the same floors, so the same rows are admitted everywhere.
 * Actual boot heaps must meet both floors or refuse.
 * File lengths (including mod archives), alignment and per-file overhead are
 * still charged by gmSquad_Budget; no host arena or pack-loaded bit is hashed.
 * Costumes and Kirby's colour hats are charged and hashed at fixed per-fighter
 * constants (gmSquad_CostumeBytes), never their local lengths, so skins never
 * change the identity or the trim; in the match a local file over its constant
 * loads the disc's own copy (squad_costume_rule.h). */
#define MU_SQUAD_ONLINE_ALLM 0xC96800u
#define MU_SQUAD_ONLINE_ALLA 0x1100000u

typedef struct MuSquadFighter { unsigned char kind, color; } MuSquadFighter;
typedef struct MuSquadSide {
    unsigned char count;
    MuSquadFighter members[MU_SQUAD_LINEUP];
} MuSquadSide;
typedef struct MuSquadSetup {
    unsigned char stocks;
    MuSquadSide sides[2]; /* canonical in-game ports P1, P2 */
} MuSquadSetup;

static inline unsigned mu_squad_colors(unsigned kind)
{
    static const unsigned char colors[26] = {
        6,5,4,4,6,4,5,4,5,5,4,4,5,4,4,5,5,6,5,5,4,5,5,5,4,5
    };
    return kind < 26 ? colors[kind] : 0;
}
/* A colour that fighter has, else its last: the Friends lobby's costume clamp (mu_friends4_css.inc
 * mu_f4_color, yougame_lobby_setup.h costume_for) over Squad Strike's per-fighter colour counts.
 * Character select runs every door colour through it before it becomes a pick, so a costume past
 * the fighter's own (an m-ex 7th) plays as its last colour and the lineup stays valid on the wire. */
static inline unsigned char mu_squad_color(unsigned kind, unsigned color)
{
    const unsigned n = mu_squad_colors(kind);
    return (unsigned char) (n == 0 ? 0 : color < n ? color : n - 1);
}
static inline int mu_squad_side_valid(const MuSquadSide* side)
{
    unsigned i;
    if (!side || side->count != MU_SQUAD_LINEUP) return 0;
    for (i = 0; i < side->count; ++i)
        if (side->members[i].color >= mu_squad_colors(side->members[i].kind)) return 0;
    return 1;
}
static inline int mu_squad_setup_valid(const MuSquadSetup* setup)
{
    unsigned stocks;
    if (!setup || !mu_squad_side_valid(&setup->sides[0]) ||
        !mu_squad_side_valid(&setup->sides[1])) return 0;
    stocks = setup->sides[0].count > setup->sides[1].count ?
        setup->sides[0].count : setup->sides[1].count;
    return setup->stocks == stocks;
}
/* Exact payload: u8 version, u8 stocks, then twice {u8 count, count * {u8 kind,u8 color}}.
 * No native structs, padding, endian-dependent fields, or trailing bytes on the wire. */
static inline size_t mu_squad_encode(const MuSquadSetup* setup, unsigned char* out, size_t capacity)
{
    size_t at = 0, need;
    unsigned p, i;
    if (!mu_squad_setup_valid(setup) || !out) return 0;
    need = 4 + 2 * (setup->sides[0].count + setup->sides[1].count);
    if (capacity < need) return 0;
    out[at++] = MU_SQUAD_VERSION;
    out[at++] = setup->stocks;
    for (p = 0; p < 2; ++p) {
        out[at++] = setup->sides[p].count;
        for (i = 0; i < setup->sides[p].count; ++i) {
            out[at++] = setup->sides[p].members[i].kind;
            out[at++] = setup->sides[p].members[i].color;
        }
    }
    return at;
}
static inline int mu_squad_decode(const unsigned char* bytes, size_t size, MuSquadSetup* out)
{
    MuSquadSetup value;
    size_t at = 2;
    unsigned p, i, count;
    if (!bytes || !out || size < 4 + 4 * MU_SQUAD_LINEUP || size > MU_SQUAD_WIRE_MAX ||
        bytes[0] != MU_SQUAD_VERSION) return 0;
    memset(&value, 0, sizeof value);
    value.stocks = bytes[1];
    for (p = 0; p < 2; ++p) {
        if (at >= size) return 0;
        count = bytes[at++];
        if (count != MU_SQUAD_LINEUP || size - at < 2 * count) return 0;
        value.sides[p].count = (unsigned char) count;
        for (i = 0; i < count; ++i) {
            value.sides[p].members[i].kind = bytes[at++];
            value.sides[p].members[i].color = bytes[at++];
        }
    }
    if (at != size || !mu_squad_setup_valid(&value)) return 0;
    *out = value;
    return 1;
}
enum MuSquadDecision { MU_SQUAD_NORMAL, MU_SQUAD_PLAY, MU_SQUAD_CANCEL };
/* Called at the ordered 0x82 boundary, never from a clock or timeout. A normal match
 * only when neither side asked for Squad Strike: a squad player never silently gets a
 * 1v1, whether the other side lacks the cap or simply sent no lineup. */
static inline enum MuSquadDecision mu_squad_decide(unsigned own_caps, unsigned peer_caps,
                                                   int own_lineup, int peer_lineup)
{
    if (!own_lineup && !peer_lineup) return MU_SQUAD_NORMAL;
    if (!(own_caps & peer_caps & MU_SQUAD_CAP)) return MU_SQUAD_CANCEL;
    return own_lineup && peer_lineup ? MU_SQUAD_PLAY : MU_SQUAD_CANCEL;
}
static inline int mu_squad_combine(unsigned a_port, const MuSquadSide* a,
                                    unsigned b_port, const MuSquadSide* b, MuSquadSetup* out)
{
    MuSquadSetup value;
    if (!out || a_port > 1 || b_port > 1 || a_port == b_port ||
        !mu_squad_side_valid(a) || !mu_squad_side_valid(b)) return 0;
    memset(&value, 0, sizeof value);
    value.sides[a_port].count = a->count;
    value.sides[b_port].count = b->count;
    memcpy(value.sides[a_port].members, a->members, a->count * sizeof a->members[0]);
    memcpy(value.sides[b_port].members, b->members, b->count * sizeof b->members[0]);
    value.stocks = a->count > b->count ? a->count : b->count;
    *out = value;
    return 1;
}
#endif
#endif
