"""Execute the SDL pump/loader/teardown bodies with counted SDL and native adapter stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from online_menu_test import compiler_command
from yougame_console_wiring_test import body

ROOT = Path(__file__).resolve().parents[2]
SDL = (ROOT / 'port/linux/window_sdl.inl').read_text()

HARNESS = r'''
#include "sdl_video_thread.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <string>
#include <atomic>
static int checks, native_calls, polls, commands, events, detaches, mac_pumps;
static std::atomic<int> logs{0};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
struct SDL_Window {} window;
struct SDL_Event { int type; };
enum { SDL_WINDOW_INPUT_FOCUS=1, SDL_WINDOW_SHOWN=2 };
int pending_event;
int SDL_PollEvent(SDL_Event* e) { ++native_calls; ++polls; if (!pending_event) return 0; e->type=pending_event; pending_event=0; return 1; }
unsigned SDL_GetWindowFlags(SDL_Window*) { ++native_calls; return SDL_WINDOW_INPUT_FOCUS|SDL_WINDOW_SHOWN; }
void SDL_SetWindowTitle(SDL_Window*,const char*) { ++native_calls; }
const char* SDL_GetWindowTitle(SDL_Window*) { ++native_calls; return "game"; }
void SDL_DestroyWindow(SDL_Window*) { ++native_calls; }
namespace linux_compat { void set_game_window(void*) { ++native_calls; } }
namespace yougame {
bool stdio_bridge() { ++native_calls; return false; }
bool embedded() { ++native_calls; return true; }
namespace shortcuts { struct Commands { bool take_settings() { ++native_calls; return false; } } commands; }
namespace parity { struct Signals { void emit(int) { ++native_calls; } } signals; }
void relay_key(const char*) { ++native_calls; }
namespace agent_test {
struct KeyScript {
  using Clock=std::chrono::steady_clock;
  template<class... Args> void pump(Args&&...) { ++native_calls; }
} key_script;
}
}
namespace mac_embed {
bool pump() { ++native_calls; ++mac_pumps; return false; }
void destroy() { ++native_calls; }
}
namespace host {
SdlVideoThread g_sdl_video_thread;
void log(const char*, const char* message) { if (std::strcmp(message,"window: pump skipped on a non-video thread")) std::abort(); ++logs; }
bool video_thread(SdlVideoThread::Call call) {
''' + body(SDL, 'video_thread') + r'''
}
static std::atomic<bool> g_exit{false};
static std::atomic<int> g_exit_code{0};
bool exit_requested(); void request_exit(int);
void request_user_exit() { request_exit(0); }
void* g_hwnd=&window;
bool g_closed=false, g_deferred_fullscreen_show=false, g_escape_left_fullscreen=false, g_fullscreen=false;
int g_x11_embed_storage;
int* g_x11_embed=&g_x11_embed_storage;
std::atomic<bool> g_x11_embed_active{true};
bool g_embed_focus_requested=false;
struct Overlay { int cancel() { ++native_calls; return 0; } } g_overlay_key;
std::atomic<bool> g_escape_press{false}, g_ui_capture{false};
std::atomic<uint64_t> g_pending_client_size{0};
bool g_pointer_rested=true; uint64_t g_pointer_moved=0, kPointerRestMs=3000;
uint64_t GetTickCount64() { ++native_calls; return 0; }
SDL_Window* sdl_window() { return static_cast<SDL_Window*>(g_hwnd); }
void apply_client_size(int,int) { ++native_calls; }
void handle_event(const SDL_Event& e) { ++native_calls; ++events; if (e.type==1) { g_closed=true; request_user_exit(); } }
void pump_embed_commands() { ++native_calls; ++commands; }
void update_embed_size() { ++native_calls; }
void show_cursor(bool) { ++native_calls; }
void hidraw_stop() { ++native_calls; }
void yg_embed_x11_detach(int*) { ++native_calls; ++detaches; }
bool window_embedded() { ++native_calls; return true; }
std::wstring window_title_base() { return L"game"; }
std::string utf8_title(const wchar_t*) { return "title"; }
std::wstring g_title_before_loading; bool g_loading_shown=false;
namespace cursor_idle { bool visible(uint64_t,uint64_t,bool,bool) { ++native_calls; return false; } }
'''

TAIL = r'''
} // host
int main() {
 using namespace host;
 g_sdl_video_thread.claim();
 std::atomic<bool> waiting{false}, saw_exit{false};
 // All entry points must return before even touching non-atomic window/adapter state.
 std::thread waiter([&] {
   for (int i=0;i<100;++i) {
     window_pump(); loading_show(L"shaders",0,1); loading_close();
     window_set_title(L"worker"); window_destroy();
   }
   waiting=true;
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
   while (!exit_requested() && std::chrono::steady_clock::now()<deadline) {
     window_pump(); std::this_thread::yield();
   }
   saw_exit=exit_requested();
 });
 while (!waiting) std::this_thread::yield();
 CHECK(native_calls==0); CHECK(logs==5); CHECK(g_hwnd==&window);
 window_pump(); CHECK(polls==1); CHECK(events==0);
#ifdef MELEE_LINUX_X11_EMBED
 CHECK(commands==1);
#endif
#ifdef __APPLE__
 CHECK(mac_pumps==1);
#endif
 loading_show(L"shaders",0,1); CHECK(polls>=2);
 loading_close(); CHECK(!g_loading_shown);
 // The actual host.cpp atomic exit flag reaches the non-owner frame-0-style waiter.
 pending_event=1; window_pump(); waiter.join();
 CHECK(saw_exit); CHECK(exit_requested()); CHECK(events==1); CHECK(logs==5);
 window_destroy(); CHECK(g_hwnd==nullptr);
#ifdef MELEE_LINUX_X11_EMBED
 CHECK(detaches==1); CHECK(!g_x11_embed_active);
#endif
 const int before=native_calls; window_pump(); CHECK(native_calls==before);
 std::printf("production SDL pump: %d checks\n",checks);
}
'''


class SdlVideoThreadWiring(unittest.TestCase):
    def test_entry_guards_precede_all_state_and_platform_work(self):
        for name, kind in (('window_pump', 'Pump'), ('pump_embed_commands', 'EmbedCommands'),
                           ('handle_event', 'Event'), ('loading_show', 'LoadingShow'),
                           ('loading_close', 'LoadingClose'), ('window_destroy', 'Destroy'),
                           ('window_set_title', 'Title')):
            code = body(SDL, name)
            # Comments may precede the guard; every executable statement must follow it.
            import re
            code = re.sub(r'//[^\n]*', '', code).strip()
            self.assertTrue(code.startswith(f'if (!video_thread(SdlVideoThread::Call::{kind})) return;'), name)
        create = body(SDL, 'window_create')
        self.assertLess(create.index('g_sdl_video_thread.claim()'), create.index('SDL_InitSubSystem'))
        self.assertLess(create.index('g_sdl_video_thread.claim()'), create.index('mac_embed::configure'))
        destroy = body(SDL, 'window_destroy')
        self.assertGreater(destroy.index('g_sdl_video_thread.release()'), destroy.index('mac_embed::destroy()'))
        self.assertIn('#ifndef _WIN32\n#include "sdl_video_thread.h"',
                      (ROOT / 'port/runtime/host/window.cpp').read_text())

    def test_production_pumps_in_all_sdl_platform_branches(self):
        host = (ROOT / 'port/runtime/host/host.cpp').read_text()
        functions = 'bool exit_requested() {\n' + body(host, 'exit_requested') + '\n}\n'
        functions += 'void request_exit(int code) {\n' + body(host, 'request_exit') + '\n}\n'
        for signature, name in (('void window_pump()', 'window_pump'),
                                ('void loading_show(const wchar_t* what, size_t done, size_t total)', 'loading_show'),
                                ('void loading_close()', 'loading_close'),
                                ('void window_set_title(const wchar_t* title)', 'window_set_title'),
                                ('void window_destroy()', 'window_destroy')):
            functions += signature + ' {\n' + body(SDL, name) + '\n}\n'
        # Select production branches after system headers: undefining Apple's compiler macro
        # on the command line would break the native libc++ headers rather than test Linux.
        for platform, defines in (('linux', '#undef __APPLE__\n'),
                                  ('x11', '#undef __APPLE__\n#define MELEE_LINUX_X11_EMBED\n'),
                                  ('mac', '#ifndef __APPLE__\n#define __APPLE__\n#endif\n')):
            for hook in (False, True):
                self.compile_pump(platform, defines, hook, functions)

    def compile_pump(self, platform, defines, hook, functions):
        with self.subTest(platform=platform, hook=hook), tempfile.TemporaryDirectory(prefix='sdl-video-') as directory:
            source = Path(directory) / 'test.cpp'
            exe = Path(directory) / 'test'
            source.write_text(HARNESS + defines + ('#define YOUGAME_AGENT_TEST_HOOK\n' if hook else '') + functions + TAIL)
            subprocess.run(compiler_command() + ['-std=c++17', '-pthread', '-I',
                           str(ROOT / 'port/runtime/host'), str(source), '-o', str(exe)],
                           check=True)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_waiting_phases_keep_owner_pumping_and_atomic_exit(self):
        main = (ROOT / 'port/app/main.cpp').read_text()
        worker = (ROOT / 'port/runtime/gx/threaded_backend.cpp').read_text()
        self.assertLess(main.index('backend = gx::create_threaded_backend('),
                        main.index('yougame::wait_identity(10000)'))
        self.assertIn('threaded = false;', main[main.index('#ifdef __APPLE__\n  // Embedded launch'):])
        self.assertLess(worker.index('host::window_pump();'), worker.index('queue.try_pop(incoming)'))
        self.assertIn('queue.wait_available(std::chrono::milliseconds(2));\n        continue;', worker)
        source = (ROOT / 'port/app/source_host.cpp').read_text()
        responsive = source[source.index('hooks.responsive ='):][:400]
        self.assertIn('host::window_pump();', responsive)
        self.assertIn('if (host::exit_requested())', responsive)
        engine = (ROOT / 'port/runtime/hle/yougame_console_engine.cpp').read_text()
        self.assertEqual(engine.count('if (hooks_.responsive) hooks_.responsive();'), 3)
        self.assertGreaterEqual(engine.count('if (!active_) return false;'), 3)
        host = (ROOT / 'port/runtime/host/host.cpp').read_text()
        self.assertIn('static std::atomic<bool> g_exit{false};', host)
        self.assertIn('g_exit.store(true)', body(host, 'request_exit'))


if __name__ == '__main__':
    unittest.main()
