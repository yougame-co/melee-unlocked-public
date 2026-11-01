// Production native/baked texture adapter, with synthetic immutable HSD data.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "mu_menu_label.h"
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
enum { MENU_KIND_8 = 8, GX_TF_IA4 = 2 };
struct HSD_ImageDesc { const void* image_ptr; int width, height, format; };
struct HSD_TObj { HSD_TObj* next; HSD_ImageDesc* imagedesc; HSD_ImageDesc** imagetbl; };
struct HSD_MObj { HSD_TObj* tobj; unsigned color; };
struct HSD_DObj { HSD_DObj* next; HSD_MObj* mobj; };
struct HSD_JObj { union { HSD_DObj* dobj; } u; };
static int menus, offered, submenu, tournament;
static int mu_slippi_menus_enabled() { return menus; }
static int mu_slippi_yougame_menu() { return offered; }
static int mu_slippi_menu_label_kind(int row) {
    if (!offered) return 0;
    if (submenu) return row >= 0 && row < 3 ? row + 3 : 0;
    return row == 0 ? 6 : row == 1 ? 7 : row == 2 ? 1 :
           tournament && row == 3 ? 8 : row == (tournament ? 4 : 3) ? 2 : 0;
}
#define DISC_SET(slot, value) ((slot) = (value))
#include "friends_label_adapter.inc"
static unsigned pixel(const unsigned char* p, int x, int y) {
    return p[((y/4)*22+x/8)*32+y%4*8+x%8];
}
int main() {
    static_assert(sizeof mu_baked_friends == 5632 && sizeof mu_baked_slippi == 5632 &&
                  sizeof mu_baked_slippi_title == 5632, "GX IA4 padded byte count");
    for (auto* art : {mu_baked_friends, mu_baked_slippi, mu_baked_slippi_title}) {
        int aa=0, fill=0, outline=0, ring=0;
        CHECK((reinterpret_cast<size_t>(art)&31)==0);
        for(int y=0;y<32;++y)for(int x=0;x<176;++x) {
            auto v=pixel(art,x,y);
            if(y>=30 || x<4 || x>=172) CHECK(v==0);
            if(art!=mu_baked_slippi_title && y==0) CHECK(v==0);
            // Natural p descenders retain a faint AA tail in texel 29;
            // both GX padding rows must still be entirely transparent.
            if(art!=mu_baked_slippi_title && y==29) CHECK((v>>4)<=1);
            aa+=(v>>4)>0 && (v>>4)<15;
            fill+=v==255; outline+=v==240;
            ring+=(v>>4)>0 && (v&15)<15;
        }
        CHECK(aa>50 && fill>100 && ring>50);
        // The title's 1/4-texel ring has partial coverage, with no fully
        // opaque black texel; rows retain a full dark ring.
        if(art!=mu_baked_slippi_title) CHECK(outline>5);
    }
    CHECK(std::memcmp(mu_baked_slippi,mu_baked_slippi_title,MU_LABEL_BYTES)!=0);
    unsigned char pixels[8]{};
    HSD_ImageDesc native[8];
    HSD_ImageDesc border{nullptr,64,64,GX_TF_IA4},other{nullptr,176,30,6};
    HSD_ImageDesc* table[]={&border};
    HSD_TObj format{nullptr,&other,table},edge{&format,&border,table},texture{&edge,nullptr,table};
    HSD_MObj material{&texture,0xffcc00};HSD_DObj draw{nullptr,&material},missing{&draw,nullptr};
    HSD_JObj label{{&missing}};
    menus=offered=1;
    for(int slot=0;slot<8;++slot) {
        native[slot]={pixels+slot,176,30,GX_TF_IA4};texture.imagedesc=&native[slot];
        mu_slippi_friends_label(&label,MENU_KIND_8,slot,1);
        CHECK(texture.imagedesc==&native[slot]);
    }
    auto originals=native[0];
    for(menus=0;menus<=1;++menus)for(offered=0;offered<=1;++offered)
      for(submenu=0;submenu<=1;++submenu)for(int menu:{0,1,8})for(int row=-1;row<9;++row) {
        const int slot=row>=0&&row<8?row:0;
        texture.imagedesc=&native[slot];
        mu_slippi_friends_label(&label,menu,row,0);
        int kind=mu_slippi_menu_label_kind(row);
        bool replace=menus&&offered&&menu==8&&kind>=1&&kind<=5;
        CHECK((texture.imagedesc!=&native[slot])==replace);
        if(replace) {
            if(submenu) CHECK(texture.imagedesc==&native[2+row]);
            else CHECK(texture.imagedesc->image_ptr==(row==2?mu_baked_friends:mu_baked_slippi));
            CHECK(texture.imagedesc->width==176&&texture.imagedesc->height==30&&texture.imagedesc->format==GX_TF_IA4);
        }
        CHECK(texture.imagetbl==table&&table[0]==&border);
        CHECK(edge.imagedesc==&border&&format.imagedesc==&other&&material.color==0xffcc00);
        CHECK(std::memcmp(&native[0],&originals,sizeof originals)==0);
    }
    menus=offered=1;
    for(int sub : {0,1,0})for(int row=0;row<(sub?3:4);++row)
      for(unsigned color:{0xffcc00u,0u,0xffcc00u}) {
        submenu=sub;texture.imagedesc=&native[row];material.color=color;
        mu_slippi_friends_label(&label,8,row,0);
        if(sub) CHECK(texture.imagedesc==&native[2+row]);
        else if(row<2) CHECK(texture.imagedesc==&native[row]);
        else CHECK(texture.imagedesc->image_ptr==(row==2?mu_baked_friends:mu_baked_slippi));
        CHECK(material.color==color);
    }
    submenu=1;
    for(int row=0;row<2;++row) {
        texture.imagedesc=&native[row];mu_slippi_friends_label(&label,8,row,0);
        CHECK(texture.imagedesc==&native[row+2]);
        submenu=0;mu_slippi_friends_label(&label,8,row,0);
        CHECK(texture.imagedesc==&native[row]); // Back also restores a reused JObj
        submenu=1;
    }
    for(int bad=0;bad<3;++bad) {
        native[2]={pixels+2,176,30,GX_TF_IA4};
        if(bad==0)native[2].width=168;if(bad==1)native[2].height=32;if(bad==2)native[2].format=0;
        texture.imagedesc=&native[0];mu_slippi_friends_label(&label,8,0,0);
        CHECK(texture.imagedesc==&native[0]);
    }
    menus=offered=tournament=1; submenu=0;
    texture.imagedesc=&native[3]; mu_slippi_friends_label(&label,8,3,0);
    CHECK(texture.imagedesc->image_ptr==mu_baked_tournament);
    CHECK(texture.imagedesc->width==176 && texture.imagedesc->height==30);
    texture.imagedesc=&native[4]; mu_slippi_friends_label(&label,8,4,0);
    CHECK(texture.imagedesc->image_ptr==mu_baked_slippi);
    CHECK(texture.imagetbl==table);
    tournament=0; texture.imagedesc=&native[3]; mu_slippi_friends_label(&label,8,3,0);
    CHECK(texture.imagedesc->image_ptr==mu_baked_slippi);
    mu_slippi_friends_label(nullptr,8,0,0);
    std::printf("native/baked labels: %d checks passed (8 originals, every top/submenu row, restore, IA4 AA/padding/guards)\n",checks);
}
