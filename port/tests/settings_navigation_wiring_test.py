"""Overlay navigation uses the captured UI pad; gameplay retains no overlay presses."""
from pathlib import Path
import unittest
import online_menu_test as menu

ROOT = Path(__file__).resolve().parents[2]


class SettingsNavigationTest(unittest.TestCase):
    def test_auto_lcancel_dirty_state_and_ranked_lock_execute_production_branch(self):
        ui = (ROOT / 'port/runtime/gx/pc_settings.cpp').read_text()
        start = ui.index('      const bool ranked = lcancel::automatic_setting_locked();')
        branch = ui[start:ui.index('      if (ranked) settings_hint', start)]
        menu.OnlineMenuTest().compile_fixture('settings-auto-save', r'''
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>
static bool locked, vanilla, click, stored, disabled;
static int frame;
static std::vector<std::string> lines;
namespace host {
void log(const char* fmt, ...) { char b[256]; va_list a; va_start(a,fmt);
  vsnprintf(b,sizeof b,fmt,a); va_end(a); lines.emplace_back(b); }
namespace gameplay { bool vanilla_only() { return vanilla; } }
}
namespace lcancel {
bool automatic_setting_locked() { return locked; }
bool automatic_enabled() { return stored; }
void set_automatic(bool v) { stored=v; }
}
enum { ImGuiHoveredFlags_AllowWhenDisabled };
namespace ImGui {
void BeginDisabled(bool v) { disabled=v; } void EndDisabled() { disabled=false; }
bool IsItemHovered(int) { return click; } bool IsMouseClicked(int) { return click; }
int GetFrameCount() { return frame; }
}
bool settings_toggle(const char*, bool* v) { if (!disabled && click) { *v=!*v; return true; } return false; }
bool change() { ++frame; bool changed=false;
''' + branch + r'''
return changed;
}
#define CHECK(x) do { if (!(x)) std::abort(); } while(0)
int main() {
  click=true; CHECK(change() && stored); CHECK(lines.back()=="settings: Auto L-cancel on");
  CHECK(change() && !stored); CHECK(lines.back()=="settings: Auto L-cancel off");
  stored=true; locked=true; CHECK(!change() && stored);
  CHECK(lines.back()=="settings: Auto L-cancel off (locked in Ranked)");
  auto n=lines.size(); click=false; CHECK(!change() && stored); CHECK(lines.size()==n);
  frame+=4; CHECK(!change()); CHECK(lines.size()==n+1);
  locked=false; vanilla=true; click=true; n=lines.size();
  CHECK(!change() && stored); CHECK(lines.size()==n);
  puts("Auto L-cancel: 12 checks; dirty on/off, preserved Ranked/vanilla preferences, transition-only lock logs");
}
''')
        self.assertIn('if (changed) state.dirty = true;', ui)
        self.assertIn('<< "\\nautolcancel " << (lcancel::automatic_enabled() ? 1 : 0)', ui)
        self.assertIn('else if (key == "autolcancel") lcancel::set_automatic(value == "1");', ui)

    def test_success_and_failure_logs_follow_actual_file_write(self):
        ui = (ROOT / 'port/runtime/gx/pc_settings.cpp').read_text()
        start = ui.index('  file.close();', ui.index('static bool write_settings_file('))
        tail = ui[start:ui.index('\n}', start)]
        menu.OnlineMenuTest().compile_fixture('settings-write-result', r'''
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
static std::vector<std::string> lines;
namespace host { void log(const char* s) { lines.emplace_back(s); } }
enum { MOVEFILE_REPLACE_EXISTING=1, MOVEFILE_WRITE_THROUGH=2 };
bool MoveFileExW(const char* from, const char* to, int) {
  std::error_code e; std::filesystem::rename(from,to,e); return !e;
}
bool save(std::filesystem::path path, bool automatic) {
  auto temporary=path; temporary+=".tmp"; std::ofstream file(temporary);
  file << "autolcancel " << automatic << '\n';
''' + tail + r'''
}
#define CHECK(x) do { if (!(x)) std::abort(); } while(0)
int main(int, char** argv) {
  auto root=std::filesystem::path(argv[0]).parent_path();
  for (bool v : {true,false}) {
    CHECK(save(root/"settings.ini",v)); CHECK(lines.back()=="settings: file saved");
    std::ifstream in(root/"settings.ini"); std::string key; int value=-1; in>>key>>value;
    CHECK(key=="autolcancel" && value==v);
  }
  CHECK(!save(root/"absent"/"settings.ini",true));
  CHECK(lines.back()=="settings: file save failed"); CHECK(lines.size()==3);
  puts("settings write: 9 checks; on/off round-trip and failed write logs");
}
''')

    def test_page_focus_and_all_other_unsaved_controls_are_wired(self):
        ui = (ROOT / 'port/runtime/gx/pc_settings.cpp').read_text()
        self.assertIn('ImGuiChildFlags_NavFlattened |', ui)
        self.assertIn('state.page_navigation.begin(state.active_tab, options.overlay_style);', ui)
        self.assertIn('if (!state.open || !state.page_navigation.active) state.page_navigation.leave();', ui)
        for style in ('clean', 'dashboard', 'gd', 'radial', 'wide'):
            self.assertIn('state.' + style + '_detail_open = false;', ui)
        self.assertIn('host::discord::configure(options.discord_app_id);\n      changed = true;', ui)
        self.assertIn('if (options.discord_presence != discord_was) {\n      changed = true;', ui)
        self.assertIn('changed |= settings_toggle("Performance overlay", &options.performance_overlay);', ui)

    def test_backend_summary_uses_actual_factory_result_including_fallbacks(self):
        from yougame_console_wiring_test import body
        win = (ROOT / 'port/runtime/gx/gx_backend.cpp').read_text()
        header = (ROOT / 'port/runtime/gx/gx_backend.h').read_text()
        start = header.index('inline std::atomic<const char*> started_backend_name')
        tracker = header[start:header.index('// Creates the requested backend;', start)]
        menu.OnlineMenuTest().compile_fixture('started-backend', r'''
#include <atomic>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <string>
struct Backend {} d11,d12,vk;
enum class RenderApi { D3D11,D3D12,Vulkan };
struct RenderOptions { RenderApi api; int dlss_mode=0,frame_generation_mode=0; };
struct VulkanWindow { void* window; std::function<void(int*,int*)> drawable_size; };
using HWND=void*; struct RECT { int left,top,right,bottom; };
void GetClientRect(HWND, RECT*) {}
namespace host { void log(const char*,...) {} }
bool vk_ok,d11_ok,usable;
Backend* create_vulkan_backend(VulkanWindow,int,int,RenderOptions) { return vk_ok?&vk:nullptr; }
Backend* create_d3d11_backend(void*,int,int,RenderOptions) { return d11_ok?&d11:nullptr; }
Backend* create_d3d12_backend(void*,int,int,RenderOptions) { return &d12; }
bool d3d12_usable() { return usable; }
''' + tracker + '\nBackend* create_render_backend(void* hwnd,int client_w,int client_h,const RenderOptions& options) {' + body(win, 'create_render_backend') + r'''
}
int main() {
  int checks=0;
  for (auto api : {RenderApi::D3D11,RenderApi::D3D12,RenderApi::Vulkan})
    for (bool v : {false,true}) for (bool d : {false,true}) for (bool u : {false,true}) {
      vk_ok=v; d11_ok=d; usable=u;
      Backend* expected=api==RenderApi::Vulkan && v ? &vk :
        ((api==RenderApi::D3D11 || (api==RenderApi::D3D12 && !u)) && d ? &d11 : &d12);
      if (create_render_backend(nullptr,640,480,{api})!=expected) std::abort(); ++checks;
      const char* name=expected==&vk?"Vulkan":expected==&d11?"D3D11":"D3D12";
      if (std::string(started_render_backend())!=name) std::abort(); ++checks;
    }
  printf("backend startup: %d checks across 24 actual/fallback combinations\n",checks);
}
''')
        linux = (ROOT / 'port/linux/full/gx_backend_linux.cpp').read_text()
        self.assertIn('return record_started_backend(backend, "Vulkan");', linux)
        self.assertIn('return record_started_backend(renderer.release(), "OpenGL");', linux)
        main = (ROOT / 'port/app/main.cpp').read_text()
        self.assertIn('backend ? gx::started_render_backend() : "none (headless)"', main)
        threaded = (ROOT / 'port/runtime/gx/threaded_backend.cpp').read_text()
        self.assertLess(threaded.index('renderer(create_render_backend('), threaded.index('init.set_value();'))
        self.assertIn('try { ready.get(); }', threaded)

    def test_settings_menu_and_practice_execute_the_same_pad_branch(self):
        ui = (ROOT / 'port/runtime/gx/pc_settings.cpp').read_text()
        nav = 'const bool nav_active = state.open || state.menu_open || practice_nav || state.fill_window;'
        start = ui.index('  // The PhobGCC menu reads the controller itself:')
        branch = ui[start:ui.index('  ImGui::NewFrame();', start)]
        from yougame_console_wiring_test import body
        helper = (ROOT / 'port/runtime/gx/settings_imgui_input.h').read_text()
        mapping = body(helper, 'gamepad')
        menu.OnlineMenuTest().compile_fixture('settings-routed-nav', r'''
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
using ImGuiKey = int;
enum { ImGuiBackendFlags_HasGamepad=1, ImGuiKey_GamepadStart, ImGuiKey_GamepadBack,
       ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadDpadUp,
       ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight };
struct IO { int BackendFlags=0, events=0; bool keys[10]{};
  void AddKeyEvent(int key, bool down) { ++events; keys[key]=down; } } io;
namespace ImGui { IO& GetIO() { return io; } }
static bool phob;
static bool phob_native_open() { return phob; }
struct State { bool open=false, menu_open=false, fill_window=false, practice_pad_armed=false; };
struct Pad { unsigned button; int stick_x,stick_y; };
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) std::abort(); } while(0)
namespace settings_input { void gamepad(IO& io, const Pad& pad) {
''' + mapping + r'''
} }
static void feed(State state, bool practice_nav, bool have_pad, Pad pad) {
  Pad navigation_pads[4] = {pad}; int nav_port = 0;
''' + nav + branch + r'''
}
int main() {
  for (int mode=0;mode<5;++mode) for (bool present : {false,true})
    for (bool armed : {false,true}) for (bool custom : {false,true}) {
      State s; s.open=mode==1; s.menu_open=mode==2; s.fill_window=mode==4;
      s.practice_pad_armed=armed; phob=custom; io={};
      feed(s,mode==3,present,{0x131f,80,-80});
      bool active=mode!=0 && present && armed;
      CHECK(io.events==(active?8:0));
      CHECK(bool(io.BackendFlags & ImGuiBackendFlags_HasGamepad)==(active&&!custom));
      for (int k=ImGuiKey_GamepadStart;k<=ImGuiKey_GamepadDpadRight;++k)
        CHECK(io.keys[k]==(active&&!custom));
    }
  std::printf("settings routed pad: %d checks across 40 closed/settings/menu/practice/full-window, armed, disconnected and Phob cases\n",checks);
}
''')

    def test_open_close_flags_and_practice_share_the_routed_pad(self):
        ui = (ROOT / 'port/runtime/gx/pc_settings.cpp').read_text()
        frame = ui[ui.index('bool settings_frame('):]
        start = frame.index('settings_input::navigation(io, state.open || state.menu_open || practice_nav || state.fill_window);')
        self.assertLess(start, frame.index('ImGui_ImplWin32_NewFrame();'))
        self.assertIn('const bool nav_active = state.open || state.menu_open || practice_nav || state.fill_window;', frame)
        self.assertIn('int nav_port = practice_nav ? std::clamp(practice.controller_port, 0, 3) : followed;', frame)
        self.assertIn('else if (have_pad && nav_active && state.practice_pad_armed)', frame)
        self.assertIn('if (have_pad && nav_active && state.practice_pad_armed && phob_native_open())', frame)
        self.assertIn('settings_input::gamepad(io, navigation_pads[nav_port]);', frame)
        # Final state applies after even the small menu's resume action.
        final = frame.rindex('settings_input::navigation(')
        self.assertGreater(final, frame.index('if (action("##resume", "BACK TO GAME", 0))'))
        self.assertIn('practice.phase == slippi::native_practice::Phase::Failure || state.fill_window)', frame[final:final + 240])
        self.assertIn('state.practice_release_capture || replay_bar::wants_cursor()', frame)
        self.assertIn('settings_toggle("Auto L-cancel", &automatic)', frame)

    def test_capture_keeps_ui_pads_and_neutralizes_all_game_ports(self):
        host = (ROOT / 'port/runtime/host/window.cpp').read_text()
        start = host.index('    g_ui_pad = pads[0];')
        end = host.index('#ifdef YOUGAME_AGENT_TEST_HOOK', start)
        capture = host[start:end]
        menu.OnlineMenuTest().compile_fixture('settings-pad-capture', r'''
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
struct Pad { unsigned short button; signed char stick_x, stick_y, sub_x, sub_y;
             unsigned char trigger_l, trigger_r; signed char err; };
static Pad pads[4], g_ui_pads[4], g_ui_pad;
static bool g_ui_gamecube, g_ui_has_pad, gamecube;
static constexpr bool console = false; // offline publisher still suppresses all gameplay ports
static std::atomic<bool> g_ui_capture;
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) std::abort(); } while(0)
static void poll() {
''' + capture + r'''
}
int main() {
  for (bool open : {true, false, true, false}) {
    for (int i=0;i<4;++i) pads[i]={0x104, 75, -60, 40, -30, 90, 80, (signed char)(i==3?-1:0)};
    g_ui_capture=open; poll();
    CHECK(g_ui_has_pad);
    for (int i=0;i<4;++i) {
      CHECK(g_ui_pads[i].button==0x104 && g_ui_pads[i].stick_x==75);
      CHECK(pads[i].err==(i==3?-1:0));
      CHECK(pads[i].button==(open?0:0x104));
      CHECK(pads[i].stick_x==(open?0:75));
      CHECK(pads[i].stick_y==(open?0:-60));
      CHECK(pads[i].sub_x==(open?0:40) && pads[i].sub_y==(open?0:-30));
      CHECK(pads[i].trigger_l==(open?0:90) && pads[i].trigger_r==(open?0:80));
    }
  }
  std::printf("settings capture: %d checks; UI keeps raw pads, all four game ports neutral while open and restored closed\n",checks);
}
''')


if __name__ == '__main__':
    unittest.main()
