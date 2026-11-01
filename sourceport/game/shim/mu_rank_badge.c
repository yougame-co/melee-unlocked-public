/* YouGame TierBadge.tsx, baked by tools/rasterize_rank_badges.py.
 * GPL-3.0-or-later. No Slippi art. The scene owns each SIS entity; its
 * destructor also unlinks/frees the badge, including on an abrupt CSS exit. */
#include <mu_rank_badge.h>
#include "mu_rank_text.h"
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/sislib.h>
#include <dolphin/gx.h>
#include <math.h>
#include <string.h>
#include "assets/mu_rank_badges.h"

struct MuRankBadge {
    HSD_Text* text;
    struct MuRankBadge* next;
    void (*text_destroy)(void*);
    MuYgRank rank;
    float x, y, size, offset, scale, alpha, glow;
    unsigned age;
    int subtext, art_only;
};
static MuRankBadge* badges;

/* Native v1 has no OS accessibility preference in its host ABI. This explicit
 * presentation-only override applies to the reveal AND all Challenger badges.
 * It reads this one named flag; never enumerate the process environment. */
char* getenv(const char* name);
int mu_rank_badge_reduced_motion(void)
{
    const char* flag = getenv("MU_REDUCED_MOTION");
    return flag && (strcmp(flag, "1") == 0 || strcmp(flag, "true") == 0);
}

static MuRankBadge* badge_for_text(void* text)
{
    MuRankBadge* b;
    for (b = badges; b; b = b->next) if (b->text == text) return b;
    return NULL;
}

static void badge_destroy(void* data)
{
    MuRankBadge** link = &badges;
    while (*link) {
        MuRankBadge* b = *link;
        if (b->text == data) {
            void (*destroy)(void*) = b->text_destroy;
            *link = b->next;
            HSD_Free(b);
            destroy(data);
            return;
        }
        link = &b->next;
    }
}

static void badge_tick(HSD_GObj* gobj)
{
    MuRankBadge* b = badge_for_text(gobj->user_data);
    /* 16s spin and 2.8s pulse share a 6720-frame period at 60 Hz. */
    if (b) b->age = mu_rank_badge_reduced_motion() ? 0 : (b->age+1) % 6720;
}

static void badge_quad(MuRankBadge* b, const unsigned char* image,
                       float factor, float angle, GXColor color)
{
    GXTexObj tex;
    const float uv[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    float half = 12*b->size*b->scale*factor;
    float cx = b->x + 12*b->size, cy = b->y + b->offset + 12*b->size;
    float sn = sinf(angle), cs = cosf(angle);
    unsigned i;
    GXSetTevColor(GX_TEVREG0, color);
    if (image) {
        GXInitTexObj(&tex, (void*)image, MU_RANK_BADGE_SIZE, MU_RANK_BADGE_SIZE,
                     GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&tex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&tex, GX_TEXMAP0);
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A0, GX_CA_ZERO);
    } else {
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    }
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (i = 0; i < 4; ++i) {
        float x = (uv[i][0]*2-1)*half, y = (uv[i][1]*2-1)*half;
        GXPosition3f32(cx + x*cs-y*sn, -(cy + x*sn+y*cs), b->text->pos_z);
        GXTexCoord2f32(uv[i][0], uv[i][1]);
    }
    GXEnd();
}

/* SIS calls this with its camera, vertex layout, blend and texture state set.
 * It restores the glyph TEV inputs and loads glyph textures after this call. */
static void badge_draw(void* object)
{
    HSD_GObj* gobj = object;
    MuRankBadge* b = badge_for_text(gobj->user_data);
    GXColor white = {255,255,255,255};
    unsigned tier, index;
    if (!b || b->alpha <= 0) return;
    /* Nothing during placements or while loading: the dashed placement ring read as a loading
     * spinner on the CSS, the setup screen and the reveal (build 23, 10-31). The text says
     * "Placements N of T" instead. */
    if (!b->rank.known || !b->rank.placed) return;
    tier = b->rank.known && b->rank.tier <= 6 ? b->rank.tier : 0;
    white.a = (u8)(b->alpha*255);
    if (b->glow > 0) {
        unsigned rgb = mu_rank_tier_rgb[tier];
        GXColor glow = {rgb >> 16, rgb >> 8, rgb, (u8)(55*b->glow*b->alpha)};
        badge_quad(b, NULL, 1.25f, 0, glow);
    }
    if (tier == 6 && !mu_rank_badge_reduced_motion()) {
        float pulse = .93f + .07f*cosf((float)(b->age % 168)*6.283185307f/168);
        badge_quad(b, mu_badge_challenger_rays, 1, (float)(b->age % 960)*6.283185307f/960, white);
        badge_quad(b, mu_badge_challenger_core, 1, 0, white);
        badge_quad(b, mu_badge_challenger_star, pulse, 0, white);
    } else {
        index = tier == 6 ? 12 : tier*2 + (b->rank.known && b->rank.placed ? 1 : 0);
        badge_quad(b, mu_badge_textures[index], 1, 0, white);
    }
}

MuRankBadge* mu_rank_badge_create(struct HSD_GObj* parent, float x, float y, float scale, const MuYgRank* rank)
{
    MuRankBadge* b;
    /* SIS owns the drawing entity independently, as in the shared fallback.
     * Callers may free before their parent; scene teardown also frees it. */
    (void)parent;
    if (scale <= 0) return NULL;
    b = HSD_MemAlloc(sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof *b);
    b->text = mu_rank_text_create();
    if (!b->text) { HSD_Free(b); return NULL; }
    b->text->pos_x = x; b->text->pos_y = y;
    b->text->box_size_x = b->text->box_size_y = 20;
    b->x = x; b->y = y; b->size = scale; b->scale = b->alpha = 1;
    b->text->x34.x = b->text->x34.y = .35f*scale;
    b->text->default_kerning = 1;
    b->subtext = HSD_SisLib_803A6B98(b->text, 80, 6, "%s", "");
    b->text_destroy = b->text->entity->user_data_remove_func;
    b->text->entity->user_data_remove_func = badge_destroy;
    b->text->render_callback = badge_draw;
    HSD_GObj_SetupProc(b->text->entity, badge_tick, 4);
    b->next = badges; badges = b;
    mu_rank_badge_set(b, rank);
    return b;
}

void mu_rank_badge_set(MuRankBadge* b, const MuYgRank* rank)
{
    static const char* const roman[] = {"", "I", "II", "III", "IV"};
    const char* numeral = "";
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
    char practice[32] = "Practice ";
#endif
    if (!b) return;
    if (rank) b->rank = *rank;
    else memset(&b->rank, 0, sizeof b->rank);
    if (!b->rank.known) numeral = "?";
    else if (b->rank.placed && b->rank.tier >= 1 && b->rank.tier <= 5 && b->rank.division <= 4)
        numeral = roman[b->rank.division];
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
    if (strncmp(b->rank.label, "Practice ", 9) == 0) {
        strcat(practice, numeral);
        numeral = practice;
    }
#endif
    HSD_SisLib_803A70A0(b->text, b->subtext, "%s", b->art_only ? "" : numeral);
}

/* CSS has a separate full label; keep enlarged art within its column rather
 * than repeating the division and Practice word on the badge's right edge. */
void mu_rank_badge_art_only(MuRankBadge* b)
{
    if (!b) return;
    b->art_only = 1;
    HSD_SisLib_803A70A0(b->text, b->subtext, "%s", "");
}

/* Private to Track C: offsets in SIS world units; other callers retain the
 * shared create/set/free interface. Alpha/scale affect the division too. */
void mu_rank_badge_pose(MuRankBadge* b, float y, float scale, float alpha, float glow)
{
    GXColor white = {255,255,255,255};
    if (!b) return;
    b->offset = y; b->scale = scale; b->alpha = alpha; b->glow = glow;
    b->text->pos_y = b->y+y;
    b->text->x34.x = b->text->x34.y = .35f*b->size*scale;
    white.a = (u8)(alpha*255);
    HSD_SisLib_803A74F0(b->text, b->subtext, &white);
}

void mu_rank_badge_free(MuRankBadge* b)
{
    if (b) HSD_SisLib_803A5CC4(b->text);
}
