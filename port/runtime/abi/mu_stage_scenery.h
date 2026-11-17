/* Stage map gobjs that are scenery, for the looks that keep only the play plane (gx_core.h
 * lightweight_background): trees, backdrops and props a stage draws on the fighters' own depth, which
 * the renderer cannot tell from floors by depth. One table for both engines: the Source Port's game
 * tags its stage draws with it (shim/mu_stage_scenery.c, token 0xF7), the Static Recomp's render
 * observer reads the drawing gobj's map index from guest memory (gx/render_observer.cpp).
 * C and C++. Display only. */
#pragma once

/* Internal stage kinds (GrKind) of the stages listed here. */
#define MU_GR_KIND_IZUMI 0x0C      /* Fountain of Dreams */
#define MU_GR_KIND_STORY 0x0A      /* Yoshi's Story */
#define MU_GR_KIND_OLDPUPUPU 0x1C  /* Dream Land N64 */

/* (internal stage, map gobj index) pairs that are scenery. Filled from the looks audit: which map
 * gobjs Lighter kept that are not floors, walls, platforms or Randall. */
static const struct MuStageScenery { int grkind; int map_id; } mu_stage_scenery_table[] = {
    { MU_GR_KIND_OLDPUPUPU, 3 }, { MU_GR_KIND_OLDPUPUPU, 5 }, { MU_GR_KIND_OLDPUPUPU, 7 },
    { MU_GR_KIND_IZUMI, 1 },
    { MU_GR_KIND_STORY, 1 },
};

/* Map gobjs that hold the floor together with props standing close behind it (Yoshi's Story's trees,
 * Fountain of Dreams' tower and star): a piece of these that lies wholly behind the play plane is
 * scenery from a few units back, where elsewhere the rule waits for kLightweightBackgroundDepth. */
static const struct MuStageScenery mu_stage_tight_table[] = {
    { MU_GR_KIND_STORY, 3 },
    { MU_GR_KIND_IZUMI, 3 },
};

static inline int mu_stage_map_is_tight(int grkind, int map_id)
{
    unsigned i;
    for (i = 0; i < sizeof mu_stage_tight_table / sizeof mu_stage_tight_table[0]; i++) {
        if (mu_stage_tight_table[i].grkind == grkind && mu_stage_tight_table[i].map_id == map_id) {
            return 1;
        }
    }
    return 0;
}

static inline int mu_stage_map_is_scenery(int grkind, int map_id)
{
    unsigned i;
    for (i = 0; i < sizeof mu_stage_scenery_table / sizeof mu_stage_scenery_table[0]; i++) {
        if (mu_stage_scenery_table[i].grkind == grkind && mu_stage_scenery_table[i].map_id == map_id) {
            return 1;
        }
    }
    return 0;
}
