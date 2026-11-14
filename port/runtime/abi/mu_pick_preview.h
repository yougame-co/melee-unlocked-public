/* Live pick previews on the online character screen: what each player is pointing their pick
 * at right now (fighter, colour, team, the Squad Strike slots picked so far, locked in or not).
 * Display only. The match itself is still decided by the 0x82 selections (and 0xE3 lineup):
 * nothing here ever reaches match setup.
 *
 * Two byte formats, both exact-size, with no native structs, padding or endian-dependent fields:
 *   the wire payload (after the NP_MSG_MU_PICK_PREVIEW id): u8 version, u8 char_id, u8 color,
 *     u8 team_id, u8 squad_count, 3 * {u8 kind, u8 color}, u8 locked;
 *   the game/host read reply: u8 version, then per remote index 0..2 {u8 valid, u8 port, wire}.
 * Sent only to a peer whose MU_BUILD caps carry MU_PICK_PREVIEW_CAP. */
#ifndef MU_PICK_PREVIEW_H
#define MU_PICK_PREVIEW_H
#include <stddef.h>
#include <string.h>

#define MU_PICK_PREVIEW_CAP 0x20        /* MU_BUILD caps bit (after MU_SQUAD_CAP 0x10) */
#define MU_PICK_PREVIEW_MSG 0xE4        /* netplay message id (after 0xE3) */
#define MU_PICK_PREVIEW_COMMAND 0xED    /* private game/host ABI, never a YouGame bridge key */
#define MU_PICK_PREVIEW_OP_SEND 0       /* {op, wire}: the local pick, every CSS frame */
#define MU_PICK_PREVIEW_OP_READ 1       /* {op}: reply MU_PICK_PREVIEW_REPLY bytes */
#define MU_PICK_PREVIEW_VERSION 1
#define MU_PICK_PREVIEW_SLOTS 3         /* Squad Strike's lineup size */
#define MU_PICK_PREVIEW_REMOTES 3       /* REMOTE_PLAYER_MAX */
#define MU_PICK_PREVIEW_NONE 0xFF       /* no fighter / no team / an empty slot's kind */
#define MU_PICK_PREVIEW_WIRE (5 + 2 * MU_PICK_PREVIEW_SLOTS + 1)
#define MU_PICK_PREVIEW_REPLY (1 + MU_PICK_PREVIEW_REMOTES * (2 + MU_PICK_PREVIEW_WIRE))
#define MU_PICK_PREVIEW_INTERVAL 6      /* frames: at most one preview per this many */

typedef struct MuPickPreview {
    unsigned char char_id;      /* external fighter id while the coin is down, else NONE */
    unsigned char color;        /* the costume the portrait shows */
    unsigned char team_id;      /* 0-2 in Teams, else NONE */
    unsigned char squad_count;  /* 0-3 slots filled; 0 outside Squad Strike */
    unsigned char kinds[MU_PICK_PREVIEW_SLOTS];   /* NONE past squad_count */
    unsigned char colors[MU_PICK_PREVIEW_SLOTS];  /* 0 past squad_count */
    unsigned char locked;       /* 0 or 1 */
} MuPickPreview;

/* Canonical form: empty slots are {NONE, 0}, filled slots name a fighter, flags are 0/1. */
static inline int mu_pick_preview_valid(const MuPickPreview* p)
{
    unsigned i;
    if (!p || p->squad_count > MU_PICK_PREVIEW_SLOTS || p->locked > 1 ||
        (p->team_id > 2 && p->team_id != MU_PICK_PREVIEW_NONE)) return 0;
    for (i = 0; i < MU_PICK_PREVIEW_SLOTS; ++i) {
        if (i < p->squad_count) {
            if (p->kinds[i] == MU_PICK_PREVIEW_NONE) return 0;
        } else if (p->kinds[i] != MU_PICK_PREVIEW_NONE || p->colors[i] != 0) return 0;
    }
    return 1;
}

/* Fills unused slots canonically, so callers only set the filled ones. */
static inline void mu_pick_preview_clear(MuPickPreview* p)
{
    memset(p, 0, sizeof *p);
    p->char_id = p->team_id = MU_PICK_PREVIEW_NONE;
    memset(p->kinds, MU_PICK_PREVIEW_NONE, sizeof p->kinds);
}

static inline size_t mu_pick_preview_encode(const MuPickPreview* p, unsigned char* out, size_t capacity)
{
    size_t at = 0;
    unsigned i;
    if (!mu_pick_preview_valid(p) || !out || capacity < MU_PICK_PREVIEW_WIRE) return 0;
    out[at++] = MU_PICK_PREVIEW_VERSION;
    out[at++] = p->char_id;
    out[at++] = p->color;
    out[at++] = p->team_id;
    out[at++] = p->squad_count;
    for (i = 0; i < MU_PICK_PREVIEW_SLOTS; ++i) {
        out[at++] = p->kinds[i];
        out[at++] = p->colors[i];
    }
    out[at++] = p->locked;
    return at;
}

static inline int mu_pick_preview_decode(const unsigned char* bytes, size_t size, MuPickPreview* out)
{
    MuPickPreview value;
    size_t at = 5;
    unsigned i;
    if (!bytes || !out || size != MU_PICK_PREVIEW_WIRE || bytes[0] != MU_PICK_PREVIEW_VERSION) return 0;
    memset(&value, 0, sizeof value);
    value.char_id = bytes[1];
    value.color = bytes[2];
    value.team_id = bytes[3];
    value.squad_count = bytes[4];
    for (i = 0; i < MU_PICK_PREVIEW_SLOTS; ++i) {
        value.kinds[i] = bytes[at++];
        value.colors[i] = bytes[at++];
    }
    value.locked = bytes[at++];
    if (at != size || !mu_pick_preview_valid(&value)) return 0;
    *out = value;
    return 1;
}

/* The sender's rate limit: send when the bytes differ from the last sent ones and at least
 * MU_PICK_PREVIEW_INTERVAL frames passed since then, or when `force` (lock-in, a fresh peer).
 * A change held back by the interval goes out on the first due frame after it. */
typedef struct MuPickPreviewSender {
    unsigned char last[MU_PICK_PREVIEW_WIRE];
    unsigned last_frame;
    int sent;
} MuPickPreviewSender;

static inline int mu_pick_preview_due(MuPickPreviewSender* s, const unsigned char bytes[MU_PICK_PREVIEW_WIRE],
                                      unsigned frame, int force)
{
    if (!s || !bytes) return 0;
    if (s->sent && memcmp(s->last, bytes, MU_PICK_PREVIEW_WIRE) == 0 && !force) return 0;
    if (s->sent && !force && frame - s->last_frame < MU_PICK_PREVIEW_INTERVAL) return 0;
    memcpy(s->last, bytes, MU_PICK_PREVIEW_WIRE);
    s->last_frame = frame;
    s->sent = 1;
    return 1;
}

/* The read reply, one slot per remote index. Invalid slots are all zero. */
typedef struct MuPickPreviewSlot {
    unsigned char valid, port;
    MuPickPreview pick;
} MuPickPreviewSlot;

static inline size_t mu_pick_preview_reply_encode(const MuPickPreviewSlot slots[MU_PICK_PREVIEW_REMOTES],
                                                  unsigned char* out, size_t capacity)
{
    size_t at = 0;
    unsigned i;
    if (!slots || !out || capacity < MU_PICK_PREVIEW_REPLY) return 0;
    memset(out, 0, MU_PICK_PREVIEW_REPLY);
    out[at++] = MU_PICK_PREVIEW_VERSION;
    for (i = 0; i < MU_PICK_PREVIEW_REMOTES; ++i, at += 2 + MU_PICK_PREVIEW_WIRE) {
        if (!slots[i].valid || slots[i].port > 3 ||
            mu_pick_preview_encode(&slots[i].pick, out + at + 2, MU_PICK_PREVIEW_WIRE) != MU_PICK_PREVIEW_WIRE) {
            memset(out + at, 0, 2 + MU_PICK_PREVIEW_WIRE);
            continue;
        }
        out[at] = 1;
        out[at + 1] = slots[i].port;
    }
    return at;
}

/* All or nothing on the frame (size, version); a bad slot is only that slot invalid. */
static inline int mu_pick_preview_reply_decode(const unsigned char* bytes, size_t size,
                                               MuPickPreviewSlot out[MU_PICK_PREVIEW_REMOTES])
{
    size_t at = 1;
    unsigned i;
    if (!out) return 0;
    memset(out, 0, MU_PICK_PREVIEW_REMOTES * sizeof out[0]);
    if (!bytes || size != MU_PICK_PREVIEW_REPLY || bytes[0] != MU_PICK_PREVIEW_VERSION) return 0;
    for (i = 0; i < MU_PICK_PREVIEW_REMOTES; ++i, at += 2 + MU_PICK_PREVIEW_WIRE) {
        if (bytes[at] != 1 || bytes[at + 1] > 3 ||
            !mu_pick_preview_decode(bytes + at + 2, MU_PICK_PREVIEW_WIRE, &out[i].pick)) {
            memset(&out[i], 0, sizeof out[i]);
            continue;
        }
        out[i].valid = 1;
        out[i].port = bytes[at + 1];
    }
    return 1;
}
#endif
