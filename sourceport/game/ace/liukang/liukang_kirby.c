/* ACE 2.0.1's Liu Kang: the ability Kirby copies (m-ex "kbFunction" and "itFunction" of
 * PlKbCpKg.dat).
 *
 * The hat file's kbFunction block (0x5C0 bytes, exports 0 to 3 and 6) and its one article block
 * (article 1) are Chun-Li's hat file's byte for byte, relocations and exports included
 * (PlKbCpCn.dat, ace/chunli/chunli_kirby.c): the hat on and off, the neutral special on the
 * ground and in the air, two states, and a shot from the bone of Kirby's part 26 on the
 * script's flag. It exports no InitCopyItems.
 *
 * The shot asks m-ex for article 1 of the copied fighter (+02B0: li r4, 1). Chun-Li's item
 * list has four entries and her load registers them; Liu Kang's has one (MxDt item lookup:
 * 352 only) and his load registers article 0 only, so for this hat the request has no entry.
 * What the console's lookup returns past the end of a list is not in this file. Here the
 * lookup answers -1 and no shot is created: the ability plays its states and its script and
 * fires nothing. The article's table would be Chun-Li's hat's (ftKbCn_ShotLogic), were there
 * a kind to give it.
 *
 * Offsets in the comments (+0260) are offsets into the kbFunction block of PlKbCpKg.dat. */
#include "liukang.h"

#include "../../akaneia/common/mu_ak_kirby.h"

#include <dolphin/os.h>

#include <melee/db/db.h>
#include <melee/ft/ft_081B.h>
#include <melee/ft/ft_084E.h>
#include <melee/ft/ft_0892.h>
#include <melee/ft/ftanim.h>
#include <melee/ft/ftcamera.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/ftCo_Fall.h>
#include <melee/ft/kinds/ftKirby/ftkirby.h>
#include <melee/ft/types.h>
#include <melee/it/it_26B1.h>
#include <melee/it/it_2725.h>
#include <melee/it/item.h>
#include <melee/lb/lb_00B0.h>

/* The part of Kirby whose bone the shot starts from (+02A4: lwz r3, 0x1A0(r10), fp->parts +
 * 26 * 0x10 on the console) and the article asked for (+02B0: li r4, 1). */
#define FTKBKG_SHOT_PART 26
#define FTKBKG_ARTICLE 1

/* +04C4, from the accessory callback (+02CC). Chun-Li's hat's spawn: the position the owner's
 * middle, the previous position the bone's, on the plane, no speed, the facing, the first
 * collision flag, the retail creator that keeps the caller's ground state (Item_8026862C); then
 * it_8026B3A8 on the new item, the owner for the debug display and it_802750F8.
 *
 * Native additions: a kind the lookup does not have (see the top of the file) and an item that
 * was not created stop here. The console tests neither. */
static void ftKbKg_Shot_Spawn(HSD_GObj* owner_gobj, Vec3* pos, ItemKind kind, float facing_dir)
{
    SpawnItem spawn = { 0 };
    Item_GObj* item_gobj;

    if ((int) kind < 0) {
        static int logged;
        if (!logged) {
            logged = 1;
            OSReport("[ak] Kirby's Liu Kang shot: the copied fighter has no article %d\n",
                     FTKBKG_ARTICLE);
        }
        return;
    }
    ftLib_80086990(owner_gobj, &spawn.pos);
    spawn.x0_parent_gobj = owner_gobj;
    spawn.x4_parent_gobj2 = owner_gobj;
    spawn.kind = kind;
    spawn.prev_pos.x = pos->x;
    spawn.prev_pos.y = pos->y;
    spawn.facing_dir = facing_dir;
    spawn.x44_flag.b0 = true;
    item_gobj = mu_ak_item_create(&spawn);
    if (item_gobj == NULL) {
        return;
    }
    it_8026B3A8(item_gobj);
    db_80225DD8(item_gobj, owner_gobj);
    it_802750F8(item_gobj);
}

/* +0260, the accessory callback of the two states: once the script raises the first throw flag,
 * clear it and fire article 1 from the bone of part 26, facing Kirby's way. */
static void ftKbKg_OnAccessory4(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ItemKind kind;
    Vec3 pos;

    if (!fp->throw_flags_b0) {
        return;
    }
    fp->throw_flags_b0 = false;
    lb_8000B1CC(fp->parts[FTKBKG_SHOT_PART].joint, NULL, &pos);
    kind = mu_ak_article_kind(gobj, FTKBKG_ARTICLE);
    ftKbKg_Shot_Spawn(gobj, &pos, kind, fp->facing_dir);
}

/* +011C, [SpecialN]: the flag cleared, state 0 from frame 0, its script, the callback. */
static void ftKbKg_SpecialN_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->throw_flags_b0 = false;
    mu_ak_kirby_state_change(gobj, 0, 0, 0.0f, 1.0f, 0.0f);
    ftAnim_8006EBA4(gobj);
    fp->accessory4_cb = ftKbKg_OnAccessory4;
}

/* +0194, [SpecialAirN]: the same with state 1. */
static void ftKbKg_SpecialAirN_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->throw_flags_b0 = false;
    mu_ak_kirby_state_change(gobj, 1, 0, 0.0f, 1.0f, 0.0f);
    ftAnim_8006EBA4(gobj);
    fp->accessory4_cb = ftKbKg_OnAccessory4;
}

/* +02E8, the animation callback of state 0: wait when the animation is over. */
static void ftKbKg_SpecialN_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ft_8008A2BC(gobj);
    }
}

/* +0338, the collision callback of state 0: off the ground, airborne and state 1 from the same
 * frame. The accessory callback is not set again. */
static void ftKbKg_SpecialN_Coll(HSD_GObj* gobj)
{
    Fighter* fp;

    if (ft_80082708(gobj)) {
        return;
    }
    fp = GET_FIGHTER(gobj);
    ftCommon_8007D5D4(fp);
    mu_ak_kirby_state_change(gobj, 1, 0, fp->cur_anim_frame, 1.0f, 0.0f);
}

/* +03B8, the animation callback of state 1: fall when the animation is over. */
static void ftKbKg_SpecialAirN_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ftCo_Fall_Enter(gobj);
    }
}

/* +0408, the collision callback of state 1: landed, grounded and state 0 from the same frame;
 * the accessory callback is put back unless the frame is 15 or more. */
static void ftKbKg_SpecialAirN_Coll(HSD_GObj* gobj)
{
    Fighter* fp;

    if (!ft_80081D0C(gobj)) {
        return;
    }
    fp = GET_FIGHTER(gobj);
    ftCommon_8007D7FC(fp);
    mu_ak_kirby_state_change(gobj, 0, 0, fp->cur_anim_frame, 1.0f, 0.0f);
    if (!(fp->cur_anim_frame >= 15.0f)) {
        fp->accessory4_cb = ftKbKg_OnAccessory4;
    }
}

/* +020C, [move_logic]: two rows, every word the file's. The interrupt words are zero; the
 * physics callbacks +0334 and +0404 are jumps to the retail ft_80084F3C and ft_80084DB0. */
static const MotionState ftKbKg_States[] = {
    { 0, 0x00340211, 0x12000000, ftKbKg_SpecialN_Anim, NULL, ft_80084F3C, ftKbKg_SpecialN_Coll,
      ftCamera_UpdateCameraBox },
    { 1, 0x00340611, 0x12000000, ftKbKg_SpecialAirN_Anim, NULL, ft_80084DB0,
      ftKbKg_SpecialAirN_Coll, ftCamera_UpdateCameraBox },
};

const MuAkKirbyCopy ftKbKg_Copy = {
    .on_swallow = mu_ak_kirby_attach_hat,   /* +0000 */
    .on_lose = ftKb_SpecialN_800EFAF0,      /* +00C0 */
    .special_n = ftKbKg_SpecialN_Enter,
    .special_air_n = ftKbKg_SpecialAirN_Enter,
    .move_logic = ftKbKg_States,
    .move_logic_count = 2,
};
