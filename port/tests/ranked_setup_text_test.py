"""Execute production GameSetup header helpers at the SIS API boundary.

This verifies calls, strings and measured bounds; it cannot verify Windows pixels.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sourceport/game/shim/mu_ranked_widgets.c"


class SetupTextTest(unittest.TestCase):
    def test_header_after_scene_font_reset(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "C++ compiler required for production helper fixture")
        source = SOURCE.read_text()
        # Execute the exact production functions, with only the SIS boundary replaced.
        helpers = source.split("static HSD_Text* text_create(", 1)[1]
        helpers = "static HSD_Text* text_create(" + helpers.split("static void header_ranks(", 1)[0]
        fixture = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#define CHECK(v) do { if (!(v)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#v); std::exit(1); } } while (0)
static unsigned char slot;
static unsigned char* HSD_SisLib_804D1124[5];
struct HSD_GObj { void (*user_data_remove_func)(void*) = nullptr; };
struct Storage { unsigned char *data=&slot, *end=&slot; };
struct HSD_Text {
    Storage* alloc_data;
    unsigned char* sis_buffer=&slot;
    HSD_GObj* entity;
    float pos_x=0,pos_y=0,pos_z=0,box_size_x=0,box_size_y=0;
    int default_kerning=0,default_alignment=0;
    struct {float x=1,y=1;} x34,font_size;
    struct Row {float x,y,sx,sy;std::string value;};
    std::vector<Row> rows;
    Storage owned_storage;
    HSD_GObj owned_entity;
};
struct MuRankedSet {unsigned cur_game=2,score[2]={1,0};};
struct MuRankedSetupModel {unsigned local_player=0;MuRankedSet* set;};
struct MuMatchState {char names[2][31];char codes[2][10];};
struct MuRankedWidgets {unsigned canvas=0;HSD_Text *text=nullptr,*names[2]={};};
static unsigned allocated,loads,clears;
static int failure,practice;
static int mu_ranked_practice() {return practice;}
static std::vector<std::string> logs;
static void destroy(void*) {}
static void OSReport(const char* message) {logs.emplace_back(message);}
static void HSD_SisLib_803A5E70() {++clears;}
static void HSD_SisLib_803A62A0(int font,const char* archive,const char* symbol) {
    CHECK(font==0 && !HSD_SisLib_804D1124[0]);
    CHECK(!std::strcmp(archive,"SdSlChr.usd") && !std::strcmp(symbol,"SIS_SelCharData"));
    ++loads;HSD_SisLib_804D1124[0]=&slot;
}
static int HSD_SisLib_803A611C(int font,void* camera,int cls,int link,int prio,int gx,int gxprio,int render) {
    CHECK(font==0 && camera==nullptr && cls==9 && link==18 && prio==0 && gx==3 && gxprio==0 && render==19);
    return 0;
}
static HSD_Text* HSD_SisLib_803A6754(int font,int canvas) {
    CHECK(font==0 && canvas==0 && HSD_SisLib_804D1124[0]);
    if(failure==1)return nullptr;
    auto* t=new HSD_Text{};++allocated;
    t->alloc_data=&t->owned_storage;t->entity=&t->owned_entity;
    t->entity->user_data_remove_func=destroy;
    if(failure==2)t->alloc_data=nullptr;
    if(failure==3)t->alloc_data->data=nullptr;
    if(failure==4)t->alloc_data->end=nullptr;
    if(failure==5)t->sis_buffer=nullptr;
    if(failure==6)t->entity=nullptr;
    if(failure==7)t->entity->user_data_remove_func=nullptr;
    return t;
}
static void HSD_SisLib_803A5CC4(HSD_Text* t) {--allocated;delete t;}
static int HSD_SisLib_803A6B98(HSD_Text* t,float x,float y,const char*,const char* value) {
    CHECK(t && t->alloc_data && t->sis_buffer && t->entity);
    // SIS stores offsets as signed integers and glyph scales as absolute values.
    t->rows.push_back({float(short(x)),float(short(y)),t->x34.x,t->x34.y,value});
    return int(t->rows.size()-1);
}
static void HSD_SisLib_803A7548(HSD_Text* t,unsigned id,float x,float y) {t->rows.at(id).sx=x;t->rows.at(id).sy=y;}
static void HSD_SisLib_803A70A0(HSD_Text* t,unsigned id,const char*,const char* value) {t->rows.at(id).value=value;}
'''
        fixture += helpers
        fixture += r'''
struct Box {float l,t,r,b;};
static Box bounds(HSD_Text* text,unsigned id) {
    auto& row=text->rows.at(id);
    float width=row.value.size()*20.5f*row.sx;
    float x=text->pos_x+row.x;
    if(text->default_alignment==1)x-=width/2;
    if(text->default_alignment==2)x-=width;
    return {x,text->pos_y+row.y,x+width,text->pos_y+row.y+24*row.sy};
}
static bool collision(Box a,Box b) {return a.l<b.r && b.l<a.r && a.t<b.b && b.t<a.b;}
int main() {
    MuRankedSet set;MuRankedSetupModel model{0,&set};MuMatchState ms{};
    // Worst-case measured width, with distinct local/opponent data.
    std::memset(ms.names[0],'L',30);std::memset(ms.names[1],'R',30);
    std::strcpy(ms.codes[0],"LEFT00000");std::strcpy(ms.codes[1],"RIGHT0000");
    for(unsigned local:{0u,1u}) {
        model.local_player=local;HSD_SisLib_804D1124[0]=nullptr;
        MuRankedWidgets w;header_text(&w,&model,&ms);header_score(&w,&model);
        CHECK(w.text && w.names[0] && w.names[1] && w.text->rows.size()==6);
        CHECK(w.text->pos_x==320 && w.text->pos_y==45 && w.text->rows[0].sx==.7f);
        CHECK(w.names[0]->rows[0].value==ms.names[local]);
        CHECK(w.names[1]->rows[0].value==ms.names[!local]);
        CHECK(w.names[0]->rows[1].value==ms.codes[local]);
        CHECK(w.names[1]->rows[1].value==ms.codes[!local]);
        CHECK(w.text->rows[5].value==(local?"Game 2 - 0:1":"Game 2 - 1:0"));
        text_set(w.text,0,"0:30");
        // Agent practice runs say so on the score row, which must still clear everything.
        for(int p:{1,0}) {
            practice=p;header_score(&w,&model);
            if(p)CHECK(w.text->rows[5].value==(local?"Practice  Game 2 - 0:1":"Practice  Game 2 - 1:0"));
            std::vector<Box> boxes={bounds(w.text,0),bounds(w.text,5)};
            for(auto* name:w.names)for(unsigned row:{0u,1u})boxes.push_back(bounds(name,row));
            for(unsigned i=0;i<boxes.size();++i) {
                auto box=boxes[i];CHECK(box.l>=0 && box.r<=640 && box.t>=0 && box.b<=110);
                for(unsigned j=0;j<i;++j)CHECK(!collision(box,boxes[j]));
                // Reserve 24px for each arrow and 18px for the best-of-nine pips.
                for(auto reserved:{Box{248,49,272,73},Box{368,49,392,73},Box{223,16.5f,417,34.5f}})
                    CHECK(!collision(box,reserved));
            }
        }
        CHECK(w.text->rows[5].value==(local?"Game 2 - 0:1":"Game 2 - 1:0"));
        CHECK(bounds(w.text,5).t==92 && w.text->rows[5].sx==.45f);
        for(auto* t:{w.text,w.names[0],w.names[1]})HSD_SisLib_803A5CC4(t);
        CHECK(allocated==0 && logs.empty());
    }
    CHECK(loads==2 && clears==2);
    MuRankedWidgets w;header_text(&w,&model,&ms);CHECK(loads==2); // no redundant archive load
    for(auto* t:{w.text,w.names[0],w.names[1]})HSD_SisLib_803A5CC4(t);
    for(failure=0;failure<=7;++failure) {
        HSD_SisLib_804D1124[0]=failure?&slot:nullptr;
        logs.clear();CHECK(!text_create(&w,320,45,.7f));CHECK(allocated==0 && logs.size()==1);
        const auto& log=logs[0];CHECK(log.find("text_create failed:")!=std::string::npos);
        CHECK(log.back()=='\n' && log.find('\n')==log.size()-1);
        CHECK(log.find(ms.names[0])==std::string::npos && log.find(ms.names[1])==std::string::npos);
    }
    std::puts("setup header: font reset, both seats, score, bounds, 8 failure paths verified");
}
'''
        with tempfile.TemporaryDirectory(prefix="ranked-setup-text-") as directory:
            cpp = Path(directory) / "fixture.cpp"
            exe = Path(directory) / "fixture"
            cpp.write_text(fixture)
            subprocess.run([compiler, "-std=c++17", "-Wno-deprecated-declarations", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
