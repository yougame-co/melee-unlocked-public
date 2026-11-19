/* The Pit II (ACE 2.0.1 only: internal 139, external 356), the m-ex grFunction block of GrTp2.dat:
 * 107 instructions with symbols. The block, its relocation table (25 entries) and its export table
 * are byte for byte Metal Cavern's (GrOMc.dat, ../metalcavern/metalcavern.c); the symbol names are
 * the block's.
 *
 * Every export is its own (1, 3 to 9), so none of the MxDt row's words (Dream Land 64's callbacks)
 * is kept. Three map gobjs (Points, Background, Floor) created through the m-ex map gobj helper
 * from the block's own table, then the camera range and blast zone. The file has no collision
 * joints.
 *
 * The MxDt row names one stage item (m-ex item kind 253). The file has no itFunction block for it
 * and its itemdata root is a null list, MxDt's logic table of kind 253 is 15 zero words, and the
 * block never spawns an item: the item has no article and no code. */
#include "../common/mu_gr_common.h"

#include <melee/gr/forward.h>

#include <melee/gr/granime.h>
#include <melee/gr/grmaterial.h>
#include <melee/gr/ground.h>
#include <melee/gr/types.h>
#include <melee/it/kinds/types.h>
#include <melee/lb/lb_00F9.h>
#include <sysdolphin/baselib/gobj.h>

/* Map gobj slot 1 (the decomp's callback1, never called by the game) is a lone blr in every entry
 * of this block (+C8, +140, +1B8): it would return its argument, never zero. */
static bool mexgr_tp2_unk(Ground_GObj* gobj)
{
    return gobj != NULL;
}

/* Slot 3 (the decomp's callback3, run when the map gobj is destroyed) is "li r3, 0; blr"
 * (+D0, +164, +1E0). */
static void mexgr_tp2_on_deletion(Ground_GObj* gobj)
{
    (void) gobj;
}

/* Points_OnCreation (+9C): animation set 0. */
static void mexgr_tp2_points_create(Ground_GObj* gobj)
{
    Ground* gp = HSD_GObjGetUserData(gobj);
    grAnime_801C8138(gobj, gp->map_id, 0);
}

/* Background_OnCreation (+D8): animation set 0, x10 bit 2 off, the material flags 0x28000000 on
 * its joints, x11 bits 0 to 2 = 2 (the console writes these as the half word at +0x10). */
static void mexgr_tp2_background_create(Ground_GObj* gobj)
{
    Ground* gp = HSD_GObjGetUserData(gobj);
    grAnime_801C8138(gobj, gp->map_id, 0);
    gp->x10_flags.b2 = 0;
    grMaterial_801C8858(GET_JOBJ(gobj), 0x28000000);
    gp->x11_flags.b012 = 2;
}

/* Background_OnFrame (+144) */
static void mexgr_tp2_background_frame(Ground_GObj* gobj)
{
    Ground_UpdateMapColl(gobj);
}

/* Floor_OnCreation (+16C). The block passes the gobj itself where Ground_InitMapColl takes the
 * joint (an m-ex source slip); the argument is only used for collision joints, which GrTp2.dat has
 * none of (map gobj 2: pointer 0, count 0), so the joint is passed here with the same effect. */
static void mexgr_tp2_floor_create(Ground_GObj* gobj)
{
    Ground* gp = HSD_GObjGetUserData(gobj);
    Ground_InitMapColl(GET_JOBJ(gobj), gp->map_id);
    grAnime_801C8138(gobj, gp->map_id, 0);
}

/* Floor_OnFrame (+1BC) */
static void mexgr_tp2_floor_frame(Ground_GObj* gobj)
{
    Ground_UpdateMapColl(gobj);
    lb_800115F4();
}

/* map_gobjs (+0): OnCreation, OnUnk, OnFrame, OnDeletion, flags */
static StageCallbacks mexgr_tp2_map[3] = {
    { mexgr_tp2_points_create, mexgr_tp2_unk, mu_gr_gobj_nop, mexgr_tp2_on_deletion, 0 },
    { mexgr_tp2_background_create, mexgr_tp2_unk, mexgr_tp2_background_frame, mexgr_tp2_on_deletion, 0 },
    { mexgr_tp2_floor_create, mexgr_tp2_unk, mexgr_tp2_floor_frame, mexgr_tp2_on_deletion, 0xC0000000 },
};

/* OnInit (+3C) */
static void mexgr_tp2_init(void)
{
    mu_grTSeak_80223908(0);
    mu_grTSeak_80223908(1);
    mu_grTSeak_80223908(2);
    Ground_801C39C0();
    Ground_801C3BB4();
}

/* Stage item 0 (kind 253): no itFunction block in the file. */
static const ItemLogicTable mexit_tp2_items[1] = { { 0 } };

const MuGrStage mu_gr_the_pit_2 = {
    .name = "The Pit II",
    .file = "GrTp2.dat",
    .map_gobjs = mexgr_tp2_map,
    .on_init = mexgr_tp2_init,
    .on_demo_init = mu_gr_demo_nop,
    .on_load = mu_gr_event_nop,
    .on_start = mu_gr_event_nop,
    .callback4 = mu_gr_false,
    .on_touch_line = mu_gr_no_touch_line,
    .on_check_shadow_render = mu_gr_shadow_always,
    .items = mexit_tp2_items,
    .item_count = 1,
    .flags = MU_GR_READY,
};
