"""Execute the production texture title, geometry and panel lifetime with synthetic HSD data."""
from pathlib import Path
import unittest
import online_menu_test as menu

ROOT = Path(__file__).resolve().parents[2]

STUBS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <initializer_list>
#include <cstdint>
#include "MU_LABEL_HEADER"
#define DISC_SET(slot, value) ((slot) = (value))
using u8 = unsigned char; using u32 = unsigned;
enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_MODE_DIRECT, MU_SLP_MODE_TEAMS,
       MU_SLP_MODE_PARTY, MU_SLP_OPT_LOGIN, MU_SLP_OPT_LOGOUT, MU_SLP_OPT_UPDATE,
       MU_SLP_OPT_FRIENDS, MU_SLP_OPT_SLIPPI };
#include "MU_ONLINE_HEADER"
enum { GX_VA_TEX7MTXIDX=8, GX_VA_POS=9, GX_VA_NULL=255, GX_DIRECT=1,
       GX_INDEX8=2, GX_INDEX16=3, GX_F32=4, GX_POS_XYZ=1,
       GX_QUADS=0x80, GX_TRIANGLESTRIP=0x98, GX_TRIANGLEFAN=0xa0,
       GX_TF_I4=0, GX_TF_IA4=2, DOBJ_HIDDEN=1, JOBJ_HIDDEN=16,
       JOBJ_PTCL=32, JOBJ_SPLINE=16384, MENU_KIND_8=8 };
#define DP(x) (x)
#define DISC_NULL(x) (!(x))
struct GXColor { u8 r,g,b,a; };
struct HSD_VtxDescList { int attr,attr_type,comp_cnt,comp_type; u8 frac; unsigned short stride; void* vertex; };
struct HSD_PObj { HSD_PObj* next; HSD_VtxDescList* verts; unsigned short flags,n_display; u8* display; };
struct HSD_ImageDesc { int format; void* image_ptr; int width, height; };
struct HSD_TObj { HSD_ImageDesc* imagedesc; };
struct HSD_Material { GXColor diffuse; float alpha; };
struct HSD_MObj { HSD_TObj* tobj; HSD_Material* mat; };
struct HSD_DObj { HSD_DObj* next; HSD_MObj* mobj; HSD_PObj* pobj; u32 flags; };
struct HSD_JObj { u32 flags; struct { HSD_DObj* dobj; } u; HSD_JObj* child; HSD_JObj* next; float mtx[3][4]; };
struct MainMenuPanelData { u8 cur_menu,prev_menu,x2,state; };
struct HSD_Text { int default_alignment,default_kerning; GXColor text_color;
    float pos_x,pos_y,pos_z; struct { float x,y; } font_size;
    float box_size_x,box_size_y; };
struct HSD_GObj { void* user_data; HSD_JObj* hsd_obj; void (*user_data_remove_func)(void*); };
static bool enabled=true; static int ranked_supported=1,slippi_submenu;
static struct { int cur_menu; } mn_804A04F0;
static int checks,created,freed,logs,old_freed,allocations; static bool fail_alloc,fail_text;
static HSD_Text* live; static HSD_JObj* heading;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
bool near(float a,float b) { return std::fabs(a-b)<0.0001f; }
int mu_slippi_menus_enabled() { return enabled; }
int mu_slippi_yougame_menu() { return ranked_supported; }
void HSD_JObjSetupMatrix(HSD_JObj*) {}
void lb_80011E24(HSD_JObj*,HSD_JObj** out,int index,int end) { CHECK(index==0x29 && end==-1); *out=heading; }
void* HSD_MemAlloc(size_t n) { ++allocations; return fail_alloc?nullptr:std::malloc(n); }
void HSD_Free(void* p) { ++freed; std::free(p); }
void old_free(void* p) { ++old_freed; std::free(p); }
void GObj_RemoveUserData(HSD_GObj* p) { p->user_data_remove_func(p->user_data); p->user_data=nullptr; }
void GObj_InitUserData(HSD_GObj* p,int,void(*cb)(void*),void* data) { p->user_data=data; p->user_data_remove_func=cb; }
HSD_Text* HSD_SisLib_803A6754(int font,int canvas) {
    CHECK(font==0&&canvas==7);CHECK(!live);++created;return live=new HSD_Text{};
}
void HSD_SisLib_803A5CC4(HSD_Text* p) { if(p==live) { delete live;live=nullptr; } }
void HSD_SisLib_803A6B98(HSD_Text*,float,float,const char* word) { CHECK(std::strcmp(word,"Slippi")==0); }
void OSReport(const char* format,...) {
    if(std::strstr(format,"SIS fallback")) return;
    CHECK(std::strcmp(format,"online menu: title imagedesc width=%u height=%u format=%u\n")==0); ++logs;
}
'''

CASES = r"""
static HSD_GObj panel() {
    auto* data=(MainMenuPanelData*)std::malloc(sizeof(MainMenuPanelData));
    *data={8,1,99,2}; return {data,nullptr,old_free};
}
static void befloat(u8* p,float f) { u32 b; std::memcpy(&b,&f,4); for(int i=0;i<4;++i)p[i]=b>>(24-8*i); }
int main() {
    u8 raw[48]; float points[12]={0,0,0, 8,0,0, 8,1,0, 0,1,0};
    for(int i=0;i<12;++i) befloat(raw+4*i,points[i]);
    u8 display[32]={GX_QUADS,0,4,0,0,0,1,0,2,0,3};
    HSD_VtxDescList verts[]={{GX_VA_POS,GX_INDEX16,GX_POS_XYZ,GX_F32,0,12,raw},{GX_VA_NULL}};
    HSD_PObj mesh{nullptr,verts,0,1,display};
    HSD_ImageDesc image{GX_TF_IA4,nullptr,256,32}, crumb_image=image;
    HSD_TObj texture{&image}, crumb_texture{&crumb_image};
    HSD_Material ink{{190,191,205,255},0.75f};
    HSD_MObj material{&texture,&ink}, crumb_material{&crumb_texture,&ink};
    HSD_DObj native{nullptr,&material,&mesh,0x80},breadcrumb{nullptr,&crumb_material,&mesh,0x40};
    HSD_JObj title{0,{&native},nullptr,nullptr,{{2,0,0,4},{0,3,0,8},{0,0,1,17}}};
    HSD_JObj crumb{0,{&breadcrumb},nullptr,&title,{{1,0,0,-10},{0,1,0,9},{0,0,1,17}}};
    HSD_JObj root{JOBJ_HIDDEN,{nullptr},&crumb,nullptr,{}}; heading=&root;
    MuTitleBounds b; CHECK(mu_slippi_title_bounds(&title,&native,&b));
    CHECK(near(b.left,4)&&near(b.right,20)&&near(b.top,11)&&near(b.bottom,8)&&near(b.z,17));
    for(int en=0;en<2;++en) for(int yg=0;yg<2;++yg) {
        enabled=en;ranked_supported=yg;auto p=panel();int before=allocations;
        mu_slippi_title_panel_init(&p);CHECK(allocations-before==int(en&&yg));GObj_RemoveUserData(&p);
    }
    enabled=true;ranked_supported=1;auto p=panel();mu_slippi_title_panel_init(&p);
    auto* state=(MuSlippiTitlePanel*)p.user_data;
    CHECK(state->panel.cur_menu==8&&state->panel.prev_menu==1&&state->panel.x2==99&&state->panel.state==2);
    for(int en=0;en<2;++en) for(int yg=0;yg<2;++yg) for(int sub=0;sub<2;++sub) for(int kind=0;kind<10;++kind) {
        enabled=en;ranked_supported=yg;slippi_submenu=sub;mn_804A04F0.cur_menu=kind;
        mu_slippi_title_panel_update(&p,7);
        CHECK((texture.imagedesc!=&image)==bool(en&&yg&&sub&&kind==8));
        CHECK(native.flags==0x80&&breadcrumb.flags==0x40&&crumb_texture.imagedesc==&crumb_image);
    }
    slippi_submenu=0;mu_slippi_title_panel_update(&p,7);
    enabled=true;ranked_supported=1;mn_804A04F0.cur_menu=8;
    int records=logs;
    u8 i4_reference[4096]{};
    for(int format : {GX_TF_I4,GX_TF_IA4}) {
        image.format=format;
        for(int cycle=0;cycle<2;++cycle) {
            slippi_submenu=1;mu_slippi_title_panel_update(&p,7);
            CHECK(texture.imagedesc==&state->image&&state->original==&image);
            CHECK(state->image.width==256&&state->image.height==32&&state->image.format==format);
            CHECK((uintptr_t(state->image.image_ptr)&31)==0);
            const u8* pixels=(const u8*)state->image.image_ptr;
            int bytes=format==GX_TF_I4?4096:8192, opaque=0;
            for(int i=0;i<bytes;++i)opaque+=pixels[i]!=0;
            CHECK(opaque>0);
            if(format==GX_TF_I4) std::memcpy(i4_reference,pixels,4096);
            else {
                int fractional=0;
                for(int y=0;y<32;++y)for(int x=0;x<256;++x) {
                    unsigned v=pixels[((y/4)*32+x/8)*32+y%4*8+x%8];
                    int pos=((y/8)*32+x/8)*64+y%8*8+x%8;
                    unsigned got=(i4_reference[pos/2]>>(x%2?0:4))&15;
                    unsigned a=v>>4,i=v&15;
                    CHECK(got==(a*(15+2*i)+22)/45);
                    if(a&&i&&i<15) {++fractional;CHECK(got<=a);}
                }
                CHECK(fractional>20);
            }
            // Native animation reselects Online Play: update must reapply Slippi.
            texture.imagedesc=&image;title.mtx[0][3]=6;
            mu_slippi_title_panel_update(&p,7);CHECK(texture.imagedesc==&state->image);
            CHECK(ink.diffuse.r==190&&ink.diffuse.g==191&&ink.diffuse.b==205&&ink.alpha==0.75f);
            CHECK(logs==records); // once per panel, already logged in scope matrix
            title.mtx[0][3]=4;
            slippi_submenu=0;mu_slippi_title_panel_update(&p,7);
            CHECK(texture.imagedesc==&image&&state->allocation==nullptr);
        }
    }
    slippi_submenu=1;mu_slippi_title_panel_update(&p,7);
    GObj_RemoveUserData(&p);CHECK(texture.imagedesc==&image&&native.flags==0x80);
    // Unsupported dimensions/allocation use the correctly tinted SIS fallback.
    // Unsupported geometry/format cannot identify the heading safely: leave art visible.
    for(int failure=0;failure<6;++failure) {
        p=panel();mu_slippi_title_panel_init(&p);
        image={GX_TF_IA4,nullptr,256,32};
        if(failure==0)image.format=6;
        if(failure==1)image.width=255;
        if(failure==2)image.height=65;
        if(failure==3)mesh.flags=0x1000;
        if(failure==4)fail_alloc=true;
        if(failure==5)title.flags=crumb.flags=JOBJ_HIDDEN;
        mu_slippi_title_panel_update(&p,7);
        CHECK(texture.imagedesc==&image);
        bool fallback=failure==1||failure==2||failure==4;
        CHECK(bool(live)==fallback);
        if(fallback) {
            CHECK(native.flags==(0x80|DOBJ_HIDDEN));
            CHECK(live->text_color.r==190&&live->text_color.g==191&&live->text_color.b==205&&live->text_color.a==191);
            CHECK(near(live->pos_x,4)&&near(live->pos_y,-11)&&near(live->font_size.y,3.0f/32));
        } else CHECK(native.flags==0x80);
        fail_alloc=false;mesh.flags=0;title.flags=crumb.flags=0;GObj_RemoveUserData(&p);CHECK(!live&&native.flags==0x80);
    }
    std::printf("Slippi texture title: %d checks, 160 scope combinations, native colour/geometry, I4/IA4 restore passed\n",checks);
}
"""

class TitleTest(unittest.TestCase):
    def test_production_texture_title_lifetime_and_geometry(self):
        label = (ROOT / 'sourceport/game/shim/mu_menu_label.c').read_text()
        title = label[label.index('typedef struct MuSlippiTitlePanel'):]
        self.assertIn('const unsigned char* label = mu_baked_slippi_title;', title)
        self.assertNotIn('mu_menu_title_render', title)
        self.assertNotIn('HSD_SisLib_FontAtlas', title)
        source = (ROOT / 'sourceport/game/shim/mu_slippi_menu.c').read_text()
        predicate = menu.fixture.extract(source, 'mu_slippi_title_active')
        menu.OnlineMenuTest().compile_fixture('slippi-title', STUBS.replace('MU_ONLINE_HEADER', str(ROOT / 'sourceport/game/include/mu_online_menu.h')).replace('MU_LABEL_HEADER', str(ROOT / 'sourceport/game/shim/mu_menu_label.h')) + predicate + title + CASES)

    def test_panel_hooks_after_animation_and_initialization(self):
        patch = (ROOT / 'sourceport/patches/melee-native.patch').read_text()
        self.assertIn('mn_8022ED6C(sp20, &mn_803EB354);\n+#ifdef MU_NATIVE\n+    mu_slippi_title_panel_update(gobj, mn_804D6BB4);', patch)
        self.assertIn('user_data->state = 0;\n+#ifdef MU_NATIVE\n+    mu_slippi_title_panel_init(temp_r31);', patch)

if __name__ == '__main__':
    unittest.main()
