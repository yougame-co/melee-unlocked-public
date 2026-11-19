/* ACE 2.0.1's Liu Kang, native (PlKg.dat, ftDataLiuKang).
 *
 * Liu Kang ships in PlKg.dat as m-ex PowerPC: one "ftFunction" block (0x20C0 bytes, 14 exports)
 * and one "itFunction" block (article 0), built without symbols. The file is made from the same
 * source as ACE's Chun-Li: 16 of its functions are hers instruction for instruction (the neutral
 * special, the up special's interrupt and collision, the m-ex CPU helper) and its article is her
 * neutral special's shot byte for byte, so those are her native functions (ace/chunli). The rest
 * is his own:
 *
 *   neutral   341/342: a shot (article 0) from the bone of part 54 on the script's flag
 *   side      343 (ground), 344 (air, once per air time): a move at the script's speed that
 *             swaps between the two on leaving or reaching the ground
 *   up        345/346: a rise in the shape of Mario's up special, then the special fall
 *   down      347 -> 348 on the ground (351 once he is off it); 349 -> 352 in the air (once
 *             per air time), 350 on landing
 *   jab       onframe: A pressed between frames 8 and 30 of jab 1 or jab 2 goes to the other
 *             one at frame 2; A between frames 15 and 30 of the dash attack goes on as 353
 *   voice     onframe: an up smash that has hit plays one of six lines from frame 15 on
 *
 * Offsets in the comments (+0620) are offsets into the ftFunction block of ACE 2.0.1's
 * PlKg.dat. "fp+" is a console offset of the Fighter. The file has no symbols: the function
 * names are this port's. */
#ifndef MU_ACE_LIUKANG_H
#define MU_ACE_LIUKANG_H

#include <Runtime/platform.h>

#include <melee/ft/forward.h>
#include <melee/it/forward.h>
#include <sysdolphin/baselib/forward.h>

#include <melee/ft/kinds/ftCommon/forward.h>
#include <melee/ft/types.h>
#include <melee/it/types.h>

#include <stddef.h>

#include "../../akaneia/mu_ak_fighter.h"
#include "mu_disc.h"

/* ---- Action states (move_logic, +0128, 13 rows numbered from ftCo_MS_Count) ---- */
typedef enum ftLiuKang_MotionState {
    ftKg_MS_SpecialN = ftCo_MS_Count, // 341 (0x155): the shot, ground
    ftKg_MS_SpecialAirN,              // 342 (0x156): the shot, air
    ftKg_MS_SpecialS,                 // 343 (0x157)
    ftKg_MS_SpecialAirS,              // 344 (0x158)
    ftKg_MS_SpecialHi,                // 345 (0x159)
    ftKg_MS_SpecialAirHi,             // 346 (0x15A)
    ftKg_MS_SpecialLw,                // 347 (0x15B)
    ftKg_MS_SpecialLwEnd,             // 348 (0x15C): after 347 on the ground
    ftKg_MS_SpecialAirLw,             // 349 (0x15D)
    ftKg_MS_SpecialAirLwLanding,      // 350 (0x15E): 349 or 352 landed
    ftKg_MS_SpecialLwFall,            // 351 (0x15F): after 347 in the air
    ftKg_MS_SpecialAirLwFall,         // 352 (0x160): after 349
    ftKg_MS_AttackDashA,              // 353 (0x161): the dash attack's follow-up
    ftKg_MS_Count,
    ftKg_MS_SelfCount = ftKg_MS_Count - ftCo_MS_Count,
} ftLiuKang_MotionState;

_Static_assert(ftKg_MS_SelfCount == 13, "13 rows in PlKg.dat's move_logic");
_Static_assert(ftKg_MS_SpecialS == 0x157 && ftKg_MS_SpecialAirLwLanding == 0x15E &&
                   ftKg_MS_SpecialLwFall == 0x15F && ftKg_MS_AttackDashA == 0x161,
               "the state numbers PlKg.dat's code passes to Fighter_ChangeMotionState");
_Static_assert(ftCo_MS_Attack11 == 0x2C && ftCo_MS_Attack12 == 0x2D &&
                   ftCo_MS_AttackDash == 0x32 && ftCo_MS_AttackHi4 == 0x3F,
               "the common states the onframe event tests (+0620)");

/* The common state the onframe event plays a line in at frame 29 (+0760: cmpwi 0xD2). */
#define FTKG_MS_VOICE_STATE ((FtMotionId) 0xD2)

/* ---- Special attributes (ftData->ext_attr, 0x5C bytes, disc data). Only the words his code
 * reads are named; +08 to +10 are read by Chun-Li's up special interrupt, which he shares. ---- */
typedef struct ftKg_DatAttrs {
    /* +00 */ float x0[2];
    /* +08 */ float specialhi_momentum_stick_range;
    /* +0C */ float specialhi_angle_diff;
    /* +10 */ float specialhi_reverse_stick_range;
    /* +14 */ float x14[6];
    /* +2C */ float specialhi_freefall_mobility; ///< +0D9C, +0F00, +115C
    /* +30 */ float specialhi_landing_lag;       ///< +0D94, +0EF8, +1154, +1C0C, +1C20
    /* +34 */ float x34;
    /* +38 */ float specialhi_drift_accel;       ///< +1098, +11D8
    /* +3C */ float x3C;
    /* +40 */ float specialhi_drift_max;         ///< +1094, +11D4
    /* +44 */ float x44[6];
} DISC_STRUCT ftKg_DatAttrs;

_Static_assert(sizeof(ftKg_DatAttrs) == 0x5C, "the size the load and the respawn event copy");

/* ---- Fighter variables (fp->u, console fp+222C) ---- */
typedef struct ftKg_FighterVars {
    /* fp+222C */ u8 x222C[0x14];
    /* fp+2240 */ s32 specialairs_used;  ///< 1 after the air side special, until landing
    /* fp+2244 */ s32 specialairlw_used; ///< 1 after the air down special, until landing
    /* fp+2248 */ s32 x2248;             ///< cleared with the two above, never read
} ftKg_FighterVars;

_Static_assert(offsetof(ftKg_FighterVars, specialairs_used) == 0x14 &&
                   offsetof(ftKg_FighterVars, x2248) == 0x1C,
               "fp+2240 to fp+2248");
_Static_assert(sizeof(ftKg_FighterVars) <= sizeof(((Fighter*) 0)->u), "his variables fit");

static inline ftKg_DatAttrs* ftKg_Attrs(Fighter* fp)
{
    return (ftKg_DatAttrs*) fp->dat_attrs;
}

static inline ftKg_FighterVars* ftKg_Vars(Fighter* fp)
{
    return (ftKg_FighterVars*) &fp->u;
}

/* His one article and the part whose bone it starts from (+1848: lwz r3, 0x360(r7), fp->parts
 * + 54 * 0x10 on the console). */
enum { ftKg_Article_Shot = 0, ftKg_Article_Count = 1 };
#define FTKG_SHOT_PART 54

/* The sound ids his code passes (his own bank). */
#define FTKG_SFX_LOAD 0x13AD       /* +0110 */
#define FTKG_SFX_HI4_RARE 0x13CB   /* +0750 */
#define FTKG_SFX_STATE_D2 0x13C8   /* +0890 */

#endif
