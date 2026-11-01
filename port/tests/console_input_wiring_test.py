"""Execute the production console read, UI publisher and hardware boundary with fake devices."""
from pathlib import Path
import unittest
import online_menu_test as menu
from yougame_console_wiring_test import body

ROOT = Path(__file__).resolve().parents[2]


class ConsoleInputTest(unittest.TestCase):
    def test_console_samples_publish_once_before_local_capture(self):
        window = (ROOT / 'port/runtime/host/window.cpp').read_text()
        source = (ROOT / 'port/app/source_host.cpp').read_text()
        snapshot = window[window.index('struct UiSnapshot {'):window.index('\nvoid input_poll(')]
        harness = r'''
#include <array>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "console_input.h"
#include "settings_chord.h"
#ifdef YOUGAME_AGENT_TEST_HOOK
#include "yougame_agent_pad.h"
#include "yougame_agent_fight.h"
namespace slippi::online { int session_mode() { return -1; } }
namespace yougame::agent_test {
InputState live_input_state(int mode) { return {0,mode}; }
struct TracePad { int buttons,x,y,cx,cy,left,right,error; std::string source; };
struct Trace { void poll(int,const TracePad&) {} } pad_trace;
}
#endif
static int checks, reads, publishes, overlay_polls, entry_polls, friends_polls;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
namespace yougame {
bool active() { return true; } bool stdio_bridge() { return true; }
namespace parity { struct Signals { void emit(int) {} } signals; }
namespace console {
bool shared_active=true, consume_friends=false;
bool entry_connected[4]{};
bool console_active() { return shared_active; }
struct Config { int my_port=2; };
struct Engine { const Config& config() const { static Config c; return c; } };
Engine& console_engine() { static Engine e; return e; }
struct Entry { bool poll(int port,bool connected,uint16_t& buttons) {
 ++entry_polls;entry_connected[port]=connected;
 return consume_friends && connected && (buttons & (0x10|0x200));
} } entry; }
namespace friends { struct Events { void poll_pad(int,bool,uint16_t&) { ++friends_polls; } } events; }
}
namespace host {
namespace controller_relay { struct GamePoll {}; }
struct PadState { uint16_t button; int8_t stick_x,stick_y,sub_x,sub_y; uint8_t trig_l,trig_r,analog_a,analog_b; int8_t err; };
enum class DeviceKind { None, Keyboard, GCAdapter, HidPad };
#ifdef YOUGAME_AGENT_TEST_HOOK
int g_ui_agent_step=-1;
bool input_script_active() { return false; }
void input_console_script(PadState*,int port) { CHECK(port==2); }
void log(const char*,...) {}
bool agent_input_allowed(yougame::agent_test::InputKey) { return true; }
bool relay_owns_hid() { return false; }
namespace controller_relay {
struct Device { int family,index; };
struct Snapshot { std::array<Device,4> devices; };
std::atomic<Snapshot(*)()> reader{nullptr};
int family_of(DeviceKind) { return 0; }
}
#endif
struct PortSource { DeviceKind kind=DeviceKind::None; int index=0; };
struct InputDebugSnapshot { unsigned gc_mask=0; PortSource feeding[4]; };
std::mutex g_ui_pad_mutex, g_devices_mutex, g_last_pads_mutex;
PadState g_ui_pad{},g_ui_pads[4]{},g_ui_navigation_pads[4]{},g_last_pads[4]{};
bool g_ui_has_pad=false,g_ui_gamecube=false,g_ui_settings_chord_held[4]{},g_ui_practice_chord_held[4]{};
std::atomic<bool> g_ui_capture{false},g_settings_toggle{false},g_practice_toggle{false};
std::atomic<int> g_settings_controller_port{-1};
void* g_hwnd=(void*)1; bool window_focused(void*) { return true; }
double now_seconds() { return 1; }
struct Overlay {
 int tick(uint64_t) { ++publishes; return 0; }
 int poll(bool,int,bool,uint16_t&,uint64_t,bool) { ++overlay_polls; return 0; }
} g_overlay_pad;
PadState hardware[4]{}, keyboard_navigation{};
PortSource feeding[4]{};
void read_devices(PadState out[4],InputDebugSnapshot& debug,bool side_effects,PadState* nav,bool console) {
 ++reads; CHECK(side_effects && console); CHECK(nav);
 *nav=keyboard_navigation;
 for(int i=0;i<4;++i) { out[i]=hardware[i]; debug.feeding[i]=feeding[i]; }
 debug.gc_mask=4;
}
''' + snapshot + '\nvoid input_read_console_devices(PadState out[4]) {\n' + body(window, 'input_read_console_devices') + r'''
}
bool window_input_captured() {
''' + body(window, 'window_input_captured') + r'''
}
}
struct ConsolePad { uint16_t button; int8_t stick_x,stick_y,sub_x,sub_y; uint8_t trigger_l,trigger_r,analog_a,analog_b; int8_t err; };
namespace yougame::console { using Pads=std::array<ConsolePad,4>; }
int g_console_host_pad = -1;
yougame::console::Pads console_hardware() {
''' + body(source, 'console_hardware') + r'''
}
static bool neutral(const ConsolePad& p) {
 return !(p.button || p.stick_x || p.stick_y || p.sub_x || p.sub_y || p.trigger_l || p.trigger_r || p.analog_a || p.analog_b);
}
static auto frame() {
 int r=reads,p=publishes,o=overlay_polls;
 auto pads=console_hardware();
 CHECK(reads==r+1); CHECK(publishes==p+1); CHECK(overlay_polls==o+4);
 CHECK(entry_polls==0 && friends_polls==0);
 return pads;
}
int main() {
 using namespace host;
 for(int i=0;i<4;++i) {
   hardware[i]={0x100,60,-50,40,-30,255,200,150,100,(int8_t)(i==3?-3:0)};
   feeding[i]={DeviceKind::GCAdapter,i};
 }
 auto live=frame();
 for(int i=0;i<4;++i) {
   CHECK(live[i].button==0x100 && live[i].stick_x==60 && live[i].trigger_l==255);
   CHECK(live[i].err==hardware[i].err);
   CHECK(live[i].stick_y==-50 && live[i].sub_x==40 && live[i].sub_y==-30);
   CHECK(live[i].trigger_r==200 && live[i].analog_a==150 && live[i].analog_b==100);
 }
 g_ui_capture=true;
 auto captured=frame();
 CHECK(g_ui_has_pad && g_ui_gamecube && g_ui_pad.button==0x100);
 for(int i=0;i<4;++i) {
   CHECK(neutral(captured[i]) && captured[i].err==hardware[i].err);
   CHECK(g_ui_pads[i].button==0x100 && g_ui_navigation_pads[i].stick_x==60);
 }
 // Closing A stays local-neutral, including a newly held button on the closing sample.
 hardware[1].button=0x200;
 g_ui_capture=false;
 auto closed=frame(); for(int i=0;i<3;++i) CHECK(neutral(closed[i]));
 closed=frame(); for(int i=0;i<3;++i) CHECK(neutral(closed[i]));
 hardware[0].button=0;
 auto released=frame(); CHECK(released[0].stick_x==60 && released[0].trigger_l==255);
 CHECK(neutral(released[1]) && neutral(released[2]));
 hardware[0].button=0x100;
 CHECK(frame()[0].button==0x100); // a new press after release arrives normally
 for(auto& p:hardware) p={};
 frame();
 // Use the shipped settings chord: Z + Start + DDown (Z+Start alone is intentionally insufficient).
 hardware[2].button=0x1010; frame(); CHECK(!g_settings_toggle.exchange(false));
 hardware[2].button=0x1014; auto chord=frame();
 CHECK(g_settings_toggle.exchange(false) && g_settings_controller_port==2);
 CHECK(chord[2].button==0 && g_ui_pads[2].button==0 && g_ui_navigation_pads[2].button==0);
 frame(); CHECK(!g_settings_toggle.exchange(false));
 hardware[2].button=0; frame();
 hardware[2].button=0x1018; frame(); CHECK(g_practice_toggle.exchange(false));
 // Keep the existing controller-over-keyboard fallback, then test keyboard-only navigation.
 for(auto& p:hardware) { p={}; p.err=-1; }
 hardware[0].err=0; hardware[0].button=0x1000; hardware[0].stick_x=80;
 feeding[0]={DeviceKind::Keyboard,0}; keyboard_navigation={};
 auto keyboard=frame(); CHECK(keyboard[0].button==0x1000 && g_ui_pads[0].stick_x==80);
 CHECK(g_ui_navigation_pads[0].button==0 && g_ui_navigation_pads[0].stick_x==0);
 hardware[2].err=0; frame(); CHECK(g_ui_pads[0].err==-1 && g_ui_navigation_pads[0].err==-1);
 // A disconnect clears the held latch, so reconnecting does not leave that port blocked.
 hardware[2].button=0x100; g_ui_capture=true; frame(); g_ui_capture=false; frame();
 hardware[2].err=-1; CHECK(frame()[2].err==-1);
 hardware[2].err=0; CHECK(frame()[2].button==0x100);
#ifdef YOUGAME_AGENT_TEST_HOOK
 // The real UI-step script is shared with console reads, before chords and local capture.
 using namespace yougame::agent_test;
 pad_back.arm(10,PadBack::Clock::now()-std::chrono::seconds(11));
 g_ui_capture=true;
 frame(); CHECK(g_ui_agent_step==0 && g_ui_pads[0].button==0);
 CHECK(pad_back.consume_ui(g_ui_agent_step));
 frame(); CHECK(pad_back.consume_ui(g_ui_agent_step));
 auto scripted=frame();
 CHECK(g_ui_agent_step==1 && g_settings_toggle.exchange(false));
 CHECK(neutral(scripted[0]) && g_ui_pads[0].button==0);
 pad_back.arm(0,PadBack::Clock::now());
#endif
 // Friends carries the host's physical CSS pad into the shared virtual port.
 for(auto& p:hardware)p={};g_ui_capture=false;frame();
 g_console_host_pad=2;hardware[2].button=0x100;
 auto selected=frame();CHECK(selected[2].button==0x100 && selected[2].err==0);
 for(int port:{0,1,3})CHECK(selected[port].err==-1);
 CHECK(g_ui_pads[1].err==0); // UI still sees the full device poll before selection
 g_console_host_pad=-1;
 // Offline still uses the same publisher with capture suppression and entry/Friends polling.
 PadState offline[4]{};
 g_ui_capture=true;
 { UiSnapshot ui{offline}; offline[0].button=0x100; offline[3].err=-3; }
 CHECK(offline[0].button==0 && offline[3].err==-3 && g_ui_pads[0].button==0x100);
 g_ui_capture=false;
 { UiSnapshot ui{offline}; }
 CHECK(entry_polls==4 && friends_polls==4);
 // A stopped shared session reuses this hardware boundary in local CSS: restore Z/B.
 yougame::console::shared_active=false; yougame::console::consume_friends=true;
 for(auto& p:hardware)p={}; feeding[0]={DeviceKind::GCAdapter,0}; hardware[0].button=0x10;
 auto invite=console_hardware();CHECK(invite[0].button==0 && yougame::console::entry_connected[0]);
 hardware[0].button=0x200;auto leave=console_hardware();CHECK(leave[0].button==0);
 g_ui_capture=true;console_hardware();CHECK(!yougame::console::entry_connected[0]);
 std::printf("console input: %d checks, %d frames/reads, real UI/chords before capture, independent release latches, offline behavior\n",checks,reads);
}
'''
        for defines in ((), ('-DYOUGAME_AGENT_TEST_HOOK',)):
            with self.subTest(hook=bool(defines)):
                menu.OnlineMenuTest().compile_fixture('console-input', harness,
                                                     '-I', str(ROOT / 'port/runtime/host'), *defines)

    def test_shared_publisher_and_platform_getter_wiring(self):
        window = (ROOT / 'port/runtime/host/window.cpp').read_text()
        source = (ROOT / 'port/app/source_host.cpp').read_text()
        for name in ('input_poll', 'input_read_console_devices'):
            self.assertIn('UiSnapshot ui{out};', body(window, name))
        snapshot = window[window.index('struct UiSnapshot {'):window.index('\nvoid input_poll(')]
        self.assertIn('pad_back.sample_for_ui', snapshot)
        self.assertIn('g_ui_agent_step = scripted.step;', snapshot)
        self.assertIn('apply_fight(pads', snapshot)
        # One shared getter follows the entire Win32/SDL (including Apple) window block.
        self.assertEqual(window.count('bool window_input_captured()'), 1)
        self.assertGreater(window.index('bool window_input_captured()'), window.index('#include "../../linux/window_sdl.inl"'))
        self.assertIn('window_input_captured()', (ROOT / 'port/runtime/host/window.h').read_text())
        begin = body(source, 'h_console_frame_begin')
        # One read per forward body, sampled or not; only a needed sample reaches the engine.
        self.assertEqual(begin.count('console_hardware()'), 1)
        self.assertIn('engine.advance(sample ? hardware : yougame::console::Pads{}, state)', begin)
        read = body(source, 'h_pad_read').split('if (g_online_test_mode == 0)')[0]
        self.assertIn('engine.active() ? yougame::console::Pads{} : console_hardware()', read)
        self.assertNotIn('input_poll(', read)
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        self.assertLess(cmake.index('add_test(NAME port_console_input_wiring'), cmake.index('  return()'))


if __name__ == '__main__':
    unittest.main()
