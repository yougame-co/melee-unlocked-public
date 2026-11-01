// Production setup callbacks and the badge/reveal's actual environment switch.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ranked_ui_renderer.h"
#include "ranked_ui_badge.inc"
struct HSD_JObj { bool running=false; float frame=0, alpha=0; GXColor color{}; unsigned samples=0; };
enum { BUTTON, TURN, ICON, ARROW };
struct Part { HSD_JObj* root; void* set=nullptr; int state=-1; unsigned kind; };
static void HSD_JObjAnimAll(void* data) {
    auto* root=static_cast<HSD_JObj*>(data);
    if(root->running){++root->frame;++root->samples;}
}
static void HSD_JObjRemoveAnimAll(HSD_JObj* root) { root->running=false; }
static void gm_8016895C(HSD_JObj* root, void*, unsigned) { root->running=true; }
static void HSD_JObjReqAnimAll(HSD_JObj* root, float frame) { root->frame=frame; }
static void tint(HSD_JObj* root, float alpha, GXColor color) { root->alpha=alpha;root->color=color; }
#include "mu_ranked_widgets_motion.h"
static void motion_flag(const char* value) {
#ifdef _WIN32
    _putenv_s("MU_REDUCED_MOTION",value);
#else
    setenv("MU_REDUCED_MOTION",value,1);
#endif
}
int main() {
    for(const char* flag:{"0","1","true"}) {
        motion_flag(flag);const bool reduced=flag[0]!='0';
        CHECK(bool(mu_rank_badge_reduced_motion())==reduced);
        for(unsigned kind:{BUTTON,TURN,ICON,ARROW})for(unsigned value=0;value<=4;++value) {
            HSD_JObj root;Part part{&root,nullptr,-1,kind};HSD_GObj object{};object.hsd_obj=&root;
            state(&part,value,value==4?2:0);
            CHECK(part.state==int(value));
            CHECK(root.alpha==(value==2?.3f:1));
            CHECK(root.color.r==(value==3?254:128)); // local turn remains readable
            const float frame=root.frame;const unsigned samples=root.samples;
            for(unsigned i=0;i<180;++i)animate(&object);
            if(reduced || value==0 || (value==2 && kind!=BUTTON && kind!=TURN)) {
                CHECK(!root.running && root.frame==frame && root.samples==samples);
            } else {
                CHECK(root.running && root.frame==frame+180 && root.samples==samples+180);
            }
            // State caching must not restart an animation on every draw.
            state(&part,value,value==4?2:0);CHECK(root.samples==samples+(root.running?180:0));
            state(&part,0,0);CHECK(!root.running);
        }
        // Even an animation installed by the archive loader cannot tick in reduced motion.
        HSD_JObj archive;archive.running=true;HSD_GObj object{};object.hsd_obj=&archive;
        animate(&object);CHECK(archive.samples==(reduced?0u:1u));
    }
    motion_flag("");
    std::cout<<"Ranked setup motion: "<<checks<<" checks passed\n";
}
