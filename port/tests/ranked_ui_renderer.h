// Minimal SIS/GX facade: production panel, reveal and art badge run unchanged.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <mu_ranked.h>
#include <mu_rank_badge.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using u8 = unsigned char;
struct GXColor { u8 r,g,b,a; };
struct HSD_GObj { void* user_data; void (*user_data_remove_func)(void*); void* hsd_obj=nullptr; };
struct SisBuffer { unsigned char *data, *end; };
struct sislib_UnkAlloc3 { sislib_UnkAlloc3* x0=nullptr; int xA=0; };
static sislib_UnkAlloc3 css_camera;
static sislib_UnkAlloc3* HSD_SisLib_804D797C = &css_camera;
static unsigned char font_slot;
static unsigned char* HSD_SisLib_804D1124[5] = {&font_slot};
static bool fail_text, fail_storage, fail_entity;
struct HSD_Text {
    SisBuffer* alloc_data=nullptr;
    unsigned char* sis_buffer=nullptr;
    float pos_x, pos_y, pos_z, box_size_x, box_size_y;
    int default_kerning, hidden;
    struct Vec { float x=1, y=1; } font_size, x34;
    GXColor bg_color;
    HSD_GObj* entity;
    void (*render_callback)(void*);
    struct Row { float x, y, sx=1, sy=1; std::string value; GXColor color{255,255,255,255}; };
    std::vector<Row> rows;
};
static unsigned text_count;
static void HSD_SisLib_803A5A2C(void* data) {
    auto* t=static_cast<HSD_Text*>(data); --text_count; delete t->alloc_data; delete t->entity; delete t;
}
static HSD_Text* HSD_SisLib_803A5ACC(int, int context, float x, float y, float z, float w, float h) {
    CHECK(context==0); ++text_count;
    auto* t=new HSD_Text{}; t->pos_x=x; t->pos_y=y; t->pos_z=z;
    t->box_size_x=w; t->box_size_y=h;
    t->entity=new HSD_GObj{t,HSD_SisLib_803A5A2C}; return t;
}
static void HSD_SisLib_803A5CC4(HSD_Text* t);
static HSD_Text* HSD_SisLib_803A6754(int font, int context) {
    if (fail_text) return nullptr;
    auto* t = HSD_SisLib_803A5ACC(font,context,0,0,0,640,480);
    if (!fail_storage) { t->alloc_data = new SisBuffer{&font_slot,&font_slot}; t->sis_buffer=&font_slot; }
    if (fail_entity) { delete t->entity; t->entity=nullptr; }
    return t;
}
static std::string format(const char* fmt, va_list args) {
    char value[512]; std::vsnprintf(value,sizeof value,fmt,args); return value;
}
static int HSD_SisLib_803A6B98(HSD_Text* t, float x, float y, const char* fmt, ...) {
    CHECK(t && t->alloc_data && t->alloc_data->data && t->sis_buffer);
    va_list args; va_start(args,fmt); auto value=format(fmt,args); va_end(args);
    t->rows.push_back({x,y,1,1,value}); return int(t->rows.size()-1);
}
static void HSD_SisLib_803A70A0(HSD_Text* t, int id, const char* fmt, ...) {
    va_list args; va_start(args,fmt); t->rows.at(id).value=format(fmt,args); va_end(args);
}
static void HSD_SisLib_803A7548(HSD_Text* t, int id, float x, float y) {
    t->rows.at(id).sx=x; t->rows.at(id).sy=y;
}
static void HSD_SisLib_803A746C(HSD_Text* t, int id, float x, float y) {
    t->rows.at(id).x=x; t->rows.at(id).y=y;
}
static void HSD_SisLib_803A74F0(HSD_Text* t, int id, GXColor* c) { t->rows.at(id).color=*c; }
static void HSD_SisLib_803A5CC4(HSD_Text* t) { if (t->entity) t->entity->user_data_remove_func(t); else HSD_SisLib_803A5A2C(t); }
static HSD_GObj* GObj_Create(int, int, int) { return new HSD_GObj{}; }
static void GObj_InitUserData(HSD_GObj* g, int, void (*destroy)(void*), void* data) {
    g->user_data=data; g->user_data_remove_func=destroy;
}
static void HSD_GObj_SetupProc(HSD_GObj*, void (*)(HSD_GObj*), int) {}
// Typed allocation lets the C implementation also compile in this C++ harness.
static MuRankBadge* HSD_MemAlloc(size_t size) { return static_cast<MuRankBadge*>(std::malloc(size)); }
static void HSD_Free(void* data) { std::free(data); }
struct GXTexObj {};
enum { GX_TEVREG0, GX_TF_RGBA8, GX_CLAMP, GX_FALSE, GX_LINEAR, GX_ANISO_1,
       GX_TEXMAP0, GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CA_ZERO,
       GX_CA_TEXA, GX_CA_A0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV,
       GX_QUADS, GX_VTXFMT0 };
template<class... A> static void GXSetTevColor(A...) {}
template<class... A> static void GXInitTexObj(A...) {}
template<class... A> static void GXInitTexObjLOD(A...) {}
template<class... A> static void GXLoadTexObj(A...) {}
template<class... A> static void GXSetTevColorIn(A...) {}
template<class... A> static void GXSetTevAlphaIn(A...) {}
template<class... A> static void GXSetTevColorOp(A...) {}
template<class... A> static void GXSetTevAlphaOp(A...) {}
template<class... A> static void GXBegin(A...) {}
template<class... A> static void GXTexCoord2f32(A...) {}
static void GXEnd() {}
struct Vertex { float x,y; };
static std::vector<Vertex> vertices;
static void GXPosition3f32(float x, float y, float) { vertices.push_back({x,y}); }

enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED };
enum { MU_SLP_MM_IDLE, MU_SLP_MM_OPPONENT_CONNECTING, MU_SLP_MM_CONNECTION_SUCCESS };
struct { int mode=MU_SLP_MODE_RANKED; } menu;
struct MuMatchState { int connection_state=MU_SLP_MM_CONNECTION_SUCCESS; int local_index=0; } match;
static MuRankedSet set;
static MuYgRanks ranks;
static bool mu_slippi_css_online() { return true; }
static auto* mu_slippi_state() { return &menu; }
static const MuMatchState* mu_slippi_css_msrb() { return &match; }
static int mu_slippi_css_scene_request() { return 0; }
static int mu_slippi_ranked_available() { return 1; }
void mu_ranked_reveal_frame();
const MuRankedSet* mu_ranked_set() { return &set; }
static bool fail_ranks;
int mu_ranked_read_ranks(MuYgRanks* out) { if (fail_ranks) return 0; *out=ranks; return 1; }
#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH
static bool practice_mode;
int mu_ranked_practice() { return practice_mode; }
#endif
static int mu_slippi_cmd(unsigned cmd, const void*, unsigned, void* out, unsigned size, unsigned* got) {
    if(cmd==0xE4)return 0;
    CHECK(cmd==0xE3 && size==16); std::memset(out,0,size);
    static_cast<u8*>(out)[1]=1; *got=size; return 0;
}
static void lbAudioAx_80024030(int) {}
