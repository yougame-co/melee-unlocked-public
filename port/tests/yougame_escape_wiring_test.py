"""Compile real embedded Escape routes and hook relays; no real-game flow certification."""
from pathlib import Path
import unittest
import online_menu_test as menu

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding='utf-8-sig')


STUBS = r'''
#include "yougame_parity.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
static int checks, scenarios;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
static bool bridge, native_embed;
static std::atomic<bool> g_capturing{false}, g_escape_press{false};
static std::mutex g_keys_mutex;
static bool g_keys[256];
static std::vector<std::string> relays;
namespace yougame {
bool stdio_bridge() { return bridge; }
void relay_key(const char* key) { relays.emplace_back(key); }
}
namespace mac_embed { bool active() { return native_embed; } }
static constexpr int VK_ESCAPE=27, VK_SHIFT=16;
static int modifiers;
static int GetKeyState(int vk) { CHECK(vk==VK_SHIFT); return modifiers ? 0x8000 : 0; }
static constexpr int WM_KEYDOWN=1, WM_KEYUP=2, WM_SYSKEYDOWN=3, WM_SYSKEYUP=4;
static constexpr int SDL_KEYDOWN=1, SDL_KEYUP=2, SDL_SCANCODE_ESCAPE=27;
static constexpr int KMOD_LSHIFT=1, KMOD_RSHIFT=2, KMOD_SHIFT=KMOD_LSHIFT|KMOD_RSHIFT;
struct SDL_Event { int type; struct { int repeat; struct { int scancode,mod; } keysym; } key; };
static void set_key(int vk, bool down) { g_keys[vk]=down; }
static int forwarded;
'''

CASES = r'''
int main() {
  for (bool stdio : {false,true}) for (bool native : {false,true})
    for (int shift : {0,KMOD_LSHIFT,KMOD_RSHIFT,KMOD_SHIFT})
      for (bool capture : {false,true}) for (bool repeat : {false,true}) for (bool down : {false,true}) {
        bridge=stdio; native_embed=native; modifiers=shift; g_capturing=capture;
        relays.clear(); g_escape_press=false; g_keys[VK_ESCAPE]=false; forwarded=0;
        event(down,repeat,27,false);
        const bool embedded=stdio || (MAC_ROUTE && native);
        CHECK(forwarded==int(!embedded));
        CHECK(g_escape_press.load()==(embedded && down && !repeat && !shift && !capture));
        CHECK(g_keys[VK_ESCAPE]==(embedded && down && !repeat && !shift && capture));
        const bool relay=embedded && down && !repeat && (shift || !capture);
        CHECK(relays.size()==size_t(relay));
        if (relay) CHECK(relays[0]==(shift ? "shift-escape" : "escape"));
        ++scenarios;
      }
  // Tab, Enter and F1 keep reaching their existing handlers, with either Shift.
  bridge=true;
  for (int key : {9,13,112}) for (int shift : {0,1,2}) {
    modifiers=shift; forwarded=0; relays.clear(); g_escape_press=false;
    event(true,false,key,false);
    CHECK(forwarded==1 && relays.empty() && !g_escape_press.load()); ++scenarios;
  }
#ifdef WINDOWS_ROUTE
  for (bool shift : {false,true}) {
    modifiers=shift; g_capturing=false; relays.clear(); g_escape_press=false;
    event(true,false,27,true); event(true,true,27,true); event(false,false,27,true);
    CHECK(relays==std::vector<std::string>{shift ? "shift-escape" : "escape"});
    CHECK(g_escape_press.load()==!shift); CHECK(!g_keys[VK_ESCAPE]); ++scenarios;
  }
#endif
  std::printf("embedded Escape route: %d scenarios, %d checks; first/repeat/up, either Shift, capture, standalone, Tab/Enter/F1\n",scenarios,checks);
}
'''


class EscapeWiringTest(unittest.TestCase):
    def test_platform_routes_compile_and_agree_before_imgui(self):
        win = source('port/runtime/host/window.cpp')
        sdl = source('port/linux/window_sdl.inl')
        start = win.index('  // One embedded Escape press')
        end = win.index('  // App shortcuts run', start)
        windows = win[start:end]
        start = sdl.index('  // Embedded Escape bypasses')
        end = sdl.index('  if (e.type == SDL_MOUSEMOTION)', start)
        platforms = [('windows', windows, True, False, False),
                     ('mac', sdl[start:end], False, True, False),
                     ('linux-x11', sdl[start:end], False, False, True),
                     ('linux-sdl', sdl[start:end], False, False, False)]
        self.assertLess(win.index(windows), win.index('if (g_on_message &&'))
        self.assertLess(sdl.index('yougame::parity::embedded_escape('), sdl.index('if (g_on_message &&'))
        self.assertLess(sdl.index('yougame::parity::embedded_escape('), sdl.index('// Escape must reach fullscreen handling'))
        self.assertIn('(e.key.keysym.mod & KMOD_SHIFT) != 0', sdl[sdl.index('g_overlay_key.tab'):])
        self.assertEqual(win.count('yougame::parity::embedded_escape('), 1)
        self.assertEqual(sdl.count('yougame::parity::embedded_escape('), 1)
        for name, block, windows_route, mac, x11 in platforms:
            with self.subTest(platform=name):
                # Isolate just the platform conditional under test from the compiler's own OS.
                block = block.replace('#ifdef __APPLE__', '#ifdef TEST_MAC_ROUTE')
                if windows_route:
                    route = 'static int route(int m, int w, long long l) {\n' + block + '\n++forwarded; return 0; }\n'
                    event = r'''
void event(bool down, bool repeat, int key, bool sys) {
  route(down ? (sys?WM_SYSKEYDOWN:WM_KEYDOWN) : (sys?WM_SYSKEYUP:WM_KEYUP),key,repeat?(1LL<<30):0);
}
'''
                else:
                    route = 'static void route(const SDL_Event& e) {\n' + block + '\n++forwarded; }\n'
                    event = r'''
void event(bool down, bool repeat, int key, bool) {
  SDL_Event e{}; e.type=down?SDL_KEYDOWN:SDL_KEYUP; e.key.repeat=repeat;
  e.key.keysym.scancode=key; e.key.keysym.mod=modifiers; route(e);
}
'''
                defines = ['-DMAC_ROUTE='+str(int(mac))]
                if windows_route:
                    defines.append('-DWINDOWS_ROUTE')
                if mac:
                    defines.append('-DTEST_MAC_ROUTE')
                if x11:
                    defines.append('-DMELEE_LINUX_X11_EMBED')
                menu.OnlineMenuTest().compile_fixture('escape-'+name, STUBS+route+event+CASES, *defines)

    def test_hint_ascii_and_ctest_on_every_os(self):
        parity = source('port/runtime/hle/yougame_parity.h')
        self.assertIn('kMenuHint = "Shift+Tab / YouGame menu (D-pad + Start)"', parity)
        self.assertNotIn('Escape / YouGame menu', parity)
        hint = parity.split('kMenuHint = "', 1)[1].split('"', 1)[0]
        self.assertTrue(hint.isascii())
        cmake = source('CMakeLists.txt')
        self.assertLess(cmake.index('add_test(NAME port_yougame_escape_wiring'), cmake.index('  return()'))
        nav = source('port/runtime/host/keyboard_ui_navigation.h')
        self.assertIn('case VK_SPACE: case VK_RETURN: case VK_ESCAPE: case VK_TAB:', nav)

    def test_existing_agent_key_pump_callbacks_compile_and_relay(self):
        for path in ('port/runtime/host/window.cpp', 'port/linux/window_sdl.inl'):
            text = source(path)
            start = text.index('  if (!g_closed) yougame::agent_test::key_script.pump(')
            block = text[start:text.index('#endif', start)]
            self.assertIn('[] { yougame::relay_key("escape"); }', block)
            for hook in (False, True):
                with self.subTest(path=path, hook=hook):
                    fixture = r'''
#include "yougame_agent_keys.h"
#include "yougame_parity.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
static std::vector<std::string> relays;
namespace yougame {
bool stdio_bridge() { return true; }
void relay_key(const char* key) { relays.emplace_back(key); }
}
bool g_closed=false;
yougame::parity::OverlayKey g_overlay_key;
void log(const char*, const char*) {}
void writer(const char*) {}
int main() {
#ifdef YOUGAME_AGENT_TEST_HOOK
  using namespace yougame::agent_test;
  std::string why;
  key_script.arm(true,parse_key_script("escape@0.01",why));
  if (!why.empty()) std::abort();
  yougame::parity::signals.start(true,writer);
  yougame::parity::signals.presented(true,KeyScript::Clock::now()-std::chrono::seconds(1));
''' + block + r'''
  if (relays!=std::vector<std::string>{"escape"}) std::abort();
#else
  if (!relays.empty()) std::abort();
#endif
  puts("production key-script callback: compiled; hook relay preserved/off absent");
}
'''
                    menu.OnlineMenuTest().compile_fixture('escape-script', fixture,
                        *(['-DYOUGAME_AGENT_TEST_HOOK'] if hook else []))


if __name__ == '__main__':
    unittest.main()
