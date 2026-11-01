/* YouGame Ranked presentation/selection wire v1. Shared by host and native game.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MU_RANKED_WIRE_H
#define MU_RANKED_WIRE_H
/* Decomp units use MSL; importing the host CRT here conflicts with ssize_t. */
typedef unsigned char MuYgU8;
typedef unsigned short MuYgU16;
typedef unsigned int MuYgU32;
typedef int MuYgI32;
typedef char MuYgWidthCheck[(sizeof(MuYgU8) == 1 && sizeof(MuYgU16) == 2 && sizeof(MuYgU32) == 4 && sizeof(MuYgI32) == 4) ? 1 : -1];
#include <string.h>

/* Optional native extension: one subcommand byte; FA-FD are content commands.
 * API-22 hosts may return unknown. Rank readers then use text, policy readers
 * refuse Ranked (an old host cannot validate a remix stage mapping). */
#define MU_YG_COMMAND 0xFEu
#define MU_YG_SUB_RANK 0u
#define MU_YG_SUB_POLICY 1u
/* Local controls exist only in the Ranked test branch. Never sent to a room. */
#define MU_YG_SUB_PRACTICE 2u
#define MU_YG_SUB_PRACTICE_STATUS 3u
/* One-byte request; one-byte boolean reply. Missing/old hosts deny practice. */
#define MU_YG_SUB_PRACTICE_ALLOWED 4u
enum { MU_YG_PRACTICE_STOP, MU_YG_PRACTICE_START,
       MU_YG_PRACTICE_WIN, MU_YG_PRACTICE_LOSS };
#define MU_YG_RANK_VERSION 1u
#define MU_YG_RANK_SIZE 37u
#define MU_YG_RESULT_SIZE 84u
#define MU_YG_RANK_RESPONSE_SIZE 159u
#define MU_YG_POLICY_SIZE 50u

enum { MU_YG_RESULT_NONE, MU_YG_RESULT_RECORDING, MU_YG_RESULT_RECORDED,
       MU_YG_RESULT_UNRATED, MU_YG_RESULT_ERROR };
enum { MU_YG_REASON_NONE, MU_YG_REASON_PACK, MU_YG_REASON_VOID,
       MU_YG_REASON_OTHER, MU_YG_REASON_UNAVAILABLE, MU_YG_REASON_MALFORMED,
       /* RECORDED only: the room gave the set by a leave (disposition "forfeit"), to this seat
        * or to the other one. Appended: older games ignore a RECORDED result's reason. */
       MU_YG_REASON_FORFEIT_WON, MU_YG_REASON_FORFEIT_LOST };

/* ALL wire integers are big endian, signed integers are two's complement. These
 * C structs are decoded values, NEVER memcpy'd onto the wire (no ABI padding).
 * Rank (37 bytes), offsets:
 * 0 known: 0 unknown, 1 authoritative standing; 1 placed: boolean;
 * 2 tier: 0 none, 1 bronze, 2 silver, 3 gold, 4 platinum, 5 diamond, 6 challenger;
 * 3 division: 1..4, or 0 for none/challenger; 4 order: u16 ladder ordering;
 * 6 rating: i32 tenths; 10 matches: u16 (saturated); 12 placement_total: u8;
 * 13 label[24]: ASCII, NUL terminated, zero padded. Unknown rank = all zeroes.
 * Response: 0 version (=1), 1 self rank, 38 opponent rank, 75 result.
 * Result (84 bytes): 0 status (enum above), 1 reason (enum above), 2 before i32
 * tenths, 6 after i32 tenths, 10 rank_before, 47 rank_after. Only RECORDED has
 * authoritative before/after. SELF/OPPONENT are relative to the querying seat.
 */
typedef struct MuYgRank {
    MuYgU8 known, placed, tier, division;
    MuYgU16 order;
    MuYgI32 rating;
    MuYgU16 matches;
    MuYgU8 placement_total;
    char label[24];
} MuYgRank;
typedef struct MuYgResult {
    MuYgU8 status, reason;
    MuYgI32 before, after;
    MuYgRank rank_before, rank_after;
} MuYgResult;
typedef struct MuYgRanks {
    MuYgRank self, opponent;
    MuYgResult result;
} MuYgRanks;

/* Policy (50 bytes): 0 version (1); 1 available (0 refuses Ranked);
 * 2 starters[5] u16 stage IDs in UI order; 12 counterpicks[6] u16 in UI order;
 * 24 strike_players[3] (0 P1/1 P2); 27 strike_counts[3];
 * 30 color_players[2] (0 P1/1 P2); 32 counterpick_actors[4] (0 winner/1 loser:
 * ban, stage, character, character); 36 ban_count; 37 dsr (last won stage);
 * 38 color_ban (set-long same character/color restriction);
 * 39 color_seconds[2]; 41 strike_seconds[3]; 44 counterpick_seconds[4];
 * 48 grace_seconds; 49 wait_seconds (opponent timeout after grace).
 * Timeout fills missing stages with the first enabled entries in the pool order
 * (GameSetup.c:894-925); character/color timeout keeps the current legal choice.
 */
typedef struct MuRankedPolicy {
    MuYgU8 version, available;
    MuYgU16 starters[5], counterpicks[6];
    MuYgU8 strike_players[3], strike_counts[3], color_players[2], counterpick_actors[4];
    MuYgU8 ban_count, dsr, color_ban;
    MuYgU8 color_seconds[2], strike_seconds[3], counterpick_seconds[4];
    MuYgU8 grace_seconds, wait_seconds;
} MuRankedPolicy;

static inline void mu_yg_put16(MuYgU8* p, MuYgU16 v) { p[0] = (MuYgU8)(v >> 8); p[1] = (MuYgU8)v; }
static inline MuYgU16 mu_yg_get16(const MuYgU8* p) { return (MuYgU16)((p[0] << 8) | p[1]); }
static inline void mu_yg_put32(MuYgU8* p, MuYgI32 v) {
    MuYgU32 u = (MuYgU32)v;
    p[0] = (MuYgU8)(u >> 24); p[1] = (MuYgU8)(u >> 16); p[2] = (MuYgU8)(u >> 8); p[3] = (MuYgU8)u;
}
static inline MuYgI32 mu_yg_get32(const MuYgU8* p) {
    MuYgU32 u = ((MuYgU32)p[0] << 24) | ((MuYgU32)p[1] << 16) | ((MuYgU32)p[2] << 8) | p[3];
    return u <= 2147483647u ? (MuYgI32)u : -1 - (MuYgI32)(4294967295u - u);
}
static inline void mu_yg_rank_encode(MuYgU8* p, const MuYgRank* r) {
    memset(p, 0, MU_YG_RANK_SIZE);
    if (!r->known) return;
    p[0] = r->known; p[1] = r->placed; p[2] = r->tier; p[3] = r->division;
    mu_yg_put16(p + 4, r->order); mu_yg_put32(p + 6, r->rating);
    mu_yg_put16(p + 10, r->matches); p[12] = r->placement_total;
    memcpy(p + 13, r->label, 23);
}
static inline void mu_yg_rank_decode(MuYgRank* r, const MuYgU8* p) {
    memset(r, 0, sizeof(*r));
    if (!p[0]) return;
    r->known = p[0]; r->placed = p[1]; r->tier = p[2]; r->division = p[3];
    r->order = mu_yg_get16(p + 4); r->rating = mu_yg_get32(p + 6);
    r->matches = mu_yg_get16(p + 10); r->placement_total = p[12];
    memcpy(r->label, p + 13, 23);
}
static inline void mu_yg_ranks_encode(MuYgU8* p, const MuYgRanks* r) {
    memset(p, 0, MU_YG_RANK_RESPONSE_SIZE); p[0] = MU_YG_RANK_VERSION;
    mu_yg_rank_encode(p + 1, &r->self); mu_yg_rank_encode(p + 38, &r->opponent);
    p[75] = r->result.status; p[76] = r->result.reason;
    mu_yg_put32(p + 77, r->result.before); mu_yg_put32(p + 81, r->result.after);
    mu_yg_rank_encode(p + 85, &r->result.rank_before); mu_yg_rank_encode(p + 122, &r->result.rank_after);
}
static inline int mu_yg_ranks_decode(MuYgRanks* r, const MuYgU8* p, unsigned size) {
    memset(r, 0, sizeof(*r));
    if (size != MU_YG_RANK_RESPONSE_SIZE || p[0] != MU_YG_RANK_VERSION) return 0;
    mu_yg_rank_decode(&r->self, p + 1); mu_yg_rank_decode(&r->opponent, p + 38);
    r->result.status = p[75]; r->result.reason = p[76];
    r->result.before = mu_yg_get32(p + 77); r->result.after = mu_yg_get32(p + 81);
    mu_yg_rank_decode(&r->result.rank_before, p + 85); mu_yg_rank_decode(&r->result.rank_after, p + 122);
    return 1;
}
static inline void mu_yg_policy_encode(MuYgU8* p, const MuRankedPolicy* r) {
    unsigned i;
    p[0] = r->version; p[1] = r->available;
    for (i = 0; i < 5; ++i) mu_yg_put16(p + 2 + 2*i, r->starters[i]);
    for (i = 0; i < 6; ++i) mu_yg_put16(p + 12 + 2*i, r->counterpicks[i]);
    memcpy(p + 24, r->strike_players, 3); memcpy(p + 27, r->strike_counts, 3);
    memcpy(p + 30, r->color_players, 2); memcpy(p + 32, r->counterpick_actors, 4);
    p[36] = r->ban_count; p[37] = r->dsr; p[38] = r->color_ban;
    memcpy(p + 39, r->color_seconds, 2); memcpy(p + 41, r->strike_seconds, 3);
    memcpy(p + 44, r->counterpick_seconds, 4); p[48] = r->grace_seconds; p[49] = r->wait_seconds;
}
static inline int mu_yg_policy_decode(MuRankedPolicy* r, const MuYgU8* p, unsigned size) {
    unsigned i;
    memset(r, 0, sizeof(*r));
    if (size != MU_YG_POLICY_SIZE || p[0] != 1) return 0;
    r->version = p[0]; r->available = p[1];
    for (i = 0; i < 5; ++i) r->starters[i] = mu_yg_get16(p + 2 + 2*i);
    for (i = 0; i < 6; ++i) r->counterpicks[i] = mu_yg_get16(p + 12 + 2*i);
    memcpy(r->strike_players, p + 24, 3); memcpy(r->strike_counts, p + 27, 3);
    memcpy(r->color_players, p + 30, 2); memcpy(r->counterpick_actors, p + 32, 4);
    r->ban_count = p[36]; r->dsr = p[37]; r->color_ban = p[38];
    memcpy(r->color_seconds, p + 39, 2); memcpy(r->strike_seconds, p + 41, 3);
    memcpy(r->counterpick_seconds, p + 44, 4); r->grace_seconds = p[48]; r->wait_seconds = p[49];
    return 1;
}
#endif
