/* ACE 2.0.1's Sub-Zero: his two articles (m-ex "itFunction" of PlSz.dat, articles 0 and 1; the
 * spawn of article 1 is in the fighter's block).
 *
 * Article 0 (item kind 353) is Chun-Li's neutral shot byte for byte: her table (ace/chunli,
 * itCn_Shot_Logic), created by her spawn, which his block carries at +1C84.
 *
 * Article 1 (item kind 354), the down special's, is a variant of the same block: a lifetime from
 * the article's attributes and no speed of its own, gone at the end of its lifetime or one frame
 * after it touches a wall, a ceiling or a floor, gone when it hits or is hit. Its animation and
 * collision callbacks and its constant slots are the shot's instruction for instruction; the
 * spawn event (no speed), the destroy event, the reflect event (no turn of the model) and the
 * missing shield bounce slot are its own. */
#include "subzero.h"

#include "../chunli/chunli.h"

#include <dolphin/os.h>

#include <melee/db/db.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/it/inlines.h>
#include <melee/it/it_26B1.h>
#include <melee/it/it_2725.h>
#include <melee/it/item.h>
#include <melee/it/types.h>
#include <sysdolphin/baselib/gobj.h>

/* ==== the spawn (fighter block) ================================================================== */

/* Fighter block +1D68, from the down special's accessory callback (+184C). Chun-Li's +3258 with
 * one more store: the request of a retail fighter projectile (the position the owner's middle,
 * the previous position the bone's, on the plane, no speed, the facing, no damage, the first
 * collision flag, the airborne creator); then it_8026B3A8 on the new item, the item into fp+2234
 * (+1E28), the owner for the debug display and it_802750F8.
 *
 * Not written by the console and zeroed here, as in her spawn: spawn+0x3E and the seven low bits
 * of spawn+0x44 (left as the stack held them), hold_kind, x10 and the ground or air word (the
 * creator writes those three). The console does not test the creator's result; here nothing
 * more is done when no item was created. */
void itSz_Lw_Spawn(HSD_GObj* owner_gobj, Vec3* pos, ItemKind kind, float facing_dir)
{
    SpawnItem spawn = { 0 };
    Item_GObj* item_gobj;
    Vec3 owner_pos;

    ftLib_80086990(owner_gobj, &owner_pos);

    spawn.x0_parent_gobj = owner_gobj;
    spawn.x4_parent_gobj2 = owner_gobj;
    spawn.kind = kind;
    spawn.pos = owner_pos;
    spawn.prev_pos.x = pos->x;
    spawn.prev_pos.y = pos->y;
    spawn.facing_dir = facing_dir;
    spawn.x3C_damage = 0;
    spawn.x40 = 0;
    spawn.x44_flag.b0 = true;
    spawn.prev_pos.z = 0.0f;
    spawn.vel.x = 0.0f;
    spawn.vel.y = 0.0f;
    spawn.vel.z = 0.0f;

    item_gobj = Item_80268B18(&spawn);
    if (item_gobj == NULL) {
        OSReport("[ak] Sub-Zero article 1: item kind %d was not created\n", (int) kind);
        return;
    }
    it_8026B3A8(item_gobj);
    ftSz_Vars(GET_FIGHTER(owner_gobj))->speciallw_item = item_gobj;
    db_80225DD8(item_gobj, owner_gobj);
    it_802750F8(item_gobj);
}

/* ==== article 1's own callbacks ================================================================== */

/* The article's special attributes (ip->xC4_article_data->x4_specialAttributes, disc data): the
 * one word the block reads. */
typedef struct itSz_LwAttrs {
    /* +0 */ float lifetime;
} DISC_STRUCT itSz_LwAttrs;

/* item 1 +0010, [onspawn]: the lifetime (the first word of the article's attributes), state 0,
 * then it_8026B3A8. The shot's spawn event without the speed. */
static void itSz_Lw_Spawned(Item_GObj* gobj)
{
    Item* ip = GET_ITEM(gobj);
    itSz_LwAttrs* attrs = (itSz_LwAttrs*) DP(ip->xC4_article_data->x4_specialAttributes);

    it_80275158(gobj, attrs->lifetime);
    Item_80268E5C(gobj, 0, ITEM_ANIM_UPDATE);
    it_8026B3A8(gobj);
}

/* item 1 +0068, [ondestroy]. The console reads the first word of the item's own variables
 * (ip+DD4) as an object, and when it is not null clears fp+2250 of that object's fighter if it
 * is not zero already. Nothing in his files writes ip+DD4 or sets fp+2250, and the game does
 * not clear an item's variables when it creates one, so on the console the word is whatever the
 * item slot held before.
 *
 * Here the word is used only when it is one of the fighters of the match and that fighter is a
 * Sub-Zero: any other value is left alone, where the console would follow it. */
static void itSz_Lw_Destroyed(Item_GObj* gobj)
{
    Item* ip = GET_ITEM(gobj);
    HSD_GObj* held;
    HSD_GObj* cur;

    __builtin_memcpy(&held, &ip->xDD4_itemVar, sizeof held);
    if (held == NULL) {
        return;
    }
    for (cur = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; cur != NULL; cur = cur->next) {
        if (cur == held) {
            Fighter* fp = GET_FIGHTER(cur);

            if (mu_ak_fighter(fp->kind) == &mu_ak_subzero && ftSz_Vars(fp)->x2250 != 0) {
                ftSz_Vars(fp)->x2250 = 0;
            }
            return;
        }
    }
}

/* item 1 +00C8, [onreflect]: the common reflect, and it lives on. The model is not turned, as
 * the shot's is. */
static bool itSz_Lw_Reflected(Item_GObj* gobj)
{
    it_80273030(gobj);
    return false;
}

/* item 1 +0000, [item_state_table]: one state, animation 0; the callbacks +00FC, +0130 and
 * +0138 are the shot's +0128, +015C and +0164. */
static ItemStateTable itSz_Lw_States[] = {
    { 0, itCn_Shot_Anim, itCn_Shot_Phys, itCn_Shot_Coll },
};

/* Article 1 (354): slots 0 to 10 and 13 exported; 11 (absorbed), 12 (the shield bounce) and 14
 * not. */
static ItemLogicTable itSz_Lw_Logic = {
    .states = itSz_Lw_States,
    .spawned = itSz_Lw_Spawned,            /* +0010 */
    .destroyed = itSz_Lw_Destroyed,        /* +0068 */
    .picked_up = itCn_Shot_Nothing,        /* +0098 */
    .dropped = itCn_Shot_Nothing,          /* +00A0 */
    .thrown = itCn_Shot_Nothing,           /* +00A8 */
    .dmg_dealt = itCn_Shot_DmgDealt,       /* +00B0 */
    .dmg_received = itCn_Shot_DmgReceived, /* +00B8 */
    .entered_air = itCn_Shot_Nothing,      /* +00C0 */
    .reflected = itSz_Lw_Reflected,        /* +00C8 */
    .clanked = itCn_Shot_Clanked,          /* +00EC */
    .absorbed = NULL,
    .shield_bounced = NULL,
    .hit_shield = itCn_Shot_HitShield,     /* +00F4 */
    .evt_unk = NULL,
};

/* ==== the list, in the order of his MxDt item lookup ============================================ */
ItemLogicTable* const itSz_ArticleTables[ftSz_Article_Count] = {
    &itCn_Shot_Logic, /* 353: article 0, Chun-Li's neutral shot table */
    &itSz_Lw_Logic,   /* 354: article 1 */
};
