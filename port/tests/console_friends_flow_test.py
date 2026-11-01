"""Round 37b: execute console device routing, first-scene selection and invite UI state offline."""
from pathlib import Path
import unittest
import sys
import online_menu_test as menu
from yougame_console_wiring_test import body

ROOT = Path(__file__).resolve().parents[2]


class ConsoleFriendsFlowTest(unittest.TestCase):
    def compile(self, name, harness):
        menu.OnlineMenuTest().compile_fixture(name, harness,
            *(['-arch', 'x86_64'] if sys.platform == 'darwin' else []),
            '-I', str(ROOT / 'port/runtime/host'),
            '-I', str(ROOT / 'port/runtime/ppc'),
            '-I', str(ROOT / 'port/linux/compat'))

    def test_relay_keyboard_through_production_console_read(self):
        window = (ROOT / 'port/runtime/host/window.cpp').read_text()
        harness = r''' 
#include "yougame_controller_relay.h"
#include "keyboard_ui_navigation.h"
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
static int checks, reads, native_reads;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
SHORT GetAsyncKeyState(int) { ++native_reads; return 0; }
namespace yougame { bool embedded() { return true; } }
namespace host {
HWND g_hwnd=(HWND)1;
bool focused=true, g_background_input=true, g_keys[256]{};
bool window_focused(HWND) { return focused; }
std::mutex g_keys_mutex,g_ds4_mutex,g_devices_mutex,g_last_pads_mutex;
KeyBindings g_key_bindings=default_key_bindings();
std::array<PadBindings,4> g_pad_bindings=default_pad_bindings(); std::array<GCBindings,4> g_gc_bindings=default_gc_bindings();
std::array<PadBindings,4> g_ds4_bindings{};
std::array<Deadzone,(size_t)PadFamily::Count> g_deadzones{};
std::array<PortSource,4> g_port_sources=default_port_sources();
std::array<PortSource,4> g_port_feeding{};
std::array<std::string,4> g_port_device_names{};
box::State g_box_keyboard[2],g_box_keyboard_navigation,g_box_hid[1][4];
int g_stick_modifier_percent=50;
PadState g_ds4_pads[4]{},g_last_pads[4]{};
uint16_t g_ds4_buttons[4]{}; void* g_ds4_devices[4]{};
void log(const char*,...) {}
uint32_t gcadapter_poll(PadState*) { ++reads; return 0; }
void apply_family_options(PadFamily,PadState&) {}
void apply_known_box_layout(int) {}
std::string hidpad_name(int) { return {}; }
bool xinput_state(int idx,XINPUT_STATE* out) {
 controller_relay::Sample s;
 if(!controller_relay::sample(CaptureDevice::XInputPad,idx,s)) return false;
 *out={}; auto& g=out->Gamepad; g.wButtons=s.buttons; g.bLeftTrigger=s.left; g.bRightTrigger=s.right;
 g.sThumbLX=s.axes[0];g.sThumbLY=s.axes[1];g.sThumbRX=s.axes[2];g.sThumbRY=s.axes[3];return true;
}
uint32_t switchpro_poll(PadState*,uint16_t*) { ++native_reads; return 0; }
uint32_t hidpad_poll(PadState*,uint32_t*) { ++native_reads; return 0; }
bool relay_owns_hid() { return true; }
uint32_t swpro_apply_bindings(int,uint16_t b,PadState& p) { return button_apply_bindings(p,0,0,[&](int i){ return (b & default_swpro_bindings()[0].mask[i])!=0; },[](int){return 0;}); }
uint32_t hid_apply_bindings(int,uint32_t,PadState&,box::State&) { return 0; }
struct UiSnapshot {
 PadState* out; bool console=false,gamecube=false,keyboard_ports[4]{}; PadState keyboard_navigation{};
};
''' + '\n'.join('bool relay_ps(int index,PadState& pad,uint16_t& buttons) {\n'+body(window,'relay_ps')+'\n}' for _ in [0]) + r'''
uint32_t relay_switch_poll(PadState out[4],uint16_t buttons[4]) {
''' + body(window,'relay_switch_poll') + r'''
}
uint32_t relay_hid_poll(PadState out[4],uint32_t buttons[4]) {
''' + body(window,'relay_hid_poll') + r'''
}
void read_devices(PadState out[4],InputDebugSnapshot& debug,bool side_effects,PadState* keyboard_navigation=nullptr,bool console=false) {
''' + body(window,'read_devices') + r'''
}
void input_read_console_devices(PadState out[4]) {
''' + body(window,'input_read_console_devices') + r'''
}
}
yougame::relay_pads::Frame relay;
host::controller_relay::Snapshot snapshot() { return yougame::relay_pads::controller_snapshot(relay); }
int main() {
 using namespace host;
 controller_relay::install(snapshot);
 g_keys[VK_RETURN]=g_keys[VK_RIGHT]=g_keys['Z']=true;
 // Empty relay stays latched; default P1 must read the window keyboard.
 PadState pads[4]; input_read_console_devices(pads);
 CHECK(pads[0].err==0 && pads[0].button==0x1100 && pads[0].stick_x==127);
 CHECK(g_port_feeding[0].kind==DeviceKind::Keyboard);
 for(int i=1;i<4;++i) CHECK(pads[i].err==-1);
 // Reproduce the app's idle keyboard stand-in, which used to win as an Xbox pad.
 auto& v=relay.pads[0];v.present=true;v.kind="other";v.id="virtual";v.family_index=0;
 input_read_console_devices(pads);
 CHECK(pads[0].err==0 && pads[0].button==0x1100 && pads[0].stick_x==127);
 CHECK(g_port_feeding[0].kind==DeviceKind::Keyboard);
 controller_relay::Sample standin;CHECK(controller_relay::sample(CaptureDevice::XInputPad,0,standin) && standin.keyboard_standin);
 // App-only keyboard delivery also works when the window has no native key event.
 for(auto& key:g_keys) key=false;
 v.buttons=0x10;v.lx=32767;
 input_read_console_devices(pads);
 CHECK(pads[0].err==0 && pads[0].button==0x1000 && pads[0].stick_x==127);
 CHECK(g_port_feeding[0].kind==DeviceKind::XInputPad);
 v.buttons=0;v.lx=0;
 g_keys[VK_RETURN]=g_keys[VK_RIGHT]=g_keys['Z']=true;
 // Every real relay family retains controller priority, including an idle pad.
 for(const char* family:{"xbox","ps","switch","hid"}) {
   auto& real=relay.pads[2];real.present=true;real.kind=family;real.id="p3";real.family_index=1;
   input_read_console_devices(pads);
   CHECK(pads[0].err==0 && pads[0].button==0 && pads[0].stick_x==0);
   CHECK(g_port_feeding[0].kind!=DeviceKind::Keyboard);
   // A real controller explicitly on P2 suppresses keyboard P1 as before.
   g_port_sources[1]=g_port_feeding[0];
   input_read_console_devices(pads);
   CHECK(pads[0].err==-1 && pads[1].err==0);
   g_port_sources=default_port_sources();real={};
 }
 // Ordinary Online/menu read still has its existing page-pad routing.
 InputDebugSnapshot debug{};for(auto& p:pads){p={};p.err=-1;}
 read_devices(pads,debug,true);
 CHECK(debug.feeding[0].kind==DeviceKind::XInputPad && pads[0].button==0);
 relay={};focused=false;input_read_console_devices(pads);
 CHECK(pads[0].button==0 && pads[0].stick_x==0);
 CHECK(reads==13 && native_reads==0);
 CHECK(controller_relay::poll_serial==12 && !controller_relay::game_poll);
 std::printf("relay console keyboard: %d checks, %d device reads, default Enter/Z/arrows, all real families\n",checks,reads);
}
'''
        self.compile('console-relay-keyboard', harness)

    def test_x11_focus_churn_preserves_keys_but_real_blur_releases(self):
        helper = (ROOT/'port/linux/embed/yougame_embed_x11.c').read_text()
        sdl = (ROOT/'port/linux/window_sdl.inl').read_text()
        start = sdl.index('  if (g_x11_embed && e.type == SDL_WINDOWEVENT &&')
        guard = sdl[start:sdl.index('  if (g_x11_embed && (e.type == SDL_KEYDOWN',start)]
        harness = r'''
#include <cstdio>
#include <cstdlib>
using Window=unsigned long;
struct Display {} display;
enum { MapNotify=1,UnmapNotify,FocusIn,FocusOut,DestroyNotify,ConfigureNotify,
       RevertToParent=10,CurrentTime=0,YG_EMBED_OK=0,YG_EMBED_IGNORED=1,YG_EMBED_QUIT=2,
       SDL_WINDOWEVENT=20,SDL_WINDOWEVENT_FOCUS_LOST=21,SDL_WINDOWEVENT_FOCUS_GAINED=22 };
struct yg_embed_x11 {
 Display* dpy=&display;Window game=100,parent=50;int focus=1,mapped=1,viewable=1,dead=0,w=640,h=480;
} embed;
struct XEvent {
 int type;struct {Display* display;Window window;int send_event;} xany;
 struct {Window window;} xdestroywindow;
 struct {Window window;int width,height;} xconfigure;
};
struct SDL_Event {int type;struct {int event;} window;};
static int checks,sets,raises,clears;
static Window current=100;
static bool held=true,local_focus=true;
static yg_embed_x11* g_x11_embed=&embed;
static struct {int gone;} trap;
#define CHECK(x) do { ++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
int XGetInputFocus(Display*,Window* window,int*) {*window=current;return 1;}
void XSetInputFocus(Display*,Window window,int,int) {++sets;current=window;}
void XRaiseWindow(Display*,Window) {++raises;}
void begin(Display*) {} void end(Display*) {} int refresh(yg_embed_x11*) {return 1;}
void focus(yg_embed_x11* e,int force) {
''' + body(helper,'focus') + r'''
}
int yg_embed_x11_focused(const yg_embed_x11* e) {
''' + body(helper,'yg_embed_x11_focused') + r'''
}
int event(yg_embed_x11* e,const XEvent* ev) {
''' + body(helper,'yg_embed_x11_event') + r'''
}
void sdl_event(SDL_Event e) {
''' + guard + r'''
 if(e.window.event==SDL_WINDOWEVENT_FOCUS_LOST) {++clears;held=false;local_focus=false;}
 else local_focus=true;
}
int main() {
 XEvent e{};e.type=FocusIn;e.xany={&display,50,0};
 for(int n=0;n<100;++n) {
   CHECK(event(&embed,&e)==YG_EMBED_OK);
   sdl_event({SDL_WINDOWEVENT,{SDL_WINDOWEVENT_FOCUS_LOST}});
   CHECK(held && local_focus && sets==0 && raises==0 && clears==0);
 }
 // A parent taking focus transfers it once; its notifications must not restack.
 current=50;CHECK(event(&embed,&e)==YG_EMBED_OK);CHECK(sets==1 && raises==0 && current==100);
 // Another application's focus is never taken back. A real loss releases keys.
 current=999;e.type=FocusOut;CHECK(event(&embed,&e)==YG_EMBED_OK);
 CHECK(sets==1 && raises==0 && !yg_embed_x11_focused(&embed));
 sdl_event({SDL_WINDOWEVENT,{SDL_WINDOWEVENT_FOCUS_LOST}});
 CHECK(!held && !local_focus && clears==1);
 sdl_event({SDL_WINDOWEVENT,{SDL_WINDOWEVENT_FOCUS_GAINED}});CHECK(!local_focus);
 current=100;sdl_event({SDL_WINDOWEVENT,{SDL_WINDOWEVENT_FOCUS_GAINED}});CHECK(local_focus);
 embed.focus=0;CHECK(!yg_embed_x11_focused(&embed));
 embed.focus=1;embed.mapped=0;CHECK(!yg_embed_x11_focused(&embed));embed.mapped=1;
 e.type=MapNotify;CHECK(event(&embed,&e)==YG_EMBED_OK);CHECK(raises==1 && sets==1);
 std::printf("X11 focus: %d checks, 100 stale loss notifications retain keys, real blur releases\n",checks);
}
'''
        self.compile('console-x11-focus', harness)
        self.assertLess(sdl.index('yg_embed_x11_focused(g_x11_embed)'),sdl.index('if (g_on_message &&'))
        self.assertIn('int yg_embed_x11_focused(const yg_embed_x11 *e);',
                      (ROOT/'port/linux/embed/yougame_embed_x11.h').read_text())

    def test_first_scene_is_vs_for_every_seat_and_both_game_variants(self):
        for path in ('mu_replay.c', 'mu_p2p_entry.c'):
            source = (ROOT / 'sourceport/game/shim' / path).read_text()
            harness = r'''
#include <cstdio>
#include <cstdlib>
using u8=unsigned char;
enum { GM_VS=2,GM_DEBUG_VS=6 };
int console, replay, test, queries;
int mu_console_active() { ++queries;return console; }
int mu_replay_on() { return replay; } int mu_online_is_test_run() { return test; }
void boot(u8* mode) {
''' + body(source, 'mu_replay_boot_mode') + r'''
}
int main() {
 int checks=0;
 for(int players:{2,3,4}) for(int seat=1;seat<=players;++seat) {
   console=1;replay=test=1;u8 mode=0x28;boot(&mode);
   if(mode!=GM_VS) std::abort();++checks;
 }
 console=0;replay=test=0;u8 mode=0x28;boot(&mode);if(mode!=0x28) std::abort();++checks;
 test=1;boot(&mode);if(mode!=GM_DEBUG_VS) std::abort();++checks;
 std::printf("console boot: %d scene checks, %d frozen-state queries\n",checks,queries);
}
'''
            self.compile('console-boot-'+path, '#include <initializer_list>\n'+harness)
        patch = (ROOT / 'sourceport/patches/melee-native.patch').read_text()
        scene = patch[patch.index('void gm_801A4510(void)'):]
        self.assertIn('mu_replay_boot_mode(&state_machine.routing.curr_mode);', scene)
        self.assertIn('hardcoded 2 (::GM_VS)', patch)
        seed = body((ROOT/'sourceport/game/shim/mu_pad.c').read_text(),'mu_console_active')
        self.assertIn('mu_host->console_seed', seed)
        begin = body((ROOT/'port/app/source_host.cpp').read_text(),'h_console_seed')
        self.assertNotIn('my_port', begin)
        self.assertIn('console_active()', begin)

    def test_guest_waiting_panel_opens_during_safe_scene_gap(self):
        self.compile('console-guest-wait', r'''
#include "yougame_console_entry.h"
#include <cstdio>
#include <cstdlib>
int main() {
 using namespace yougame;int checks=0;
 auto check=[&](bool good){++checks;if(!good)std::abort();};
 check(console::waiting_status().empty());
 friends::events.menu_scene(false); // accepted mid-match: a toast only, Z (grab) keeps the invite
 check(friends::events.push({{"type","join"},{"room","abcdef"}},1));
 check(console::waiting_status().empty());
 check(!console::entry.poll(0,true,0x10));
 friends::events.menu_scene(true);
 check(console::waiting_status()==console::kGuestWait);
 check(friends::events.take_join(false).empty()); // scene not safe yet
 check(console::waiting_status()==console::kGuestWait);
 check(console::entry.poll(0,true,0x10)); // Z cancels before safe handoff
 check(console::waiting_status().empty());
 check(friends::events.push({{"type","join"},{"room","abcdef"}},2));
 check(friends::events.take_join(true)=="abcdef");
 check(console::entry.begin());
 console::entry.status(console::kGuestWait);
 check(console::waiting_status()==console::kGuestWait);
 console::entry.status("Starting together...");check(console::waiting_status()=="Starting together...");
 console::entry.status("Couldn't start playing together");check(console::waiting_status()=="Couldn't start playing together");
 console::entry.stop();console::entry.finished();check(console::waiting_status().empty());
 std::printf("guest waiting: %d checks, join -> settling -> waiting -> start/failure/cancel\n",checks);
}
''')
        ui = (ROOT/'port/runtime/gx/pc_settings.cpp').read_text()
        status = ui.index('const auto console_status = yougame::console::waiting_status();')
        panel = ui.index('ImGui::Begin("##yougame_friend_waiting"',status)
        self.assertLess(status,panel)
        self.assertIn('!console_status.empty()',ui[status:panel])
        self.assertIn('yougame::slippi_link_status(status)',ui[panel:panel+200])
        practice = (ROOT/'port/runtime/hle/native_practice.cpp').read_text()
        self.assertIn('yougame::friends::events.menu_scene(yougame::friends::menu_safe(major, minor));', practice)


if __name__ == '__main__':
    unittest.main()
