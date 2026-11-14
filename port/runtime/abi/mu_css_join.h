/* Friends CSS wire record. Bytes only: no C ABI padding, pointers, bitfields or native endian.
 * Multi-byte integers and IEEE754 float bits are little-endian. Never memcpy a game struct here.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_CSS_JOIN_H
#define MU_CSS_JOIN_H
#define MU_CSS_VERSION 2
#define MU_CSS_SIZE 252
#define MU_CSS_CAPTURE 0
#define MU_CSS_APPLY 1
#define MU_CSS_IDENTITY 2
#define MU_CSS_PORTS 4
#define MU_CSS_CAPTURE_CHECKED 3
/* Capture results: a stable CSS may still need the normal relaunch path. */
#define MU_CSS_UNSTABLE -1
#define MU_CSS_NOT_TRANSFERABLE 1
typedef struct MuCssError {
    const char* field;
    int port, offset;
    double value;
} MuCssError;
static inline int mu_css_reject(MuCssError* error, const char* field, int port, int offset, double value)
{
    if (error) { error->field = field; error->port = port; error->offset = offset; error->value = value; }
    return 0;
}
/* Identity is the eight game-encoded tag bytes, port rumble and name rumble, plus the
 * game's automatic-handicap value. Host device mappings/calibration never leave the host. */
typedef struct MuCssIdentity {
    unsigned char tag[8], flags, auto_handicap;
} MuCssIdentity;
typedef struct MuCssPort {
    unsigned char fighter, costume, kind, team, cpu, handicap, tag_index, flags;
    unsigned char tag[8];
    unsigned char cursor_x[4], cursor_y[4], token_x[4], token_y[4], target_x[4], target_y[4];
    unsigned char cursor_state, held_token, cursor_flags, cursor_timer[2], cursor_repeat[2];
    unsigned char token_state, token_colour, token_timer, auto_handicap;
} MuCssPort;
typedef struct MuCssState {
    unsigned char version, size, shield01[2];
    /* GameRules 0x00..0x13 by named field; unk_14 is a LE signed 32-bit integer. */
    unsigned char rules[24];
    unsigned char item_frequency, item_mask[8], stage_mask[4], teams, stage[2];
    unsigned char ready, shield23[2], reserved;
    MuCssPort ports[4];
} MuCssState;
static inline unsigned long long mu_css_read_le(const unsigned char* p, unsigned n)
{
    unsigned long long v = 0; unsigned i;
    for (i = 0; i < n; ++i) v |= (unsigned long long) p[i] << (8 * i);
    return v;
}
static inline void mu_css_write_le(unsigned char* p, unsigned long long v, unsigned n)
{
    unsigned i; for (i = 0; i < n; ++i) p[i] = (unsigned char) (v >> (8 * i));
}
/* Retail mnnamenew.c:83-229 (KeyMap/GlyphTable), 692-712 (four glyphs).
 * Complete Shift-JIS glyphs only, never byte 0x25, and a zero-filled empty tail.
 * Do not repair an untrusted name: the whole join must take the fallback. */
static inline int mu_css_tag_check(const unsigned char tag[8], int* offset)
{
    static const unsigned short glyphs[] = {
        0x8140, 0x8141, 0x8142, 0x8144, 0x8148, 0x8149, 0x815b, 0x8160, 0x817b, 0x817c,
        0x8181, 0x8190, 0x8193, 0x8195, 0x8197, 0x824f, 0x8250, 0x8251, 0x8252, 0x8253,
        0x8254, 0x8255, 0x8256, 0x8257, 0x8258, 0x8260, 0x8261, 0x8262, 0x8263, 0x8264,
        0x8265, 0x8266, 0x8267, 0x8268, 0x8269, 0x826a, 0x826b, 0x826c, 0x826d, 0x826e,
        0x826f, 0x8270, 0x8271, 0x8272, 0x8273, 0x8274, 0x8275, 0x8276, 0x8277, 0x8278,
        0x8279, 0x829f, 0x82a0, 0x82a1, 0x82a2, 0x82a3, 0x82a4, 0x82a5, 0x82a6, 0x82a7,
        0x82a8, 0x82a9, 0x82aa, 0x82ab, 0x82ac, 0x82ad, 0x82ae, 0x82af, 0x82b0, 0x82b1,
        0x82b2, 0x82b3, 0x82b4, 0x82b5, 0x82b6, 0x82b7, 0x82b8, 0x82b9, 0x82ba, 0x82bb,
        0x82bc, 0x82bd, 0x82be, 0x82bf, 0x82c0, 0x82c1, 0x82c2, 0x82c3, 0x82c4, 0x82c5,
        0x82c6, 0x82c7, 0x82c8, 0x82c9, 0x82ca, 0x82cb, 0x82cc, 0x82cd, 0x82ce, 0x82cf,
        0x82d0, 0x82d1, 0x82d2, 0x82d3, 0x82d4, 0x82d5, 0x82d6, 0x82d7, 0x82d8, 0x82d9,
        0x82da, 0x82db, 0x82dc, 0x82dd, 0x82de, 0x82df, 0x82e0, 0x82e1, 0x82e2, 0x82e3,
        0x82e4, 0x82e5, 0x82e6, 0x82e7, 0x82e8, 0x82e9, 0x82ea, 0x82eb, 0x82ed, 0x82f0,
        0x82f1, 0x8340, 0x8341, 0x8342, 0x8343, 0x8344, 0x8345, 0x8346, 0x8347, 0x8348,
        0x8349, 0x834a, 0x834b, 0x834c, 0x834d, 0x834e, 0x834f, 0x8350, 0x8351, 0x8352,
        0x8353, 0x8354, 0x8355, 0x8356, 0x8357, 0x8358, 0x8359, 0x835a, 0x835b, 0x835c,
        0x835d, 0x835e, 0x835f, 0x8360, 0x8361, 0x8362, 0x8363, 0x8364, 0x8365, 0x8366,
        0x8367, 0x8368, 0x8369, 0x836a, 0x836b, 0x836c, 0x836d, 0x836e, 0x836f, 0x8370,
        0x8371, 0x8372, 0x8373, 0x8374, 0x8375, 0x8376, 0x8377, 0x8378, 0x8379, 0x837a,
        0x837b, 0x837c, 0x837d, 0x837e, 0x8380, 0x8381, 0x8382, 0x8383, 0x8384, 0x8385,
        0x8386, 0x8387, 0x8388, 0x8389, 0x838a, 0x838b, 0x838c, 0x838d, 0x838f, 0x8392,
        0x8393,
    };
    unsigned i = 0, count = 0, j, content = 0;
    while (i < 8 && tag[i]) {
        unsigned glyph;
        if (++count > 4 || tag[i] == 0x25) break;
        if (tag[i] == 0x20) { ++i; continue; }
        if (i + 1 >= 8) break;
        if (tag[i+1] == 0x25) { ++i; break; }
        glyph = ((unsigned) tag[i] << 8) | tag[i+1];
        for (j = 0; j < sizeof(glyphs)/sizeof(glyphs[0]); ++j)
            if (glyphs[j] == glyph) break;
        if (j == sizeof(glyphs)/sizeof(glyphs[0])) break;
        content |= glyph != 0x8140;
        i += 2;
    }
    if (i < 8 && tag[i]) { if (offset) *offset = (int)i; return 0; }
    for (; i < 8; ++i) if (tag[i]) { if (offset) *offset = (int)i; return 0; }
    if (count && !content) { if (offset) *offset = 0; return 0; }
    return 1;
}
static inline int mu_css_tag_valid(const unsigned char tag[8]) { return mu_css_tag_check(tag, 0); }
static inline int mu_css_identity_check(const unsigned char tag[8], unsigned flags, unsigned handicap,
                                      MuCssError* error, int port)
{
    int offset = 0;
    if (!mu_css_tag_check(tag,&offset)) return mu_css_reject(error,"tag",port,offset,0);
    if ((flags & ~7u) || ((flags & 4) ? !tag[0] : ((flags & 2) || tag[0])))
        return mu_css_reject(error,"flags",port,-1,flags);
    if (handicap < 1 || handicap > 9) return mu_css_reject(error,"auto_handicap",port,-1,handicap);
    return 1;
}
static inline int mu_css_identity_valid(const unsigned char tag[8], unsigned flags, unsigned handicap)
{
    return mu_css_identity_check(tag,flags,handicap,0,-1);
}
/* Sound-test music IDs, not the system sound mode (which is fixed by the host). */
static inline int mu_css_music_valid(unsigned id)
{
    static const unsigned char ids[] = {0,1,2,3,4,5,6,7,8,10,11,13,14,15,16,17,19,20,21,22,23,24,25,29,30,31,32,33,34,35,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,74,75,76,77,78,80,81,83,84,93,94,95,96,97};
    unsigned i;
    for (i = 0; i < sizeof(ids); ++i) if (id == ids[i]) return 1;
    return 0;
}
/* Retail field sources (src/melee in the pinned decomp):
 * mn/mnmainrule.c:114,143,325: primary rule limits; mn/mnruleplus.c:101: extra rules.
 * gm/gmmain_lib.c:29,869: rule defaults/menu music; mn/mnsoundtest.c:89: music IDs.
 * mn/mndiagram.c:43,1380 and mn/mndiagram2.c:261: record-menu indices.
 * mn/mnitemsw.c:180,227: frequency -1..4 and mask toggles (31 item IDs at :80).
 * mn/mnstagesw.c:46: 29 stage bits; mn/mnstagesel.static.h:15: selected stage IDs.
 * mn/mncharsel.c:161: fighter icons; gm/gm_1601.c:158: costume counts.
 * mn/mncharsel.c:2315: CPU/handicap 1..9; gm/gm_1601.c:3620: handicap indexing.
 * mn/mncharsel.c:2433,2533,3104,3147,3419,3440,3586,4491,4524: cursor/token
 * states, slider IDs, colours, timers, finite motion and initialization. Tokens may
 * overshoot their target by at most three units; the bounds below enclose that motion.
 * sourceport/game/shim/mu_te_menus.c:15,73: shield palette and disabled sentinel.
 * Version/size/reserved and flags are the byte ABI above, not retail structure padding. */
static inline int mu_css_validate(const MuCssState* s, MuCssError* error)
{
    unsigned i, k;
#define MU_CHECK(test, field, port, offset, value) \
    do { if (!(test)) return mu_css_reject(error,field,port,offset,(double)(value)); } while (0)
#define MU_FIELD(field, test) MU_CHECK((test), #field, -1, -1, s->field)
    if (!s) return mu_css_reject(error,"state",-1,-1,0);
    MU_FIELD(version,s->version == MU_CSS_VERSION);
    MU_FIELD(size,s->size == MU_CSS_SIZE);
    MU_FIELD(reserved,!s->reserved);
    MU_FIELD(teams,s->teams <= 1);
    MU_FIELD(ready,s->ready <= 1);
    MU_FIELD(item_frequency,s->item_frequency <= 4 || s->item_frequency == 255);
    for (i = 0; i < 4; ++i) {
        unsigned char shield = i < 2 ? s->shield01[i] : s->shield23[i-2];
        MU_CHECK(shield <= 17 || shield == 255,"shield",(int)i,-1,shield);
    }
    for (i = 0; i < 20; ++i) {
        static const unsigned char max[] = {1,0x36,3,99,99,2,20,4,99,1,1,1,2,1,24,24,119,119,24,119};
        unsigned v = s->rules[i];
        MU_CHECK(v <= max[i] && (i != 1 || v == 0x34 || v == 0x36) &&
                 (i != 4 || v) && (i != 6 || v >= 5),"rules",-1,(int)i,v);
    }
    { unsigned long long music = mu_css_read_le(s->rules+20,4);
      MU_CHECK(music == 0xffffffffu || mu_css_music_valid((unsigned)music),"rules",-1,20,music); }
    { unsigned long long items = mu_css_read_le(s->item_mask,8);
      MU_CHECK((items & 0x10000000u) && (items >> 32) == ((items & 0x80000000u) ? 0xffffffffu : 0),"item_mask",-1,-1,items); }
    MU_CHECK((mu_css_read_le(s->stage_mask,4) | 0x1fffffffu) == 0xffffffffu,"stage_mask",-1,-1,mu_css_read_le(s->stage_mask,4));
    { unsigned stage = (unsigned)mu_css_read_le(s->stage,2);
      MU_CHECK(stage <= 32 && stage != 1 && stage != 21 && stage != 26,"stage",-1,-1,stage); }
    for (i = 0; i < 4; ++i) {
        const MuCssPort* p = &s->ports[i];
#define MU_PORT(field, test) MU_CHECK((test), #field, (int)i, -1, p->field)
        /* Fighter icons and costume counts are runtime resources (including m-ex).
         * The native capture/apply checks icons[] and gm_GetNumCostumesForCKind. */
        MU_PORT(kind,p->kind == 0 || p->kind == 1 || p->kind == 3);
        MU_PORT(team,p->team <= 2);
        MU_PORT(cpu,p->cpu >= 1 && p->cpu <= 9);
        MU_PORT(handicap,p->handicap >= 1 && p->handicap <= 9);
        MU_PORT(tag_index,p->tag_index <= 120 && ((p->flags & 4) ? p->tag_index != 120 : p->tag_index == 120));
        MU_PORT(flags,!(p->flags & ~63) && (p->flags & 48) != 48);
        if (!mu_css_identity_check(p->tag,p->flags & 7,p->auto_handicap,error,(int)i)) return 0;
        MU_PORT(cursor_flags,!p->cursor_flags);
        MU_CHECK(mu_css_read_le(p->cursor_timer,2) <= 1,"cursor_timer",(int)i,-1,mu_css_read_le(p->cursor_timer,2));
        MU_CHECK(mu_css_read_le(p->cursor_repeat,2) <= 31,"cursor_repeat",(int)i,-1,mu_css_read_le(p->cursor_repeat,2));
        MU_PORT(held_token,p->held_token <= 11);
        MU_PORT(cursor_state,p->cursor_state <= 3);
        MU_PORT(token_state,p->token_state <= 4);
        MU_PORT(token_colour,p->token_colour != 6 && (p->token_colour <= 8 || p->token_colour == 255));
        MU_PORT(token_timer,p->token_timer <= 39);
#undef MU_PORT
        for (k = 0; k < 6; ++k) {
            static const char* fields[] = {"cursor_x","cursor_y","token_x","token_y","target_x","target_y"};
            const unsigned char* f = (const unsigned char*) p + 16 + 4*k;
            unsigned long long bits = mu_css_read_le(f,4);
            union { unsigned int u; float f; } value;
            value.u = (unsigned int)bits;
            MU_CHECK((bits & 0x7f800000u) != 0x7f800000u,fields[k],(int)i,-1,value.f);
            /* m-ex MEX_CSS_CURSOR_RIGHT_29 extends the retail right clamp to 29. */
            MU_CHECK(k != 0 || (value.f >= -35.0f && value.f <= 29.0f),fields[k],(int)i,-1,value.f);
            MU_CHECK(k != 1 || (value.f >= -22.0f && value.f <= 25.0f),fields[k],(int)i,-1,value.f);
            MU_CHECK(k < 2 || (k & 1) || (value.f >= -36.0f && value.f <= 32.0f),fields[k],(int)i,-1,value.f);
            MU_CHECK(k < 2 || !(k & 1) || (value.f >= -27.0f && value.f <= 26.0f),fields[k],(int)i,-1,value.f);
        }
    }
#undef MU_FIELD
#undef MU_CHECK
    return 1;
}
static inline int mu_css_valid(const MuCssState* s) { return mu_css_validate(s,0); }
#ifdef __cplusplus
static_assert(sizeof(MuCssIdentity) == 10, "CSS identity bytes");
static_assert(sizeof(MuCssPort) == 51, "CSS port bytes");
static_assert(sizeof(MuCssState) == MU_CSS_SIZE, "CSS state bytes");
#else
_Static_assert(sizeof(MuCssPort) == 51, "CSS port bytes");
_Static_assert(sizeof(MuCssState) == MU_CSS_SIZE, "CSS state bytes");
#endif
#endif
