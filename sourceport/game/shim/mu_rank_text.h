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
        memcpy(out, rank->label, sizeof rank->label);
        out[sizeof rank->label] = 0;
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
