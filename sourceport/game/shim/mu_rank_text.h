/* Ranked UI needs editable SIS storage and CSS font/camera context 0.
 * A static text from 803A5ACC has alloc_data == NULL and cannot take subtexts. */
#ifndef MU_RANK_TEXT_H
#define MU_RANK_TEXT_H
#include <mu_ranked_wire.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/gobj.h>
#include <stdio.h>
#include <string.h>

/* Host labels remain unchanged on the wire. The retail SIS encoder treats an
 * ASCII slash as a Shift-JIS lead byte and consumes the next character. */
static void mu_rank_text_label(char out[32], const MuYgRank* rank)
{
    if (rank->known && !rank->placed) {
        unsigned total = rank->placement_total ? rank->placement_total : 5;
        unsigned matches = rank->matches < total ? rank->matches : total;
        sprintf(out, "Placements %u of %u", matches, total);
    } else {
        char* slash;
        memcpy(out, rank->label, sizeof rank->label);
        out[sizeof rank->label] = 0;
        /* The host's label filter keeps '/' (for "Placements 2/5"); a placed label must not. */
        while ((slash = strchr(out, '/'))) *slash = '-';
    }
}

/* A rating in tenths as the CSS panel and the end-of-set panel both show it: "1008", "1534.7",
 * "-25". The tenth appears only when it is not zero. */
static inline void mu_rank_number(char* out, long long tenths)
{
    unsigned long long v = tenths < 0 ? -tenths : tenths;
    if (v % 10) sprintf(out, "%s%llu.%u", tenths < 0 ? "-" : "", v/10, (unsigned)(v%10));
    else sprintf(out, "%s%llu", tenths < 0 ? "-" : "", v/10);
}

/* Tier colors as ranks.ts draws them: none, bronze, silver, gold, platinum, diamond, challenger. */
static inline unsigned mu_rank_text_rgb(unsigned tier)
{
    static const unsigned rgb[] = {0xb5c0cd, 0xc98a5b, 0xb5c0cd, 0xf0c040, 0x5fd8c8, 0x8cc8ff, 0xffc857};
    return rgb[tier <= 6 ? tier : 0];
}

/* The SIS encoder has no ASCII '+' (it reads it as half of a two-byte character and drops it);
 * the font has the full-width plus. It has no arrow or middle dot either. */
#define MU_RANK_PLUS "\x81\x7B"

/* When the room gives this seat the set because the opponent left (CTO 11-01): a headline with
 * the games from this seat's side, the standing as the CSS reveal shows it (a promotion spelled
 * out), and "Rating N  +D". Zero for anything else, so the caller keeps DISCONNECTED. */
static int mu_rank_text_forfeit(char headline[64], char standing[80], char rating[48],
                                const MuYgResult* r, unsigned mine, unsigned theirs)
{
    char before[32], after[32], current[24], delta[24];
    const MuYgRank *a = &r->rank_before, *b = &r->rank_after;
    if (r->status != MU_YG_RESULT_RECORDED || r->reason != MU_YG_REASON_FORFEIT_WON || !b->known) return 0;
    if (mine || theirs) sprintf(headline, "Opponent left - you win the set %u:%u", mine, theirs);
    else strcpy(headline, "Opponent left - you win the set");
    mu_rank_text_label(before, a);
    mu_rank_text_label(after, b);
    if (a->known && a->placed && b->placed && b->order > a->order)
        sprintf(standing, "Promoted from %s to %s", before, after);
    else if (a->known && !a->placed && b->placed) sprintf(standing, "Placed in %s", after);
    else strcpy(standing, after);
    mu_rank_number(current, r->after);
    mu_rank_number(delta, (long long)r->after - r->before);
    sprintf(rating, "Rating %s  %s%s", current, r->after > r->before ? MU_RANK_PLUS : "", delta);
    return 1;
}

static HSD_Text* mu_rank_text_create(void)
{
    sislib_UnkAlloc3* camera = HSD_SisLib_804D797C;
    HSD_Text* text;
    if (!HSD_SisLib_804D1124[0]) return NULL;
    while (camera && camera->xA != 0) camera = camera->x0;
    if (!camera) return NULL;
    text = HSD_SisLib_803A6754(0, 0);
    if (!text) return NULL;
    if (!text->alloc_data || !text->alloc_data->data ||
        !text->alloc_data->end || !text->sis_buffer || !text->entity ||
        !text->entity->user_data_remove_func) {
        HSD_SisLib_803A5CC4(text);
        return NULL;
    }
    return text;
}
#endif
