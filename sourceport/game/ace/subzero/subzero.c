/* ACE 2.0.1's Sub-Zero: the whole fighter (m-ex "ftFunction" of PlSz.dat). See subzero.h.
 *
 * PlSz.dat exports fourteen ftFunction slots: 0 onload, 3 move_logic, 4 to 11 the special moves,
 * 23 onframe, 24 onactionstatechange, 25 onrespawn and 34 onlanding. Every other slot is MxDt's
 * default, which the registry fills when a field here is NULL. */
#include "subzero.h"

#include "../chunli/chunli.h"

#include <melee/ft/fighter.h>
#include <melee/ft/ft_081B.h>
#include <melee/ft/ft_084E.h>
#include <melee/ft/ft_0881.h>
#include <melee/ft/ft_0892.h>
#include <melee/ft/ftanim.h>
#include <melee/ft/ftcamera.h>
#include <melee/ft/ftcliffcommon.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/ftparts.h>
#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftCommon/ftCo_Fall.h>
#include <melee/ft/kinds/ftCommon/ftCo_FallSpecial.h>
#include <melee/ft/kinds/ftCommon/ftCo_Landing.h>
#include <melee/ft/types.h>
#include <melee/lb/lb_00B0.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/random.h>

/* The double constants of the block: +1C50 (0.0174533, the degree-to-radian factor of the down
 * special's tilt) and +1C58 (pi / 2, the turn of 350). */
static const double ftSz_DegToRad = 0.0174533;
static const double ftSz_HalfPi = 1.5707963267948966;

/* A plain state change: flags 0, frame 0, rate 1, no blend (the form of every change he makes
 * from frame 0). */
static inline void ftSz_Change(HSD_GObj* gobj, FtMotionId msid)
{
    Fighter_ChangeMotionState(gobj, msid, Ft_MF_None, 0.0f, 1.0f, 0.0f, NULL);
}

/* ---- load and events ------------------------------------------------------------------------- */

/* +1768, the accessory callback of 341 and 342: once the script raises the first throw flag,
 * clear it and fire article 0 from the bone of part 54, facing his way (the spawn at +1C84 is
 * Chun-Li's +3258). The same instructions as Liu Kang's +1814. */
static void ftSz_SpecialN_OnAccessory4(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ItemKind kind;
    Vec3 pos;

    if (!fp->throw_flags_b0) {
        return;
    }
    fp->throw_flags_b0 = false;
    lb_8000B1CC(fp->parts[FTSZ_SHOT_PART].joint, NULL, &pos);
    kind = mu_ak_article_kind(gobj, ftSz_Article_Shot);
    itCn_Shot_Spawn(gobj, &pos, kind, fp->facing_dir);
}

/* +17E4, the accessory callback the down special's two entries set: once the script raises the
 * first throw flag, clear it and create article 1 at the bone of part 4, facing his way. */
static void ftSz_SpecialLw_OnAccessory4(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ItemKind kind;
    Vec3 pos;

    if (!fp->throw_flags_b0) {
        return;
    }
    fp->throw_flags_b0 = false;
    lb_8000B1CC(fp->parts[FTSZ_LW_PART].joint, NULL, &pos);
    kind = mu_ak_article_kind(gobj, ftSz_Article_Lw);
    itSz_Lw_Spawn(gobj, &pos, kind, fp->facing_dir);
}

/* +0000, [onload]. His attribute block (0xAC bytes) into fp+2D8, which becomes fp+2D4, the four
 * command variables cleared. Then the m-ex CPU helper (Chun-Li's, with the same posing kind
 * 0x1C at +00A4; the word at +091C that would skip it is never written) and articles 0 and 1
 * under his own kinds (m-ex MEX_IndexFighterItem). He does not call the retail Sheik's onload,
 * which Liu Kang and Chun-Li do. */
static void ftSz_Init_OnLoad(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* src = (ftSz_DatAttrs*) DP(fp->ft_data->ext_attr);
    DISC_PTR(void)* items;

    *(ftSz_DatAttrs*) fp->dat_attrs_backup = *src;
    fp->dat_attrs = fp->dat_attrs_backup;
    fp->cmd_vars[0] = 0;
    fp->cmd_vars[1] = 0;
    fp->cmd_vars[2] = 0;
    fp->cmd_vars[3] = 0;
    ftCn_Cpu_InitSpoofData(gobj);
    items = DP(fp->ft_data->x48_items);
    mu_ak_register_article(fp->kind, DP(items[ftSz_Article_Shot]), ftSz_Article_Shot);
    mu_ak_register_article(fp->kind, DP(items[ftSz_Article_Lw]), ftSz_Article_Lw);
}

/* +08C0, [onactionstatechange]: a ledge catch (252) or DeadDown (0) gives back the air side and
 * down specials. */
static void ftSz_Init_OnActionStateChange(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (fp->motion_id == ftCo_MS_CliffCatch || fp->motion_id == ftCo_MS_DeadDown) {
        ftSz_Vars(fp)->specialairs_used = 0;
        ftSz_Vars(fp)->specialairlw_used = 0;
        ftSz_Vars(fp)->x224C = 0;
    }
}

/* +08EC, [onrespawn]: his attribute block again into fp+2D8. */
static void ftSz_Init_LoadSpecialAttrs(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* src = (ftSz_DatAttrs*) DP(fp->ft_data->ext_attr);

    *(ftSz_DatAttrs*) fp->dat_attrs_backup = *src;
}

/* +0904, [onlanding]: the air side and down specials are back. */
static void ftSz_Init_OnLanding(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    ftSz_Vars(fp)->specialairs_used = 0;
    ftSz_Vars(fp)->specialairlw_used = 0;
    ftSz_Vars(fp)->x224C = 0;
}

/* +05C8, [onframe]. A random number from 0 to 99 is drawn every frame, used or not.
 *
 * A down air whose first hitbox is live and has hit goes on as 351 from frame 0, A pressed or
 * not. A pressed: jab 2 between frames 8 and 30 goes back to jab 1 at frame 2, and jab 1 on to
 * jab 2 the same way. The checks follow one another as the file's do, each on the state the one
 * before left.
 *
 * Then, in an up smash whose first hitbox is live and has hit, from frame 22 on: one line, by
 * the number drawn (0 to 5 the rare one, 6 to 80 five lines by fifteens, above that the silent
 * id -1). And at frame 29 of common state 0xD2, a line.
 *
 * The five lines are the cases of the jump table at +1860. Its entries are offsets from the
 * table, which land in a second copy of this function at +1E54 that nothing calls; each case
 * goes back to the same place in that copy, with the same registers and stack, so the two are
 * one function here. */
static void ftSz_Init_OnFrame(HSD_GObj* gobj)
{
    static const int lines[5] = { 0x13E3, 0x13E6, 0x13E9, 0x13EC, 0x13EF };
    Fighter* fp = GET_FIGHTER(gobj);
    const s32 r = HSD_Randi(100);
    FtMotionId m;

    mu_ak_test_motion(fp);
    m = fp->motion_id;
    if (!(fp->input.pressed_buttons & HSD_PAD_A)) {
        /* +0654 */
        if (m == ftCo_MS_AttackAirLw) {
            goto airlw;
        }
        goto voice;
    }
    if (m == ftCo_MS_Attack12) {
        goto jab2;
    }
    if (m == ftCo_MS_AttackAirLw) {
        goto airlw;
    }

jab1:
    /* +0610 */
    if (m != ftCo_MS_Attack11) {
        goto voice;
    }
    if (fp->cur_anim_frame <= 8.0f) {
        return;
    }
    if (!(fp->cur_anim_frame < 30.0f)) {
        return;
    }
    /* +07E8 */
    Fighter_ChangeMotionState(gobj, ftCo_MS_Attack12, Ft_MF_None, 2.0f, 1.0f, 0.0f, NULL);
    m = fp->motion_id;
    goto voice;

airlw:
    /* +0710 */
    if (fp->x914[0].victims_1[0].victim == NULL || fp->x914[0].state == 0) {
        return;
    }
    ftSz_Change(gobj, ftSz_MS_AttackAirLwHit);
    ftAnim_8006EBA4(gobj);
    m = fp->motion_id;
    if (!(fp->input.pressed_buttons & HSD_PAD_A)) {
        goto voice;
    }
    goto jab1;

jab2:
    /* +0778 */
    if (fp->cur_anim_frame <= 8.0f || fp->cur_anim_frame >= 30.0f) {
        return;
    }
    Fighter_ChangeMotionState(gobj, ftCo_MS_Attack11, Ft_MF_None, 2.0f, 1.0f, 0.0f, NULL);
    m = fp->motion_id;
    if (m == ftCo_MS_AttackAirLw) {
        goto airlw;
    }
    if (!(fp->input.pressed_buttons & HSD_PAD_A)) {
        goto voice;
    }
    goto jab1;

voice:
    /* +065C */
    if (m == ftCo_MS_AttackHi4) {
        if (fp->x914[0].victims_1[0].victim == NULL || fp->x914[0].state == 0) {
            return;
        }
        if (!(fp->cur_anim_frame >= 22.0f)) {
            return;
        }
        if (r <= 5) {
            ft_80088478(fp, FTSZ_SFX_HI4_RARE, 0xFF, 0x40);
        } else if ((u32) (r - 6) > 0x4A) {
            ft_80088478(fp, -1, 0, 0x40);
        } else {
            ft_80088478(fp, lines[(r - 6) / 15], 0xFF, 0x40);
        }
        m = fp->motion_id;
    }
    /* +0664 */
    if (m != FTSZ_MS_VOICE_STATE) {
        return;
    }
    if (!(fp->cur_anim_frame == 29.0f)) {
        return;
    }
    ft_80088478(fp, FTSZ_SFX_STATE_D2, 0xFF, 0x40);
}

/* ---- callbacks more than one state uses ---------------------------------------------------- */

/* +09C0 (and +0D08, +1190, the same instructions; Liu Kang's +0A48): wait when the animation is
 * over. */
static void ftSz_WaitAtEnd_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ft_8008A2BC(gobj);
    }
}

/* +0A90 (and +137C, +1648; Liu Kang's +0B18): fall when the animation is over. */
static void ftSz_FallAtEnd_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ftCo_Fall_Enter(gobj);
    }
}

/* An interrupt callback that is a lone blr (+0A0C, +0ADC, +0C4C, +0D54, +0E04, +1694) and the
 * empty collision callbacks of 344, 346 and 347 (+0D5C, +10BC, +118C). */
static void ftSz_Empty(HSD_GObj* gobj)
{
    (void) gobj;
}

/* ---- the neutral special ------------------------------------------------------------------- */

/* +025C, [specialn]: Liu Kang's +02C8 with his accessory callback. */
static void ftSz_SpecialN_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->throw_flags_b0 = false;
    ftSz_Change(gobj, ftSz_MS_SpecialN);
    ftAnim_8006EBA4(gobj);
    fp->accessory4_cb = ftSz_SpecialN_OnAccessory4;
}

/* +02D4, [specialairn]: Liu Kang's +0340 with his accessory callback. */
static void ftSz_SpecialAirN_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    fp->throw_flags_b0 = false;
    ftSz_Change(gobj, ftSz_MS_SpecialAirN);
    ftAnim_8006EBA4(gobj);
    fp->accessory4_cb = ftSz_SpecialN_OnAccessory4;
}

/* +0A10, the physics callback of 341 (Liu Kang's +1428): the retail ftCommon_8007E2FC, then
 * ft_80085154. */
static void ftSz_SpecialN_Phys(HSD_GObj* gobj)
{
    ftCommon_8007E2FC(gobj);
    ft_80085154(gobj);
}

/* +0A40, the collision callback of 341 (Liu Kang's +17C4): off the ground (the retail ground
 * test that may not leave an edge), the retail step off. */
static void ftSz_SpecialN_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (!ft_800827A0(gobj)) {
        ftCommon_8007D6A4(fp);
    }
}

/* +0AE4, the collision callback of 342 (Liu Kang's +0B6C with 23.0 for his 11.0, +0B5C):
 * landed, grounded and 341 from the same frame; the shot callback is put back unless the frame
 * is 23 or more. */
static void ftSz_SpecialAirN_Coll(HSD_GObj* gobj)
{
    Fighter* fp;

    if (!ft_80081D0C(gobj)) {
        return;
    }
    fp = GET_FIGHTER(gobj);
    ftCommon_8007D7FC(fp);
    Fighter_ChangeMotionState(gobj, ftSz_MS_SpecialN, Ft_MF_None, fp->cur_anim_frame, 1.0f, 0.0f,
                              NULL);
    if (!(fp->cur_anim_frame >= 23.0f)) {
        fp->accessory4_cb = ftSz_SpecialN_OnAccessory4;
    }
}

/* ---- the side special ---------------------------------------------------------------------- */

/* +034C, [specials] (Liu Kang's +03B8). */
static void ftSz_SpecialS_Enter(HSD_GObj* gobj)
{
    ftSz_Change(gobj, ftSz_MS_SpecialS);
    ftAnim_8006EBA4(gobj);
}

/* +039C, [specialairs]: once per air time. The flag is set after the state change; on the
 * ground (it is not, when the game calls this) the down special's flag would be cleared. */
static void ftSz_SpecialAirS_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ftSz_Vars(fp)->specialairs_used == 1) {
        return;
    }
    ftSz_Change(gobj, ftSz_MS_SpecialAirS);
    ftAnim_8006EBA4(gobj);
    ftSz_Vars(fp)->specialairs_used = 1;
    if (GET_FIGHTER(gobj)->ground_or_air == GA_Ground) {
        ftSz_Vars(GET_FIGHTER(gobj))->specialairlw_used = 0;
    }
}

/* +0BA4, the animation callback of 343: once his first hitbox is live and has hit, 344 from
 * frame 0 and its script. Then, in either state, wait when the animation is over. */
static void ftSz_SpecialS_Anim(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (fp->x914[0].victims_1[0].victim != NULL && fp->x914[0].state != 0) {
        ftSz_Change(gobj, ftSz_MS_SpecialSHit);
        ftAnim_8006EBA4(gobj);
    }
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ft_8008A2BC(gobj);
    }
}

/* +0C50 (and +0E08; Liu Kang's +0C7C), the physics callback of 343 and 345. */
static void ftSz_SpecialS_Phys(HSD_GObj* gobj)
{
    ft_80085154(gobj);
    ft_80084FA8(gobj);
}

/* +0C80, the collision callback of 343: off the ground, 345 from the same frame, airborne. */
static void ftSz_SpecialS_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ft_80082708(gobj)) {
        return;
    }
    Fighter_ChangeMotionState(gobj, ftSz_MS_SpecialAirS, Ft_MF_None, fp->cur_anim_frame, 1.0f,
                              0.0f, NULL);
    ftCommon_8007D5D4(fp);
}

/* +0D60, the animation callback of 345: at the end, the fall; with the air flag set, the special
 * fall first (which may be interrupted, +0DC4: li r6, 1; Liu Kang's passes 0), which stands
 * when the flag is still 1 after it. */
static void ftSz_SpecialAirS_Anim(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* da = ftSz_Attrs(fp);

    if (ftAnim_IsFramesRemaining(gobj)) {
        return;
    }
    if (ftSz_Vars(fp)->specialairs_used != 0) {
        ftCo_80096900(gobj, 1, 0, true, da->freefall_mobility, da->landing_lag);
        if (ftSz_Vars(fp)->specialairs_used == 1) {
            return;
        }
    }
    ftCo_Fall_Enter(gobj);
}

/* +0E38, the collision callback of 345 (Liu Kang's +0E0C): on the floor (the floor and ledge
 * test with the facing as the direction), 343 from the same frame and grounded; else the ledge
 * catch test. */
static void ftSz_SpecialAirS_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ft_CheckGroundAndLedge(gobj, (int) fp->facing_dir)) {
        Fighter_ChangeMotionState(gobj, ftSz_MS_SpecialS, Ft_MF_None, fp->cur_anim_frame, 1.0f,
                                  0.0f, NULL);
        ftCommon_8007D7FC(fp);
        return;
    }
    ftCliffCommon_80081298(gobj);
}

/* ---- the up special ------------------------------------------------------------------------ */

/* +0424, [specialhi] (Liu Kang's +04F8 with 346). */
static void ftSz_SpecialHi_Enter(HSD_GObj* gobj)
{
    ftSz_Change(gobj, ftSz_MS_SpecialHi);
    ftAnim_8006EBA4(gobj);
}

/* +0474, [specialairhi] (Liu Kang's +0548 with 347). */
static void ftSz_SpecialAirHi_Enter(HSD_GObj* gobj)
{
    ftSz_Change(gobj, ftSz_MS_SpecialAirHi);
    ftAnim_8006EBA4(gobj);
}

/* +1BC8, from the two animation callbacks below: 350 and its script, airborne, the first two
 * command variables cleared and the attributes' speed upward. The attributes are read before
 * the state change. */
static void ftSz_SpecialHiJump_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* da = ftSz_Attrs(fp);

    ftSz_Change(gobj, ftSz_MS_SpecialHiJump);
    ftAnim_8006EBA4(gobj);
    ftCommon_8007D5D4(fp);
    fp->cmd_vars[0] = 0;
    fp->self_vel.y = da->specialhijump_vel_y;
    fp->cmd_vars[1] = 0;
}

/* +0ED4 (and +10C0, the same instructions), the animation callback of 346 and 347: at the end,
 * 350. */
static void ftSz_SpecialHi_Anim(HSD_GObj* gobj)
{
    if (!ftAnim_IsFramesRemaining(gobj)) {
        ftSz_SpecialHiJump_Enter(gobj);
    }
}

/* +1040 (and +1110, the same instructions), physics: on the ground the ground step; in the air
 * the two air steps, the simple drift with his two words and the drift. */
static void ftSz_SpecialHi_Phys(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* da;

    if (fp->ground_or_air != GA_Air) {
        ft_80084FA8(gobj);
        return;
    }
    da = ftSz_Attrs(fp);
    ft_80085154(gobj);
    ft_800851C0(gobj);
    ftCommon_CalcSelfAccel_DriftSimple(fp, 0.0f, da->specialhi_drift_accel,
                                       da->specialhi_drift_max);
    ftCommon_CalcSelfAccel_Drift(fp);
}

/* +144C, the animation callback of 350: at the end, the special fall with his mobility and lag
 * (+14A0 to +14B0: 0, 1, 0 for the three integer arguments, where 345 passes 1, 0, 1). */
static void ftSz_SpecialHiJump_Anim(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* da = ftSz_Attrs(fp);

    if (!ftAnim_IsFramesRemaining(gobj)) {
        ftCo_80096900(gobj, 0, 1, false, da->freefall_mobility, da->landing_lag);
    }
}

/* +14C4, the interrupt callback of 350: once (command variable 0 is set on the first call), a
 * turn to the stick when it is past the attributes' range on either side. The turn angle is a
 * double product with pi / 2 rounded to single. */
static void ftSz_SpecialHiJump_IASA(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    float stick_x;
    float range;

    if (fp->cmd_vars[0] != 0) {
        return;
    }
    stick_x = fp->input.lstick[0].x;
    fp->cmd_vars[0] = 1;
    range = ftSz_Attrs(fp)->specialhijump_reverse_stick_range;
    if (stick_x < 0.0f) {
        if (range >= -stick_x) {
            return;
        }
    } else if (range >= stick_x) {
        return;
    }
    /* +1500 */
    ftCommon_UpdateFacing(fp);
    ftPartSetRotY(fp, 0, (float) ((double) fp->facing_dir * ftSz_HalfPi));
}

/* +1550, the physics callback of 350: the simple drift with its own two words, then a fall with
 * its own gravity and terminal speed. */
static void ftSz_SpecialHiJump_Phys(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* da = ftSz_Attrs(fp);

    ftCommon_CalcSelfAccel_DriftSimple(fp, 0.0f, da->specialhijump_drift_accel,
                                       da->specialhijump_drift_max);
    ftCommon_Fall(fp, da->specialhijump_gravity, da->specialhijump_terminal_vel);
}

/* +15A0, the collision callback of 350: with command variable 1 at 1 (the script's), the floor
 * and ledge test with the facing as the direction, and the ledge catch test when that finds no
 * floor. Then, either way, the landing test into the special landing with his lag. */
static void ftSz_SpecialHiJump_Coll(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* da = ftSz_Attrs(fp);

    if (fp->cmd_vars[1] == 1) {
        if (!ft_CheckGroundAndLedge(gobj, (int) fp->facing_dir)) {
            ftCliffCommon_80081298(gobj);
        }
    }
    if (ft_80081D0C(gobj)) {
        ftCo_LandingFallSpecial_Enter(gobj, false, da->landing_lag);
    }
}

/* ---- the down special ---------------------------------------------------------------------- */

/* +04C4, [speciallw]: 348, the article's accessory callback, then the script. */
static void ftSz_SpecialLw_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    ftSz_Change(gobj, ftSz_MS_SpecialLw);
    fp->accessory4_cb = ftSz_SpecialLw_OnAccessory4;
    ftAnim_8006EBA4(gobj);
}

/* +052C, [specialairlw]: once per air time. A second one in the same air time changes no
 * state: it sets the accessory callback and runs the script step of the state he is in (+053C
 * to +0548), kept. The first: 349, the flag (cleared again on the ground, as the air side
 * special's entry clears it), the accessory callback, the script. */
static void ftSz_SpecialAirLw_Enter(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    if (ftSz_Vars(fp)->specialairlw_used == 1) {
        fp->accessory4_cb = ftSz_SpecialLw_OnAccessory4;
        ftAnim_8006EBA4(gobj);
        return;
    }
    ftSz_Change(gobj, ftSz_MS_SpecialAirLw);
    ftSz_Vars(fp)->specialairlw_used = 1;
    if (GET_FIGHTER(gobj)->ground_or_air == GA_Ground) {
        ftSz_Vars(GET_FIGHTER(gobj))->specialairlw_used = 0;
    }
    fp->accessory4_cb = ftSz_SpecialLw_OnAccessory4;
    ftAnim_8006EBA4(gobj);
}

/* +11DC, the interrupt callback of 348 (and of 349: +13C8 is a jump here). The shape of Chun-Li's
 * +1C70 (Mario's up special) with his own attribute words:
 *  - while command variable 0 is clear and the stick is past +24, a tilt of up to +28 degrees
 *    against the stick; the two "absolute values" compare with 0.0174533, not 0, and the stored
 *    value is the "absolute" one, not the signed tilt (it replaces fp+6BC only when larger);
 *  - when the script raises the fourth throw flag, it is cleared, and he turns to the stick
 *    unless +2C is the stick's size or more.
 * The degree-to-radian product is a double product rounded to single, the turn angle a single
 * product with (float) pi / 2. */
MU_NO_CONTRACT static void ftSz_SpecialLw_IASA(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* da = ftSz_Attrs(fp);
    float stick_x = fp->input.lstick[0].x;
    float stick_abs = stick_x >= 0.0f ? stick_x : -stick_x;

    if (fp->cmd_vars[0] == 0) {
        float range = da->speciallw_momentum_stick_range;

        if (stick_abs > range) {
            /* +1274 */
            float num = stick_abs - range;
            float den = 1.0f - range;
            float diff = da->speciallw_angle_diff;
            float rad;
            float old;

            rad = num / den;
            rad = rad * diff;
            rad = (float) ((double) rad * ftSz_DegToRad);
            if (!(stick_x <= 0.0f)) {
                rad = -rad;
            }
            old = fp->lstick_angle;
            if (!((double) old >= ftSz_DegToRad)) {
                old = -old;
            }
            if (!((double) rad >= ftSz_DegToRad)) {
                rad = -rad;
            }
            if (!(rad <= old)) {
                fp->lstick_angle = rad;
            }
        }
    }

    /* +1218 */
    if (!fp->throw_flags_b3) {
        return;
    }
    fp->throw_flags_b3 = false;
    if (da->freefall_mobility >= stick_abs) {
        return;
    }
    /* +1238 */
    ftCommon_UpdateFacing(fp);
    ftPartSetRotY(fp, 0, fp->facing_dir * 1.5707964f);
}

/* +1300, the collision callback of 348: off the ground, airborne and 349 from the same frame. */
static void ftSz_SpecialLw_Coll(HSD_GObj* gobj)
{
    Fighter* fp;

    if (ft_80082708(gobj)) {
        return;
    }
    fp = GET_FIGHTER(gobj);
    ftCommon_8007D5D4(fp);
    Fighter_ChangeMotionState(gobj, ftSz_MS_SpecialAirLw, Ft_MF_None, fp->cur_anim_frame, 1.0f,
                              0.0f, NULL);
}

/* +13D0, the collision callback of 349: landed, grounded and 348 from the same frame. */
static void ftSz_SpecialAirLw_Coll(HSD_GObj* gobj)
{
    Fighter* fp;

    if (!ft_80081D0C(gobj)) {
        return;
    }
    fp = GET_FIGHTER(gobj);
    ftCommon_8007D7FC(fp);
    Fighter_ChangeMotionState(gobj, ftSz_MS_SpecialLw, Ft_MF_None, fp->cur_anim_frame, 1.0f, 0.0f,
                              NULL);
}

/* ---- the down air that has hit (351) --------------------------------------------------------- */

/* +1C60, the callback 351 leaves at fp+21C0 (the one the game calls when he deals damage): the
 * attributes' speed, upward and along his facing. */
static void ftSz_AttackAirLwHit_OnDealDamage(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftSz_DatAttrs* da = ftSz_Attrs(fp);

    fp->self_vel.y = da->attackairlwhit_vel_y;
    fp->self_vel.x = fp->facing_dir * da->attackairlwhit_vel_x;
}

/* +1698, the physics callback of 351: the retail ft_80084DB0, then the callback above is set,
 * every frame. */
static void ftSz_AttackAirLwHit_Phys(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    ft_80084DB0(gobj);
    fp->deal_dmg_cb = ftSz_AttackAirLwHit_OnDealDamage;
}

/* +16D0, the collision callback of 351: landed, grounded and the down air's landing (0x4A) from
 * frame 0 at rate 2. */
static void ftSz_AttackAirLwHit_Coll(HSD_GObj* gobj)
{
    if (!ft_80081D0C(gobj)) {
        return;
    }
    ftCommon_8007D7FC(GET_FIGHTER(gobj));
    Fighter_ChangeMotionState(gobj, ftCo_MS_LandingAirLw, Ft_MF_None, 0.0f, 2.0f, 0.0f, NULL);
}

/* ---- the state table (+00FC, 11 rows; every word is the file's) ---------------------------- */
static const MotionState ftSz_Init_MotionStateTable[ftSz_MS_SelfCount] = {
    /* 341 */
    { 295, 0x00340111, 0x12000000, ftSz_WaitAtEnd_Anim, ftSz_Empty, ftSz_SpecialN_Phys,
      ftSz_SpecialN_Coll, ftCamera_UpdateCameraBox },
    /* 342: physics +0AE0, a jump to the retail ft_80084DB0 */
    { 296, 0x00340111, 0x12000000, ftSz_FallAtEnd_Anim, ftSz_Empty, ft_80084DB0,
      ftSz_SpecialAirN_Coll, ftCamera_UpdateCameraBox },
    /* 343 */
    { 297, 0x00340013, 0x13000000, ftSz_SpecialS_Anim, ftSz_Empty, ftSz_SpecialS_Phys,
      ftSz_SpecialS_Coll, ftCamera_UpdateCameraBox },
    /* 344: physics +0D58, a jump to the retail ft_80084FA8 */
    { 298, 0x00340013, 0x13000000, ftSz_WaitAtEnd_Anim, ftSz_Empty, ft_80084FA8, ftSz_Empty,
      ftCamera_UpdateCameraBox },
    /* 345 */
    { 299, 0x00340413, 0x13000000, ftSz_SpecialAirS_Anim, ftSz_Empty, ftSz_SpecialS_Phys,
      ftSz_SpecialAirS_Coll, ftCamera_UpdateCameraBox },
    /* 346: the interrupt +0F20 is Chun-Li's +1C70 */
    { 300, 0x00340013, 0x14000000, ftSz_SpecialHi_Anim, ftCn_SpecialHi_IASA, ftSz_SpecialHi_Phys,
      ftSz_Empty, ftCamera_UpdateCameraBox },
    /* 347: the interrupt +110C is a jump to 346's */
    { 301, 0x00340413, 0x14000000, ftSz_SpecialHi_Anim, ftCn_SpecialHi_IASA, ftSz_SpecialHi_Phys,
      ftSz_Empty, ftCamera_UpdateCameraBox },
    /* 348: physics +12FC, a jump to the retail ft_80085154 */
    { 302, 0x00340014, 0x15000000, ftSz_WaitAtEnd_Anim, ftSz_SpecialLw_IASA, ft_80085154,
      ftSz_SpecialLw_Coll, ftCamera_UpdateCameraBox },
    /* 349: the interrupt +13C8 is a jump to 348's; physics +13CC, a jump to ft_80085154 */
    { 303, 0x00340414, 0x15000000, ftSz_FallAtEnd_Anim, ftSz_SpecialLw_IASA, ft_80085154,
      ftSz_SpecialAirLw_Coll, ftCamera_UpdateCameraBox },
    /* 350 */
    { 304, 0x00340113, 0x14000000, ftSz_SpecialHiJump_Anim, ftSz_SpecialHiJump_IASA,
      ftSz_SpecialHiJump_Phys, ftSz_SpecialHiJump_Coll, ftCamera_UpdateCameraBox },
    /* 351 */
    { 305, 0x00340413, 0x13000000, ftSz_FallAtEnd_Anim, ftSz_Empty, ftSz_AttackAirLwHit_Phys,
      ftSz_AttackAirLwHit_Coll, ftCamera_UpdateCameraBox },
};

const MuAkFighter mu_ak_subzero = {
    .name = "Sub-Zero",
    .file = "PlSz.dat",

    .onload = ftSz_Init_OnLoad,
    .specialn = ftSz_SpecialN_Enter,
    .specialairn = ftSz_SpecialAirN_Enter,
    .specials = ftSz_SpecialS_Enter,
    .specialairs = ftSz_SpecialAirS_Enter,
    .specialhi = ftSz_SpecialHi_Enter,
    .specialairhi = ftSz_SpecialAirHi_Enter,
    .speciallw = ftSz_SpecialLw_Enter,
    .specialairlw = ftSz_SpecialAirLw_Enter,
    .onframe = ftSz_Init_OnFrame,
    .onactionstatechange = ftSz_Init_OnActionStateChange,
    .onrespawn = ftSz_Init_LoadSpecialAttrs,
    .onlanding = ftSz_Init_OnLanding,

    .move_logic = ftSz_Init_MotionStateTable,
    .move_logic_count = ftSz_MS_SelfCount,

    .flags = MU_AK_READY,
    .article_tables = itSz_ArticleTables,
    .article_count = ftSz_Article_Count,
    .kirby = &ftKbSz_Copy,
};
