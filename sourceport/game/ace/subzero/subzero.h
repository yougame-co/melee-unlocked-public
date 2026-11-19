/* ACE 2.0.1's Sub-Zero, native (PlSz.dat, ftDataSubZero).
 *
 * Sub-Zero ships in PlSz.dat as m-ex PowerPC: one "ftFunction" block (0x2200 bytes, 14 exports)
 * and two "itFunction" blocks (articles 0 and 1), built without symbols. The file is made from
 * the same source as ACE's Liu Kang and Chun-Li: 19 of its functions are Liu Kang's instruction
 * for instruction (13 of them Chun-Li's too: the neutral special's entries, the up special's
 * interrupt, the shot's spawn, the m-ex CPU helper), and article 0 is Chun-Li's neutral shot
 * byte for byte, so those are their native functions (ace/chunli, ace/liukang). The rest is his
 * own:
 *
 *   neutral   341/342: a shot (article 0) from the bone of part 54 on the script's flag
 *   side      343 (ground), which goes on as 344 once it has hit; 345 (air, once per air time,
 *             then the special fall); 343 and 345 swap on leaving or reaching the ground
 *   up        346/347, then 350 at the end of the animation: a rise at the attributes' speed
 *             with its own drift and gravity, one turn by the stick, the special fall after it
 *   down      348 (ground), 349 (air, once per air time): the shape of Mario's up special's
 *             interrupt (a tilt and a turn by the stick) and an article (1) from the bone of
 *             part 4 on the script's flag; the two swap on leaving or reaching the ground
 *   jab       onframe: A pressed between frames 8 and 30 of jab 1 or jab 2 goes to the other
 *             one at frame 2
 *   down air  onframe: a down air that has hit goes on as 351, which bounces him off at the
 *             attributes' speed on each hit and lands as the down air's landing at rate 2
 *   voice     onframe: an up smash that has hit plays one of six lines from frame 22 on
 *
 * Offsets in the comments (+05C8) are offsets into the ftFunction block of ACE 2.0.1's
 * PlSz.dat; "item 1 +0068" is an offset into the itFunction block of article 1. "fp+" is a
 * console offset of the Fighter. The file has no symbols: the function names are this port's. */
#ifndef MU_ACE_SUBZERO_H
#define MU_ACE_SUBZERO_H

#include <Runtime/platform.h>

#include <melee/ft/forward.h>
#include <melee/it/forward.h>
#include <sysdolphin/baselib/forward.h>

#include <melee/ft/kinds/ftCommon/forward.h>
#include <melee/ft/types.h>
#include <melee/it/kinds/forward.h>
#include <melee/it/kinds/types.h>
#include <melee/it/types.h>

#include <stddef.h>

#include "../../akaneia/mu_ak_fighter.h"
#include "mu_disc.h"

/* ---- Action states (move_logic, +00FC, 11 rows numbered from ftCo_MS_Count) ---- */
typedef enum ftSubZero_MotionState {
    ftSz_MS_SpecialN = ftCo_MS_Count, // 341 (0x155): the shot, ground
    ftSz_MS_SpecialAirN,              // 342 (0x156): the shot, air
    ftSz_MS_SpecialS,                 // 343 (0x157)
    ftSz_MS_SpecialSHit,              // 344 (0x158): 343 once it has hit
    ftSz_MS_SpecialAirS,              // 345 (0x159)
    ftSz_MS_SpecialHi,                // 346 (0x15A)
    ftSz_MS_SpecialAirHi,             // 347 (0x15B)
    ftSz_MS_SpecialLw,                // 348 (0x15C)
    ftSz_MS_SpecialAirLw,             // 349 (0x15D)
    ftSz_MS_SpecialHiJump,            // 350 (0x15E): after 346 or 347
    ftSz_MS_AttackAirLwHit,           // 351 (0x15F): the down air once it has hit
    ftSz_MS_Count,
    ftSz_MS_SelfCount = ftSz_MS_Count - ftCo_MS_Count,
} ftSubZero_MotionState;

_Static_assert(ftSz_MS_SelfCount == 11, "11 rows in PlSz.dat's move_logic");
_Static_assert(ftSz_MS_SpecialN == 0x155 && ftSz_MS_SpecialAirN == 0x156 &&
                   ftSz_MS_SpecialS == 0x157 && ftSz_MS_SpecialSHit == 0x158 &&
                   ftSz_MS_SpecialAirS == 0x159 && ftSz_MS_SpecialHi == 0x15A &&
                   ftSz_MS_SpecialAirHi == 0x15B && ftSz_MS_SpecialLw == 0x15C &&
                   ftSz_MS_SpecialAirLw == 0x15D && ftSz_MS_SpecialHiJump == 0x15E &&
                   ftSz_MS_AttackAirLwHit == 0x15F,
               "the state numbers PlSz.dat's code passes to Fighter_ChangeMotionState");
_Static_assert(ftCo_MS_Attack11 == 0x2C && ftCo_MS_Attack12 == 0x2D &&
                   ftCo_MS_AttackHi4 == 0x3F && ftCo_MS_AttackAirLw == 0x45 &&
                   ftCo_MS_LandingAirLw == 0x4A && ftCo_MS_CliffCatch == 0xFC &&
                   ftCo_MS_DeadDown == 0,
               "the common states his code tests or enters (+05C8, +08C0, +1734)");

/* The common state the onframe event plays a line in at frame 29 (+0664: cmpwi 0xD2). */
#define FTSZ_MS_VOICE_STATE ((FtMotionId) 0xD2)

/* ---- Special attributes (ftData->ext_attr, 0xAC bytes, disc data; values of PlSz.dat). Only
 * the words his code reads are named; +08 to +10 are read by Chun-Li's up special interrupt,
 * which he shares (+0F20). ---- */
typedef struct ftSz_DatAttrs {
    /* +00 */ float x0[2];
    /* +08 */ float specialhi_momentum_stick_range; ///< 0.009
    /* +0C */ float specialhi_angle_diff;           ///< the integer 23, read as a float
    /* +10 */ float specialhi_reverse_stick_range;  ///< 0.01
    /* +14 */ float x14[4];
    /* +24 */ float speciallw_momentum_stick_range; ///< 0.07 (+120C)
    /* +28 */ float speciallw_angle_diff;           ///< 3.0 (+1284)
    /* +2C */ float freefall_mobility; ///< 0.74 (+0DC8, +14A4); the down special's turn range
                                       ///< too (+1224)
    /* +30 */ float landing_lag;       ///< 20 (+0DC0, +149C, +1600)
    /* +34 */ float specialhijump_reverse_stick_range; ///< 0.5 (+14F0)
    /* +38 */ float specialhi_drift_accel;          ///< 0.5 (+1098, +1168)
    /* +3C */ float x3C;
    /* +40 */ float specialhijump_vel_y;            ///< 2.75 (+1C20)
    /* +44 */ float specialhi_drift_max;            ///< 0.85 (+1094, +1164)
    /* +48 */ float x48;
    /* +4C */ float specialhijump_drift_accel;      ///< 0.2 (+156C)
    /* +50 */ float specialhijump_drift_max;        ///< 0.6 (+1570)
    /* +54 */ float specialhijump_gravity;          ///< 0.1 (+1588)
    /* +58 */ float specialhijump_terminal_vel;     ///< 2.0 (+1584)
    /* +5C */ float x5C[12];
    /* +8C */ float attackairlwhit_vel_x;           ///< -1.1, times the facing (+1C6C)
    /* +90 */ float attackairlwhit_vel_y;           ///< 1.2 (+1C70)
    /* +94 */ float x94[6];
} DISC_STRUCT ftSz_DatAttrs;

_Static_assert(sizeof(ftSz_DatAttrs) == 0xAC, "the size the load and the respawn event copy");
_Static_assert(__builtin_offsetof(ftSz_DatAttrs, speciallw_momentum_stick_range) == 0x24 &&
                   __builtin_offsetof(ftSz_DatAttrs, specialhijump_vel_y) == 0x40 &&
                   __builtin_offsetof(ftSz_DatAttrs, specialhijump_terminal_vel) == 0x58 &&
                   __builtin_offsetof(ftSz_DatAttrs, attackairlwhit_vel_x) == 0x8C,
               "the attribute offsets his code reads");

/* ---- Fighter variables (fp->u, console fp+222C) ----
 * The item pointer takes eight bytes here and four on the console: the words after it keep
 * their console offsets from fp+222C. */
typedef struct ftSz_FighterVars {
    /* fp+222C */ u8 x222C[8];
    /* fp+2234 */ Item_GObj* speciallw_item; ///< the last article 1 created (+1E28), never read
    /* fp+2238 */ u8 x2238[0x18 - 8 - sizeof(Item_GObj*)];
    /* fp+2244 */ s32 specialairs_used;  ///< 1 after the air side special, until landing
    /* fp+2248 */ s32 specialairlw_used; ///< 1 after the air down special, until landing
    /* fp+224C */ s32 x224C;             ///< cleared with the two above, never read
    /* fp+2250 */ s32 x2250;             ///< cleared by article 1's destroy event, never set
} ftSz_FighterVars;

_Static_assert(offsetof(ftSz_FighterVars, specialairs_used) == 0x18 &&
                   offsetof(ftSz_FighterVars, specialairlw_used) == 0x1C &&
                   offsetof(ftSz_FighterVars, x224C) == 0x20 &&
                   offsetof(ftSz_FighterVars, x2250) == 0x24,
               "fp+2244 to fp+2250");
_Static_assert(sizeof(ftSz_FighterVars) <= sizeof(((Fighter*) 0)->u), "his variables fit");

static inline ftSz_DatAttrs* ftSz_Attrs(Fighter* fp)
{
    return (ftSz_DatAttrs*) fp->dat_attrs;
}

static inline ftSz_FighterVars* ftSz_Vars(Fighter* fp)
{
    return (ftSz_FighterVars*) &fp->u;
}

/* His two articles (MxDt item lookup: 353 and 354) and the parts whose bones they start from
 * (+179C: lwz r3, 0x360(r7); +1818: lwz r3, 0x40(r7); fp->parts + part * 0x10 on the console). */
enum { ftSz_Article_Shot = 0, ftSz_Article_Lw = 1, ftSz_Article_Count = 2 };
#define FTSZ_SHOT_PART 54
#define FTSZ_LW_PART 4

/* The sound ids his code passes (his own bank). */
#define FTSZ_SFX_HI4_RARE 0x13F2 /* +082C */
#define FTSZ_SFX_STATE_D2 0x13F5 /* +0698 */

/* ---- subzero.c ---- */
extern const MuAkFighter mu_ak_subzero;

/* ---- subzero_items.c ---- */
/* +1D68 */ void itSz_Lw_Spawn(HSD_GObj* owner_gobj, Vec3* pos, ItemKind kind, float facing_dir);
extern ItemLogicTable* const itSz_ArticleTables[ftSz_Article_Count];

/* ---- subzero_kirby.c ---- */
extern const struct MuAkKirbyCopy ftKbSz_Copy;

#endif
