/* Decorative GameSetup animation callbacks. GPL-3.0-or-later.
 * Shared with the capturing animation test; gameplay clocks live in the model. */
#ifndef MU_RANKED_WIDGETS_MOTION_H
#define MU_RANKED_WIDGETS_MOTION_H
int mu_rank_badge_reduced_motion(void);
static void tint(HSD_JObj* root, float alpha, GXColor color);
static void animate(HSD_GObj* obj)
{
    if (!mu_rank_badge_reduced_motion()) HSD_JObjAnimAll(obj->hsd_obj);
}
/* state: 0 static, 1 blink, 2 dim, 3 local turn (yellow), 4 icon hover. */
static void state(Part* p, unsigned value, unsigned animation)
{
    GXColor gray={128,128,128,255}, yellow={254,202,52,255};
    if (!p->root || p->state == (int)value) return;
    HSD_JObjRemoveAnimAll(p->root);
    tint(p->root,value == 2 ? .3f : 1,value == 3 ? yellow : gray);
    if (value == 1 || value == 3 || value == 4 || p->kind == BUTTON || p->kind == TURN) {
        gm_8016895C(p->root,p->set,animation);
        HSD_JObjReqAnimAll(p->root,p->kind == BUTTON && value == 1 ? 10 : 0);
        if (value == 0 || mu_rank_badge_reduced_motion()) { HSD_JObjAnimAll(p->root); HSD_JObjRemoveAnimAll(p->root); }
    }
    p->state=value;
}
#endif
