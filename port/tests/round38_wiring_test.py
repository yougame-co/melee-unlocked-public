"""Round 38: shaped focus rendering and the embedded broker refusal path, offline."""
from pathlib import Path
import unittest
from yougame_console_wiring_test import body
import online_menu_test as menu

ROOT = Path(__file__).resolve().parents[2]


def read(name):
    return (ROOT / name).read_text()


class Round38Wiring(unittest.TestCase):
    def test_shape_helpers_execute_visibility_and_exact_face(self):
        visuals = read('port/runtime/gx/pc_settings_visuals.inl')
        helpers = '\n'.join('static ' + signature + ' {' + body(visuals, name) + '}' for name, signature in (
            ('settings_shaped_hit_button', 'bool settings_shaped_hit_button(const char* id, ImVec2 size)'),
            ('settings_nav_frame_on_item', 'bool settings_nav_frame_on_item()'),
            ('settings_nav_frame', 'void settings_nav_frame(ImDrawList* draw, const ImVec2 (&quad)[4])')))
        menu.OnlineMenuTest().compile_fixture('round38-focus', r'''
#include <cstdio>
#include <cstdlib>
#include <algorithm>
using ImU32=unsigned;
struct ImVec2 { float x,y; ImVec2(float a=0,float b=0):x(a),y(b){} };
struct ImVec4 { float x,y,z,w; ImVec4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){} };
#define IM_COL32(r,g,b,a) ((unsigned)(r)|((unsigned)(g)<<8)|((unsigned)(b)<<16)|((unsigned)(a)<<24))
enum { ImGuiCol_NavCursor, ImDrawFlags_Closed };
struct ImGuiContext { bool NavCursorVisible; unsigned NavId; struct { unsigned ID; } LastItemData; } ctx;
ImGuiContext* GImGui=&ctx;
struct Window { struct { bool NavHideHighlightOneFrame; } DC; } window;
static unsigned style=0xff884422; static bool transparent=false;
struct ImDrawList {
  void AddPolyline(const ImVec2* p,int n,unsigned color,int flags,float width) {
    if (n!=4 || flags!=ImDrawFlags_Closed || width!=2 || color==style || p[0].x!=9 ||
        p[1].x!=200 || p[2].y!=50 || p[3].x!=0) std::abort();
  }
};
namespace ImGui {
void PushStyleColor(int,unsigned color) { transparent=color==0; }
void PopStyleColor() { transparent=false; }
Window* GetCurrentWindow() { return &window; }
unsigned GetColorU32(int) { return style; }
ImVec4 ColorConvertU32ToFloat4(unsigned) { return ImVec4(.2f,.1f,.4f,1); }
unsigned ColorConvertFloat4ToU32(ImVec4 c) { return IM_COL32(c.x*255,c.y*255,c.z*255,255); }
}
bool settings_hit_button(const char*, ImVec2) { if (!transparent) std::abort(); return true; }
#define CHECK(x) do { if (!(x)) std::abort(); } while(0)
''' + helpers + r'''
int main() {
  int checks=0;
  for (bool visible : {false,true}) for (unsigned nav : {0u,7u})
  for (unsigned item : {0u,7u,8u}) for (bool hidden : {false,true}) {
    ctx={visible,nav,{item}}; window.DC.NavHideHighlightOneFrame=hidden;
    CHECK(settings_nav_frame_on_item()==(visible && nav && item==nav && !hidden)); ++checks;
  }
  CHECK(settings_shaped_hit_button("button",ImVec2(200,58))); CHECK(!transparent); checks+=2;
  const ImVec2 face[]={ImVec2(9,-4),ImVec2(200,-4),ImVec2(191,50),ImVec2(0,50)};
  ImDrawList draw; settings_nav_frame(&draw,face); ++checks;
  printf("shaped focus: %d checks passed\n",checks);
}
''')

    def test_pause_keeps_navigation_activation_and_outlines_lifted_face(self):
        ui = read('port/runtime/gx/pc_settings.cpp')
        action = ui[ui.index('    auto action = '):ui.index('    if (!state.menu_quit) {', ui.index('    auto action = '))]
        self.assertIn('ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));', action)
        self.assertLess(action.index('PushStyleColor'), action.index('ImGui::InvisibleButton'))
        self.assertIn('ImGuiButtonFlags_EnableNav', action)
        self.assertIn('ImGui::PopStyleColor();', action)
        self.assertIn('if (settings_nav_frame_on_item())\n        draw->AddPolyline(face, 4, settings_mix_color(accent, IM_COL32_WHITE, .75f), ImDrawFlags_Closed, 2.0f);', action)
        self.assertIn('p.y + y - (hot ? 4.0f : 0.0f)', action)
        for pin in ('ImGui::SetFocusID', 'ImGui::SetNavCursorVisible(true)', 'agent_menu_focus',
                    'agent_menu_label', 'state.menu_quit && enter', '!quit_button_focused'):
            self.assertIn(pin, action)
        self.assertEqual(ui[ui.index('    auto action = '):].count('ImGui::SetItemDefaultFocus();'), 2)

    def test_all_other_custom_shapes_checked(self):
        ui = read('port/runtime/gx/pc_settings.cpp')
        visuals = read('port/runtime/gx/pc_settings_visuals.inl')
        for name in ('settings_gd_home', 'settings_clean_home', 'settings_nav_tile'):
            self.assertIn('settings_nav_frame', body(ui, name))
        self.assertIn('point(x+444+dx, y+30+dy)', body(ui, 'settings_gd_home'))
        self.assertIn('row.point(540+row.lift,row.y+30+row.lift)', body(visuals, 'settings_gd_row'))
        for name in ('settings_gd_row', 'settings_page_footer', 'settings_dashboard_tiles'):
            self.assertIn('settings_shaped_hit_button', body(visuals, name))
            self.assertIn('settings_nav_frame', body(visuals, name))
        self.assertIn('slanted ? settings_shaped_hit_button', visuals)
        self.assertIn('if (nav_frame) settings_nav_frame(draw,strip);', visuals)
        for item in ('##clean_home_chip', '##clean_categories'):
            self.assertIn('settings_shaped_hit_button("' + item + '"', ui)
        self.assertIn('settings_hit_button("##wheel_category", hit, 0)', body(ui, 'settings_radial_nav'))
        waiting = ui[ui.index('ImGui::Begin("##yougame_friend_waiting"'):]
        waiting = waiting[:waiting.index('ImGui::End();')]
        self.assertNotIn('Button', waiting)
        self.assertIn('ImGuiWindowFlags_NoInputs', ui)
        tile = body(ui, 'settings_nav_tile')
        self.assertIn('if (gd_kit) ImGui::PushStyleColor(ImGuiCol_NavCursor', tile)
        self.assertIn('if (settings_nav_frame_on_item()) settings_nav_frame(draw, face);', tile)

    def test_broker_close_flows_to_friends_notice_and_safe_log(self):
        link = read('port/runtime/hle/yougame_link.cpp')
        console = read('port/runtime/hle/yougame_console_link.cpp')
        self.assertIn('broker_->lobby_mismatch()', link)
        self.assertIn('io.room_mismatch = [b] { return b->room && b->room->lobby_mismatch(); };', link)
        self.assertIn('i.room_mismatch && i.room_mismatch() ? kMixedBuilds', console)
        self.assertIn('session_notice(error) ? session_notice(error)', body(link, 'friends_entry'))
        self.assertIn('console: room open refused (close %d, %s)', link)
        self.assertIn('room->lobby_mismatch() ? "lobby_mismatch" : "unclassified"', link)
        self.assertIn('return fail(kMixedBuilds)', console)
        self.assertNotIn('MELEE_SOURCE_PORT', body(link, 'console_io'))

    def test_production_room_refusal_log_executes(self):
        link = read('port/runtime/hle/yougame_link.cpp')
        start = link.index('    if (!room->open_session("",ticket,code,&error,ms,session))')
        refusal = link[start:link.index('    conn = room->connection();', start)]
        menu.OnlineMenuTest().compile_fixture('round38-room-log', r'''
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
static std::string logged;
namespace host { void log(const char* fmt,...) { char b[200]; va_list a; va_start(a,fmt);
  vsnprintf(b,sizeof b,fmt,a); va_end(a); logged=b; } }
struct Socket { bool mismatch=false;
  bool open_session(const char*,const std::string&,const std::string&,std::string*,int,const std::string&) { return false; }
  int close_code() const { return 1006; } bool lobby_mismatch() const { return mismatch; }
} socket;
bool attempt() { auto* room=&socket; std::string ticket,code,error,session; int ms=100;
''' + refusal + r'''
  return true;
}
int main() {
  for (bool mismatch : {false,true}) {
    socket.mismatch=mismatch; if (attempt()) std::abort();
    const std::string want=std::string("console: room open refused (close 1006, ")+
        (mismatch?"lobby_mismatch":"unclassified")+")";
    if (logged!=want) std::abort();
  }
  puts("room refusal log: 4 checks passed");
}
''')

    def test_protocol_key_both_branches_and_comment(self):
        io = body(read('port/runtime/hle/yougame_link.cpp'), 'console_io')
        self.assertEqual(io.count('+ "-console4";'), 2)
        self.assertIn('console4 adds game-local save choices and rejoin agreement', io)


if __name__ == '__main__':
    unittest.main()
