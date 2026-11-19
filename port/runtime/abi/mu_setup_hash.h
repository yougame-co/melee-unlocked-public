/* Melee Unlocked's match setup hash: both machines build the match setup on their own, then
 * exchange a hash of it (NP_MSG_MU_SETUP_HASH) and start only when every hash agrees.
 *
 * Each message carries two 64-bit FNV-1a hashes:
 *   inputs: what the setup was built from (the selections every player sent, the game's own
 *           overwrites, the room's teams, the Squad Strike lineups). Two machines that hold the
 *           same inputs must build the same setup.
 *   setup:  what was built (the game-info block as written to the game, every player's port,
 *           character, colour and team, the stage, the RNG offset, the rules, the squad bytes),
 *           and while Squad Strike is live, the squad budget identity (the 64-hex disc budget
 *           hash) and the online heap floors (MU_SQUAD_ONLINE_ALLM, MU_SQUAD_ONLINE_ALLA).
 * A peer whose inputs differ from ours has not seen our latest selections yet (or we theirs):
 * that is a wait, never a refusal. Same inputs with a different setup is a real disagreement.
 * The budget identity and floors are never inputs: no message brings the peer's, so a waiting
 * peer could never catch up. A difference there is a mismatch that refuses before frame 0.
 *
 * Plain C, no host state, fixed big-endian wire and hash order: the same bytes on every machine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MU_SETUP_HASH_H
#define MU_SETUP_HASH_H
#include <stddef.h>
#include <stdint.h>

#define MU_SETUP_HASH_MSG 0xE6   /* netplay message id; 0xE4 and 0xE5 stay free for others */
#define MU_SETUP_HASH_CAP 0x40   /* NP_MSG_MU_BUILD capability bit; 0x20 is pick previews */
#define MU_SETUP_HASH_VERSION 1
#define MU_SETUP_HASH_WIRE 21    /* excludes the message id: 'M' 'U' 'H', version, port, 2 x u64 */
#define MU_SETUP_HASH_TEXT "Match setup didn't match your opponent's. Match not started."

#define MU_FNV64_OFFSET 0xCBF29CE484222325ull
#define MU_FNV64_PRIME 0x00000100000001B3ull

static inline uint64_t mu_fnv64(uint64_t h, const void* data, size_t size)
{
    const unsigned char* p = (const unsigned char*) data;
    size_t i;
    for (i = 0; i < size; ++i) {
        h ^= p[i];
        h *= MU_FNV64_PRIME;
    }
    return h;
}
static inline uint64_t mu_fnv64_u8(uint64_t h, unsigned v)
{
    const unsigned char b = (unsigned char) v;
    return mu_fnv64(h, &b, 1);
}
static inline uint64_t mu_fnv64_u16(uint64_t h, unsigned v)
{
    const unsigned char b[2] = {(unsigned char) (v >> 8), (unsigned char) v};
    return mu_fnv64(h, b, 2);
}
static inline uint64_t mu_fnv64_u32(uint64_t h, uint32_t v)
{
    const unsigned char b[4] = {(unsigned char) (v >> 24), (unsigned char) (v >> 16),
                                (unsigned char) (v >> 8), (unsigned char) v};
    return mu_fnv64(h, b, 4);
}
/* A length, then the bytes: two fields can never run into each other. */
static inline uint64_t mu_fnv64_bytes(uint64_t h, const void* data, size_t size)
{
    return mu_fnv64(mu_fnv64_u32(h, (uint32_t) size), data, size);
}

typedef struct MuSetupHash {
    unsigned char port;   /* the sender's player index */
    uint64_t inputs, setup;
} MuSetupHash;

static inline void mu_setup_hash_put64(unsigned char* out, uint64_t v)
{
    int i;
    for (i = 0; i < 8; ++i) out[i] = (unsigned char) (v >> (56 - 8 * i));
}
static inline uint64_t mu_setup_hash_get64(const unsigned char* in)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; ++i) v = (v << 8) | in[i];
    return v;
}
/* Exact payload after the message id; 0 when it does not fit. */
static inline size_t mu_setup_hash_encode(const MuSetupHash* value, unsigned char* out, size_t capacity)
{
    if (!value || !out || capacity < MU_SETUP_HASH_WIRE) return 0;
    out[0] = 'M';
    out[1] = 'U';
    out[2] = 'H';
    out[3] = MU_SETUP_HASH_VERSION;
    out[4] = value->port;
    mu_setup_hash_put64(out + 5, value->inputs);
    mu_setup_hash_put64(out + 13, value->setup);
    return MU_SETUP_HASH_WIRE;
}
/* Exactly MU_SETUP_HASH_WIRE bytes: anything shorter, longer or of another version is refused. */
static inline int mu_setup_hash_decode(const unsigned char* bytes, size_t size, MuSetupHash* out)
{
    if (!bytes || !out || size != MU_SETUP_HASH_WIRE || bytes[0] != 'M' || bytes[1] != 'U' ||
        bytes[2] != 'H' || bytes[3] != MU_SETUP_HASH_VERSION) return 0;
    out->port = bytes[4];
    out->inputs = mu_setup_hash_get64(bytes + 5);
    out->setup = mu_setup_hash_get64(bytes + 13);
    return 1;
}

/* The ports of the game-info block (0x60 + 0x24 per port): P3 and P4's slot type (0 = a human
 * plays there, 3 = nobody) for this match's player count, then every human port's costume index
 * (0x67): 0 for the first port with a character (and, in Teams, team; otherwise colour), 1 for the
 * next one, and so on. Ice Climbers count as Popo. The block is kept between matches and builds,
 * so the slot types are written first: counting with the previous build's P3/P4 types gave a
 * 4-player match's first build other costume indices than every later one, from the same inputs. */
static inline void mu_setup_ports(unsigned char* block, size_t size, unsigned remote_count, int teams)
{
    int i, j;
    if (!block || size < 0x60 + 4 * 0x24) return;
    block[0x61 + 2 * 0x24] = remote_count >= 2 ? 0 : 3;
    block[0x61 + 3 * 0x24] = remote_count >= 3 ? 0 : 3;
    for (i = 0; i < 4; ++i) {
        const unsigned char* a = block + 0x60 + i * 0x24;
        unsigned char index = 0;
        if (a[1] != 0) continue;
        for (j = 0; j < i; ++j) {
            const unsigned char* b = block + 0x60 + j * 0x24;
            if (b[1] != 0) continue;
            if ((a[0] == 0x13 ? 0x12 : a[0]) == (b[0] == 0x13 ? 0x12 : b[0]) &&
                (teams ? a[9] == b[9] : a[3] == b[3])) ++index;
        }
        block[0x67 + i * 0x24] = index;
    }
}

/* The 4-player Friends lobby (yougame_lobby_setup.h): the room's frozen setup names every port's
 * slot type (0 = a human plays there, 1 = a CPU, 3 = nobody). Write all four, then the costume index
 * (0x67) over every port that plays, humans and CPUs alike, by the same rule as mu_setup_ports. */
static inline void mu_setup_ports_kinds(unsigned char* block, size_t size, const unsigned char kinds[4], int teams)
{
    int i, j;
    if (!block || !kinds || size < 0x60 + 4 * 0x24) return;
    for (i = 0; i < 4; ++i) block[0x61 + i * 0x24] = kinds[i] == 0 || kinds[i] == 1 ? kinds[i] : 3;
    for (i = 0; i < 4; ++i) {
        const unsigned char* a = block + 0x60 + i * 0x24;
        unsigned char index = 0;
        if (a[1] != 0 && a[1] != 1) continue;
        for (j = 0; j < i; ++j) {
            const unsigned char* b = block + 0x60 + j * 0x24;
            if (b[1] != 0 && b[1] != 1) continue;
            if ((a[0] == 0x13 ? 0x12 : a[0]) == (b[0] == 0x13 ? 0x12 : b[0]) &&
                (teams ? a[9] == b[9] : a[3] == b[3])) ++index;
        }
        block[0x67 + i * 0x24] = index;
    }
}

/* Which of a port's records the setup hash keeps whole: a human's (0) and a CPU's (1). Any other
 * port (3, nobody) keeps only its slot type, since the block still holds whoever last played there.
 * A CPU's record holds its character, colour, team and level: blanking it, as before the Friends
 * lobby, let two machines agree on a match whose CPUs differed. */
static inline int mu_setup_record_hashed(unsigned type)
{
    return type == 0 || type == 1;
}

enum MuSetupHashDecision {
    MU_SETUP_HASH_SKIP,      /* the peer never said it sends one (Slippi Dolphin, an older build) */
    MU_SETUP_HASH_WAIT,      /* nothing yet, or built from other inputs: wait for the next one */
    MU_SETUP_HASH_AGREE,     /* the same inputs built the same setup */
    MU_SETUP_HASH_MISMATCH   /* the same inputs built another setup: never start this match */
};
/* One peer. No clock: a missing hash is the caller's connect timeout, the same on both sides. */
static inline enum MuSetupHashDecision mu_setup_hash_decide(int peer_cap, int peer_received,
                                                            const MuSetupHash* local,
                                                            const MuSetupHash* peer)
{
    if (!peer_cap) return MU_SETUP_HASH_SKIP;
    if (!peer_received || !local || !peer || peer->inputs != local->inputs) return MU_SETUP_HASH_WAIT;
    return peer->setup == local->setup ? MU_SETUP_HASH_AGREE : MU_SETUP_HASH_MISMATCH;
}

/* A mismatch refuses the match only once it has stood for MU_SETUP_HASH_SETTLE_MS. Each machine
 * sends its hash once per change, on its own frames, so the hash held from a peer can be one the
 * peer has already replaced (its next one is on the wire): a setup that changed once on every
 * machine compared each side's new hash with a peer's old one and refused a match all four
 * agreed on. Either side's next hash restarts the wait; a real disagreement still refuses, within
 * the connect timeout. The caller keeps one per peer and resets it on any other decision. */
#define MU_SETUP_HASH_SETTLE_MS 1000
typedef struct MuSetupHashPending {
    int open;
    uint64_t local, peer, since;
} MuSetupHashPending;
static inline int mu_setup_hash_mismatch_final(MuSetupHashPending* pending, const MuSetupHash* local,
                                               const MuSetupHash* peer, uint64_t now_ms)
{
    if (!pending || !local || !peer) return 1;
    if (!pending->open || pending->local != local->setup || pending->peer != peer->setup) {
        pending->open = 1;
        pending->local = local->setup;
        pending->peer = peer->setup;
        pending->since = now_ms;
    }
    return now_ms - pending->since >= MU_SETUP_HASH_SETTLE_MS;
}

/* When the wait for one peer's matching hash began. The connect timeout counts from this side's own
 * latest setup OR the peer's latest hash, whichever came last. A ranked set builds its first setup
 * at lock-in, agrees on it, and then strikes for half a minute or more before the game's overwrite
 * builds the real one. The seat that finishes first sends its new hash; on the other seat (still
 * waiting for the last strike step, a relay hop behind) that hash has other inputs, a wait. Counted
 * from that seat's lock-in setup, the wait was long over the moment the hash arrived, and it treated
 * a peer one step ahead as gone (11-18, Ness vs Ness, ended at step 4). A peer's newer hash is proof
 * it is there and moving: the wait counts from it. The caller keeps one per peer, reset with the
 * rest of its state when its own setup changes. */
typedef struct MuSetupHashWait {
    int seen;                /* a peer hash has been noted since the last reset */
    uint64_t inputs, setup;  /* that hash */
    uint64_t since;          /* when it arrived (first seen) */
} MuSetupHashWait;
static inline int mu_setup_hash_wait_over(MuSetupHashWait* wait, uint64_t local_since, int peer_received,
                                          const MuSetupHash* peer, uint64_t now_ms, uint64_t timeout_ms)
{
    uint64_t start = local_since;
    if (!wait) return now_ms - local_since >= timeout_ms;
    if (peer_received && peer &&
        (!wait->seen || wait->inputs != peer->inputs || wait->setup != peer->setup)) {
        wait->seen = 1;
        wait->inputs = peer->inputs;
        wait->setup = peer->setup;
        wait->since = now_ms;
    }
    if (wait->seen && wait->since > start) start = wait->since;
    return now_ms - start >= timeout_ms;
}
#endif
