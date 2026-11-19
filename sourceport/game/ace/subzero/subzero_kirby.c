/* ACE 2.0.1's Sub-Zero: the ability Kirby copies (m-ex "kbFunction" and "itFunction" of
 * PlKbCpSz.dat).
 *
 * The hat file exports six kbFunction slots: 0 and 1 (the hat on and off, the same instructions
 * as Chun-Li's hat, ace/chunli/chunli_kirby.c), 2 and 3 (the neutral special on the ground and
 * in the air), 5 (InitCopyItems) and 6 (the two states). Its one article is Chun-Li's neutral
 * shot byte for byte (her table, itCn_Shot_Logic) and its spawn (+0488) is her fighter's
 * (PlCn.dat +3258: the creator is Item_80268B18, where her hat's own is Item_8026862C).
 *
 * The ability is Sub-Zero's neutral special: on the script's flag, the shot from the bone of
 * Kirby's part 31, under Sub-Zero's item kind for article 0. InitCopyItems hands the hat file's
 * own copy of the article to the item code under that kind, in the place of the one Sub-Zero's
 * load gave it.
 *
 * Offsets in the comments (+0260) are offsets into the kbFunction block of PlKbCpSz.dat. */
#include "subzero.h"

#include "../../akaneia/common/mu_ak_kirby.h"
#include "../chunli/chunli.h"

#include <melee/ft/ft_081B.h>
#include <melee/ft/ft_084E.h>
#include <melee/ft/ft_0892.h>
#include <melee/ft/ftanim.h>
#include <melee/ft/ftcamera.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/ftCo_Fall.h>
#include <melee/ft/kinds/ftKirby/ftkirby.h>
#include <melee/ft/types.h>
#include <melee/lb/lb_00B0.h>

/* The part of Kirby whose bone the shot starts from (+02A4: lwz r3, 0x1F0(r10), fp->parts +
 * 31 * 0x10 on the console) and the article asked for (+02B0: li r4, 0). */
#define FTKBSZ_SHOT_PART 31
#define FTKBSZ_ARTICLE 0

/* +020C, [InitCopyItems]: the article at +0C of the hat data becomes article 0 of the copied
 * kind (m-ex MEX_IndexFighterItem). */
static void ftKbSz_InitCopyItems(FighterKind kind, KirbyHatStruct* hat)
{
    mu_ak_register_article(kind, DISC_GET(Article, hat->hat_dynamics[0]), FTKBSZ_ARTICLE);
}

/* +0260, the accessory callback of the two states: once the script raises the first throw flag,
 * clear it and fire the shot from the bone of part 31, facing Kirby's way. */
static void ftKbSz_OnAccessory4(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ItemKind kind;
    Vec3 pos;

    if (!fp->throw_flags_b0) {
        return;
    }
    fp->throw_flags_b0 = false;
    lb_8000B1CC(fp->parts[FTKBSZ_SHOT_PART].joint, NULL, &pos);
    kind = mu_ak_article_kind(gobj, FTKBSZ_ARTICLE);
    itCn_Shot_Spawn(gobj, &pos, kind, fp->facing_dir);
}

/* +011C, [SpecialN]: the flag cleared, state 0 from frame 0, its script, the callback. */
static void ftKbSz_SpecialN_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->throw_flags_b0 = false;
    mu_ak_kirby_state_change(gobj, 0, 0, 0.0f, 1.0f, 0.0f);
    ftAnim_8006EBA4(gobj);
    fp->accessory4_cb = ftKbSz_OnAccessory4;
}

/* +0194, [SpecialAirN]: the same with state 1. */
static void ftKbSz_SpecialAirN_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->throw_flags_b0 = false;
    mu_ak_kirby_state_change(gobj, 1, 0, 0.0f, 1.0f, 0.0f);
    ftAnim_8006EBA4(gobj);
    fp->accessory4_cb = ftKbSz_OnAccessory4;
}

/* +02E8, the animation callback of state 0: wait when the animation is over. */
static void ftKbSz_SpecialN_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ft_8008A2BC(gobj);
    }
}

/* +0338, the collision callback of state 0: off the ground, airborne and state 1 from the same
 * frame. The accessory callback is not set again. */
static void ftKbSz_SpecialN_Coll(HSD_GObj* gobj)
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
static void ftKbSz_SpecialAirN_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ftCo_Fall_Enter(gobj);
    }
}

/* +0408, the collision callback of state 1: landed, grounded and state 0 from the same frame.
 * The accessory callback is not set again (Chun-Li's hat sets hers before frame 15). */
static void ftKbSz_SpecialAirN_Coll(HSD_GObj* gobj)
{
    Fighter* fp;

    if (!ft_80081D0C(gobj)) {
        return;
    }
    fp = GET_FIGHTER(gobj);
    ftCommon_8007D7FC(fp);
    mu_ak_kirby_state_change(gobj, 0, 0, fp->cur_anim_frame, 1.0f, 0.0f);
}

/* +0218, [move_logic]: two rows, every word the file's. The interrupt words are zero; the
 * physics callbacks +0334 and +0404 are jumps to the retail ft_80084F3C and ft_80084DB0. */
static const MotionState ftKbSz_States[] = {
    { 0, 0x00340211, 0x12000000, ftKbSz_SpecialN_Anim, NULL, ft_80084F3C, ftKbSz_SpecialN_Coll,
      ftCamera_UpdateCameraBox },
    { 1, 0x00340611, 0x12000000, ftKbSz_SpecialAirN_Anim, NULL, ft_80084DB0,
      ftKbSz_SpecialAirN_Coll, ftCamera_UpdateCameraBox },
};

const MuAkKirbyCopy ftKbSz_Copy = {
    .on_swallow = mu_ak_kirby_attach_hat,   /* +0000 */
    .on_lose = ftKb_SpecialN_800EFAF0,      /* +00C0 */
    .special_n = ftKbSz_SpecialN_Enter,
    .special_air_n = ftKbSz_SpecialAirN_Enter,
    .init_items = ftKbSz_InitCopyItems,
    .move_logic = ftKbSz_States,
    .move_logic_count = 2,
};
