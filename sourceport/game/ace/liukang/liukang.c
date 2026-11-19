/* ACE 2.0.1's Liu Kang: the whole fighter (m-ex "ftFunction" of PlKg.dat). See liukang.h.
 *
 * PlKg.dat exports fourteen ftFunction slots: 0 onload, 3 move_logic, 4 to 11 the special moves,
 * 23 onframe, 24 onactionstatechange, 25 onrespawn and 34 onlanding. Every other slot is MxDt's
 * default, which the registry fills when a field here is NULL. */
#include "liukang.h"

#include "../chunli/chunli.h"

#include <dolphin/os.h>

#include <melee/ft/fighter.h>
#include <melee/ft/ft_0877.h>
#include <melee/ft/ft_081B.h>
#include <melee/ft/ft_084E.h>
#include <melee/ft/ft_0881.h>
#include <melee/ft/ft_0892.h>
#include <melee/ft/ftanim.h>
#include <melee/ft/ftcamera.h>
#include <melee/ft/ftcliffcommon.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/ftCo_Fall.h>
#include <melee/ft/kinds/ftCommon/ftCo_FallSpecial.h>
#include <melee/ft/kinds/ftCommon/ftCo_Landing.h>
#include <melee/ft/kinds/ftSeak/ftseak.h>
#include <melee/ft/types.h>
#include <melee/lb/lb_00B0.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/random.h>

/* The ability Kirby copies (liukang_kirby.c). */
extern const struct MuAkKirbyCopy ftKbKg_Copy;

/* A plain state change: flags 0, frame 0, rate 1, no blend (the form of every change he makes
 * from frame 0). */
static inline void ftKg_Change(HSD_GObj* gobj, FtMotionId msid)
{
    Fighter_ChangeMotionState(gobj, msid, Ft_MF_None, 0.0f, 1.0f, 0.0f, NULL);
}

/* ---- load and events ------------------------------------------------------------------------- */

/* +1814, the accessory callback of 341 and 342: once the script raises the first throw flag,
 * clear it and fire article 0 from the bone of part 54, facing his way (the spawn at +1C28 is
 * Chun-Li's +3258). */
static void ftKg_SpecialN_OnAccessory4(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ItemKind kind;
    Vec3 pos;

    if (!fp->throw_flags_b0) {
        return;
    }
    fp->throw_flags_b0 = false;
    lb_8000B1CC(fp->parts[FTKG_SHOT_PART].joint, NULL, &pos);
    kind = mu_ak_article_kind(gobj, ftKg_Article_Shot);
    itCn_Shot_Spawn(gobj, &pos, kind, fp->facing_dir);
}

/* +0000, [onload]. His attribute block (0x5C bytes) into fp+2D8, which becomes fp+2D4, the four
 * command variables cleared. A line of his (0x13AD) when the damage and the word at fp+186C read
 * as a float add up to 100 or more while the damage is under 100. Then the retail Sheik's onload
 * by its console address (+007C: 0x801100EC), the m-ex CPU helper (Chun-Li's, with the same
 * posing kind 0x1C) and article 0 under his own kind (m-ex MEX_IndexFighterItem). */
static void ftKg_Init_OnLoad(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftKg_DatAttrs* src = (ftKg_DatAttrs*) DP(fp->ft_data->ext_attr);
    DISC_PTR(void)* items = DP(fp->ft_data->x48_items);
    float percent = fp->dmg.x1830_percent;
    float other;
    float sum;

    *(ftKg_DatAttrs*) fp->dat_attrs_backup = *src;
    __builtin_memcpy(&other, &fp->dmg.x186c, sizeof other);
    sum = percent + other;
    fp->dat_attrs = fp->dat_attrs_backup;
    fp->cmd_vars[0] = 0;
    fp->cmd_vars[1] = 0;
    fp->cmd_vars[2] = 0;
    fp->cmd_vars[3] = 0;
    if (sum >= 100.0f && percent < 100.0f) {
        ft_PlaySFX(fp, FTKG_SFX_LOAD, (u8) 0x4B0, 0x32);
    }
    ftSk_Init_OnLoad(gobj);
    ftCn_Cpu_InitSpoofData(gobj);
    mu_ak_register_article(fp->kind, DP(items[ftKg_Article_Shot]), ftKg_Article_Shot);
}

/* +0920, [onactionstatechange]: a ledge catch (252) or DeadDown (0) gives back the air side and
 * down specials. */
static void ftKg_Init_OnActionStateChange(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (fp->motion_id == ftCo_MS_CliffCatch || fp->motion_id == ftCo_MS_DeadDown) {
        ftKg_Vars(fp)->specialairs_used = 0;
        ftKg_Vars(fp)->specialairlw_used = 0;
        ftKg_Vars(fp)->x2248 = 0;
    }
}

/* +094C, [onrespawn]: his attribute block again into fp+2D8. */
static void ftKg_Init_LoadSpecialAttrs(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftKg_DatAttrs* src = (ftKg_DatAttrs*) DP(fp->ft_data->ext_attr);

    *(ftKg_DatAttrs*) fp->dat_attrs_backup = *src;
}

/* +0964, [onlanding]: the air side and down specials are back. */
static void ftKg_Init_OnLanding(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    ftKg_Vars(fp)->specialairs_used = 0;
    ftKg_Vars(fp)->specialairlw_used = 0;
    ftKg_Vars(fp)->x2248 = 0;
}

/* +0620, [onframe]. A random number from 0 to 99 is drawn every frame, used or not.
 *
 * A pressed: the dash attack between frames 15 and 30 goes on as 353 from the same frame; jab 2
 * between frames 8 and 30 goes back to jab 1 at frame 2, and jab 1 on to jab 2 the same way.
 * The checks follow one another as the file's do, each on the state the one before left.
 *
 * Then, in an up smash whose first hitbox is live and has hit, from frame 15 on: one line, by
 * the number drawn (0 to 5 the rare one, 6 to 80 five lines by fifteens, above that the silent
 * id -1). And at frame 29 of common state 0xD2, a line. */
static void ftKg_Init_OnFrame(HSD_GObj* gobj)
{
    static const int lines[5] = { 0x13DD, 0x13E0, 0x13E3, 0x13E6, 0x13E9 };
    Fighter* fp = GET_FIGHTER(gobj);
    const s32 r = HSD_Randi(100);
    FtMotionId m;

    mu_ak_test_motion(fp);
    if (fp->input.pressed_buttons & HSD_PAD_A) {
        m = fp->motion_id;
        if (m == ftCo_MS_AttackDash) {
            /* +0794 */
            if (fp->cur_anim_frame <= 15.0f || fp->cur_anim_frame >= 30.0f) {
                return;
            }
            Fighter_ChangeMotionState(gobj, ftKg_MS_AttackDashA, Ft_MF_None,
                                      GET_FIGHTER(gobj)->cur_anim_frame, 1.0f, 0.0f, NULL);
            ftAnim_8006EBA4(gobj);
            m = fp->motion_id;
            if (!(fp->input.pressed_buttons & HSD_PAD_A)) {
                goto voice;
            }
        }
        if (m == ftCo_MS_Attack12) {
            /* +080C */
            if (fp->cur_anim_frame <= 8.0f || fp->cur_anim_frame >= 30.0f) {
                return;
            }
            Fighter_ChangeMotionState(gobj, ftCo_MS_Attack11, Ft_MF_None, 2.0f, 1.0f, 0.0f,
                                      NULL);
            m = fp->motion_id;
            if (!(fp->input.pressed_buttons & HSD_PAD_A)) {
                goto voice;
            }
        }
        if (m == ftCo_MS_Attack11) {
            /* +06E8 */
            if (fp->cur_anim_frame <= 8.0f || fp->cur_anim_frame >= 30.0f) {
                return;
            }
            Fighter_ChangeMotionState(gobj, ftCo_MS_Attack12, Ft_MF_None, 2.0f, 1.0f, 0.0f,
                                      NULL);
            m = fp->motion_id;
        }
    } else {
        m = fp->motion_id;
    }

voice:
    /* +065C */
    if (m == ftCo_MS_AttackHi4) {
        if (fp->x914[0].victims_1[0].victim == NULL || fp->x914[0].state == 0) {
            return;
        }
        if (!(fp->cur_anim_frame >= 15.0f)) {
            return;
        }
        if (r <= 5) {
            ft_80088478(fp, FTKG_SFX_HI4_RARE, 0xFF, 0x40);
        } else if ((u32) (r - 6) > 0x4A) {
            ft_80088478(fp, -1, 0, 0x40);
        } else {
            ft_80088478(fp, lines[(r - 6) / 15], 0xFF, 0x40);
        }
        m = fp->motion_id;
    }
    /* +0760 */
    if (m == FTKG_MS_VOICE_STATE && fp->cur_anim_frame == 29.0f) {
        ft_80088478(fp, FTKG_SFX_STATE_D2, 0xFF, 0x40);
    }
}

/* ---- the neutral special: Chun-Li's, with his accessory callback ---------------------------- */

/* +02C8, [specialn]. */
static void ftKg_SpecialN_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->throw_flags_b0 = false;
    ftKg_Change(gobj, ftKg_MS_SpecialN);
    ftAnim_8006EBA4(gobj);
    fp->accessory4_cb = ftKg_SpecialN_OnAccessory4;
}

/* +0340, [specialairn]. */
static void ftKg_SpecialAirN_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->throw_flags_b0 = false;
    ftKg_Change(gobj, ftKg_MS_SpecialAirN);
    ftAnim_8006EBA4(gobj);
    fp->accessory4_cb = ftKg_SpecialN_OnAccessory4;
}

/* +0A48 (and +0C2C, +13D8, +1544, +1744, the same instructions): wait when the animation is
 * over. */
static void ftKg_WaitAtEnd_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ft_8008A2BC(gobj);
    }
}

/* +0B18 (and +15E8, +1670): fall when the animation is over. */
static void ftKg_FallAtEnd_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ftCo_Fall_Enter(gobj);
    }
}

/* +0A9C, the collision callback of 341: off the ground, airborne and 342 from the same frame. */
static void ftKg_SpecialN_Coll(HSD_GObj* gobj)
{
    Fighter* fp;

    if (ft_80082708(gobj)) {
        return;
    }
    fp = GET_FIGHTER(gobj);
    ftCommon_8007D5D4(fp);
    Fighter_ChangeMotionState(gobj, ftKg_MS_SpecialAirN, Ft_MF_None, fp->cur_anim_frame, 1.0f,
                              0.0f, NULL);
}

/* +0B6C, the collision callback of 342: landed, grounded and 341 from the same frame; the shot
 * callback is put back before frame 11. */
static void ftKg_SpecialAirN_Coll(HSD_GObj* gobj)
{
    Fighter* fp;

    if (!ft_80081D0C(gobj)) {
        return;
    }
    fp = GET_FIGHTER(gobj);
    ftCommon_8007D7FC(fp);
    Fighter_ChangeMotionState(gobj, ftKg_MS_SpecialN, Ft_MF_None, fp->cur_anim_frame, 1.0f, 0.0f,
                              NULL);
    if (fp->cur_anim_frame < 11.0f) {
        fp->accessory4_cb = ftKg_SpecialN_OnAccessory4;
    }
}

/* An interrupt callback that is a lone blr (+0A94, +0B64, +0C78, +0DD8, +1330, +1424, +14C8,
 * +1590, +1634, +16BC, +1790) and the empty collision callback of 348 (+1458). */
static void ftKg_Empty(HSD_GObj* gobj)
{
    (void) gobj;
}

/* ---- the side special ---------------------------------------------------------------------- */

/* +03B8, [specials]. */
static void ftKg_SpecialS_Enter(HSD_GObj* gobj)
{
    ftKg_Change(gobj, ftKg_MS_SpecialS);
    ftAnim_8006EBA4(gobj);
}

/* +0408, [specialairs]: once per air time. The flag is set after the state change; on the
 * ground (it is not, when the game calls this) the down special's flag would be cleared. */
static void ftKg_SpecialAirS_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ftKg_Vars(fp)->specialairs_used == 1) {
        return;
    }
    ftKg_Change(gobj, ftKg_MS_SpecialAirS);
    ftAnim_8006EBA4(gobj);
    ftKg_Vars(fp)->specialairs_used = 1;
    if (GET_FIGHTER(gobj)->ground_or_air == GA_Ground) {
        ftKg_Vars(GET_FIGHTER(gobj))->specialairlw_used = 0;
    }
}

/* +0C7C (and +0DDC, +1794), physics: Chun-Li's +1A2C. */
static void ftKg_SpecialS_Phys(HSD_GObj* gobj)
{
    ft_80085154(gobj);
    ft_80084FA8(gobj);
}

/* +0CAC, the collision callback of 343: off the ground, 344 from the same frame, airborne. */
static void ftKg_SpecialS_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ft_80082708(gobj)) {
        return;
    }
    Fighter_ChangeMotionState(gobj, ftKg_MS_SpecialAirS, Ft_MF_None, fp->cur_anim_frame, 1.0f,
                              0.0f, NULL);
    ftCommon_8007D5D4(fp);
}

/* +0D34, the animation callback of 344: at the end, the fall; with the air flag set, the special
 * fall first, which stands when the flag is still 1 after it. */
static void ftKg_SpecialAirS_Anim(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftKg_DatAttrs* da = ftKg_Attrs(fp);

    if (ftAnim_IsFramesRemaining(gobj)) {
        return;
    }
    if (ftKg_Vars(fp)->specialairs_used != 0) {
        ftCo_80096900(gobj, 1, 0, false, da->specialhi_freefall_mobility,
                      da->specialhi_landing_lag);
        if (ftKg_Vars(fp)->specialairs_used == 1) {
            return;
        }
    }
    ftCo_Fall_Enter(gobj);
}

/* +0E0C, the collision callback of 344: on the floor (the floor and ledge test with the facing
 * as the direction), 343 from the same frame and grounded; else the ledge catch test. */
static void ftKg_SpecialAirS_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ft_CheckGroundAndLedge(gobj, (int) fp->facing_dir)) {
        Fighter_ChangeMotionState(gobj, ftKg_MS_SpecialS, Ft_MF_None, fp->cur_anim_frame, 1.0f,
                                  0.0f, NULL);
        ftCommon_8007D7FC(fp);
        return;
    }
    ftCliffCommon_80081298(gobj);
}

/* ---- the up special ------------------------------------------------------------------------ */

/* +0490, [specialhi]: state 345, its script, airborne. */
static void ftKg_SpecialHi_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    ftKg_Change(gobj, ftKg_MS_SpecialHi);
    ftAnim_8006EBA4(gobj);
    ftCommon_8007D5D4(fp);
}

/* +04F8, [specialairhi]. */
static void ftKg_SpecialAirHi_Enter(HSD_GObj* gobj)
{
    ftKg_Change(gobj, ftKg_MS_SpecialAirHi);
    ftAnim_8006EBA4(gobj);
}

/* +0EA8 (and +1104), animation: at the end, the special fall with his mobility and lag. */
static void ftKg_SpecialHi_Anim(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftKg_DatAttrs* da = ftKg_Attrs(fp);

    if (!ftAnim_IsFramesRemaining(gobj)) {
        ftCo_80096900(gobj, 1, 0, false, da->specialhi_freefall_mobility,
                      da->specialhi_landing_lag);
    }
}

/* +1040 (and +1180), physics: on the ground the ground step; in the air the two air steps, the
 * simple drift with his two words and the drift. */
static void ftKg_SpecialHi_Phys(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftKg_DatAttrs* da;

    if (fp->ground_or_air != GA_Air) {
        ft_80084FA8(gobj);
        return;
    }
    da = ftKg_Attrs(fp);
    ft_80085154(gobj);
    ft_800851C0(gobj);
    ftCommon_CalcSelfAccel_DriftSimple(fp, 0.0f, da->specialhi_drift_accel,
                                       da->specialhi_drift_max);
    ftCommon_CalcSelfAccel_Drift(fp);
}

/* +1C00 (and +1C14): the landing of the two collision callbacks, the special landing with his
 * lag. */
static void ftKg_SpecialHi_OnLand(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    ftCo_LandingFallSpecial_Enter(gobj, false, ftKg_Attrs(fp)->specialhi_landing_lag);
}

/* +10BC (and +11FC), collision: Chun-Li's +1E0C with his landing. */
static void ftKg_SpecialHi_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (fp->self_vel.y < 0.0f) {
        ft_800831CC(gobj, NULL, ftKg_SpecialHi_OnLand);
        return;
    }
    ft_CheckGroundAndLedge(gobj, (int) fp->facing_dir);
}

/* ---- the down special ---------------------------------------------------------------------- */

/* +0548, [speciallw]. */
static void ftKg_SpecialLw_Enter(HSD_GObj* gobj)
{
    ftKg_Change(gobj, ftKg_MS_SpecialLw);
    ftAnim_8006EBA4(gobj);
}

/* +0598, [specialairlw]: once per air time, as the air side special. */
static void ftKg_SpecialAirLw_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ftKg_Vars(fp)->specialairlw_used == 1) {
        return;
    }
    ftKg_Change(gobj, ftKg_MS_SpecialAirLw);
    ftAnim_8006EBA4(gobj);
    ftKg_Vars(fp)->specialairlw_used = 1;
    if (GET_FIGHTER(gobj)->ground_or_air == GA_Ground) {
        ftKg_Vars(GET_FIGHTER(gobj))->specialairlw_used = 0;
    }
}

/* +1244, the animation callback of 347: off the floor, airborne. At the end of the animation,
 * 348 on the ground, 351 (airborne again) in the air. */
static void ftKg_SpecialLw_Anim(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (!ft_80082708(gobj)) {
        ftCommon_8007D5D4(fp);
    }
    if (ftAnim_IsFramesRemaining(gobj)) {
        return;
    }
    if (fp->ground_or_air != GA_Ground) {
        ftCommon_8007D5D4(GET_FIGHTER(gobj));
        ftKg_Change(gobj, ftKg_MS_SpecialLwFall);
        return;
    }
    ftKg_Change(gobj, ftKg_MS_SpecialLwEnd);
}

/* +1338, the collision callback of 347: off the floor, airborne, and 351 once the animation is
 * over. */
static void ftKg_SpecialLw_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ft_80082708(gobj)) {
        return;
    }
    ftCommon_8007D5D4(fp);
    if (ftAnim_IsFramesRemaining(gobj)) {
        return;
    }
    ftCommon_8007D5D4(GET_FIGHTER(gobj));
    ftKg_Change(gobj, ftKg_MS_SpecialLwFall);
}

/* +1428 (and +1638), physics: Chun-Li's +2468. */
static void ftKg_SpecialLwEnd_Phys(HSD_GObj* gobj)
{
    ftCommon_8007E2FC(gobj);
    ft_80085154(gobj);
}

/* +145C, the animation callback of 349: at the end, 352. */
static void ftKg_SpecialAirLw_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ftKg_Change(gobj, ftKg_MS_SpecialAirLwFall);
    }
}

/* +14D0 (and +16D0), the collision callback of 349 and 352: landed, grounded and 350. */
static void ftKg_SpecialAirLw_Coll(HSD_GObj* gobj)
{
    if (!ft_80081D0C(gobj)) {
        return;
    }
    ftCommon_8007D7FC(GET_FIGHTER(gobj));
    ftKg_Change(gobj, ftKg_MS_SpecialAirLwLanding);
}

/* +1598, the collision callback of 350: a landing grounds him. */
static void ftKg_SpecialAirLwLanding_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ft_80081D0C(gobj)) {
        ftCommon_8007D7FC(fp);
    }
}

/* +1668, the collision callback of 351: airborne, every frame. */
static void ftKg_SpecialLwFall_Coll(HSD_GObj* gobj)
{
    ftCommon_8007D5D4(GET_FIGHTER(gobj));
}

/* +16C0, the physics callback of 352: his gravity. */
static void ftKg_SpecialAirLwFall_Phys(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    ftCommon_Fall(fp, fp->co_attrs.gravity, fp->co_attrs.terminal_velocity);
}

/* +17C4, the collision callback of 353: off the ground (the retail ground test that may not
 * leave an edge), the retail step off. */
static void ftKg_AttackDashA_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (!ft_800827A0(gobj)) {
        ftCommon_8007D6A4(fp);
    }
}

/* ---- the state table (+0128, 13 rows; every word is the file's) ---------------------------- */
static const MotionState ftKg_Init_MotionStateTable[ftKg_MS_SelfCount] = {
    /* 341: physics +0A98, a jump to the retail ft_80084F3C */
    { 295, 0x00341011, 0x12000000, ftKg_WaitAtEnd_Anim, ftKg_Empty, ft_80084F3C,
      ftKg_SpecialN_Coll, ftCamera_UpdateCameraBox },
    /* 342: physics +0B68, a jump to the retail ft_80084DB0 */
    { 296, 0x00341411, 0x12000000, ftKg_FallAtEnd_Anim, ftKg_Empty, ft_80084DB0,
      ftKg_SpecialAirN_Coll, ftCamera_UpdateCameraBox },
    /* 343 */
    { 297, 0x00340013, 0x13000000, ftKg_WaitAtEnd_Anim, ftKg_Empty, ftKg_SpecialS_Phys,
      ftKg_SpecialS_Coll, ftCamera_UpdateCameraBox },
    /* 344 */
    { 298, 0x00340013, 0x13000000, ftKg_SpecialAirS_Anim, ftKg_Empty, ftKg_SpecialS_Phys,
      ftKg_SpecialAirS_Coll, ftCamera_UpdateCameraBox },
    /* 345: the interrupt is Chun-Li's +1C70 */
    { 299, 0x00340013, 0x14000000, ftKg_SpecialHi_Anim, ftCn_SpecialHi_IASA,
      ftKg_SpecialHi_Phys, ftKg_SpecialHi_Coll, ftCamera_UpdateCameraBox },
    /* 346: the interrupt +117C is a jump to 345's */
    { 300, 0x00340413, 0x14000000, ftKg_SpecialHi_Anim, ftCn_SpecialHi_IASA,
      ftKg_SpecialHi_Phys, ftKg_SpecialHi_Coll, ftCamera_UpdateCameraBox },
    /* 347: physics +1334, a jump to the retail ft_80085154 */
    { 301, 0x00340414, 0x15000000, ftKg_SpecialLw_Anim, ftKg_Empty, ft_80085154,
      ftKg_SpecialLw_Coll, ftCamera_UpdateCameraBox },
    /* 348 */
    { 302, 0x00340414, 0x15000000, ftKg_WaitAtEnd_Anim, ftKg_Empty, ftKg_SpecialLwEnd_Phys,
      ftKg_Empty, ftCamera_UpdateCameraBox },
    /* 349: physics +14CC, a jump to the retail ft_80085154 */
    { 303, 0x00340112, 0x15000000, ftKg_SpecialAirLw_Anim, ftKg_Empty, ft_80085154,
      ftKg_SpecialAirLw_Coll, ftCamera_UpdateCameraBox },
    /* 350: physics +1594, a jump to the retail ftCommon_8007E2FC */
    { 304, 0x00340512, 0x15000000, ftKg_WaitAtEnd_Anim, ftKg_Empty, ftCommon_8007E2FC,
      ftKg_SpecialAirLwLanding_Coll, ftCamera_UpdateCameraBox },
    /* 351 */
    { 305, 0x00340112, 0x15000000, ftKg_FallAtEnd_Anim, ftKg_Empty, ftKg_SpecialLwEnd_Phys,
      ftKg_SpecialLwFall_Coll, ftCamera_UpdateCameraBox },
    /* 352 */
    { 306, 0x00340512, 0x15000000, ftKg_FallAtEnd_Anim, ftKg_Empty, ftKg_SpecialAirLwFall_Phys,
      ftKg_SpecialAirLw_Coll, ftCamera_UpdateCameraBox },
    /* 353 */
    { 307, 0x00340512, 0x13000000, ftKg_WaitAtEnd_Anim, ftKg_Empty, ftKg_SpecialS_Phys,
      ftKg_AttackDashA_Coll, ftCamera_UpdateCameraBox },
};

/* His article: Chun-Li's neutral shot table (the itFunction block is hers byte for byte). */
static ItemLogicTable* const itKg_ArticleTables[ftKg_Article_Count] = {
    &itCn_Shot_Logic,
};

/* In progress until its hidden matches pass. */
const MuAkFighter mu_ak_liukang = {
    .name = "Liu Kang",
    .file = "PlKg.dat",

    .onload = ftKg_Init_OnLoad,
    .specialn = ftKg_SpecialN_Enter,
    .specialairn = ftKg_SpecialAirN_Enter,
    .specials = ftKg_SpecialS_Enter,
    .specialairs = ftKg_SpecialAirS_Enter,
    .specialhi = ftKg_SpecialHi_Enter,
    .specialairhi = ftKg_SpecialAirHi_Enter,
    .speciallw = ftKg_SpecialLw_Enter,
    .specialairlw = ftKg_SpecialAirLw_Enter,
    .onframe = ftKg_Init_OnFrame,
    .onactionstatechange = ftKg_Init_OnActionStateChange,
    .onrespawn = ftKg_Init_LoadSpecialAttrs,
    .onlanding = ftKg_Init_OnLanding,

    .move_logic = ftKg_Init_MotionStateTable,
    .move_logic_count = ftKg_MS_SelfCount,

    .flags = MU_AK_READY,
    .article_tables = itKg_ArticleTables,
    .article_count = ftKg_Article_Count,
    .kirby = &ftKbKg_Copy,
};
