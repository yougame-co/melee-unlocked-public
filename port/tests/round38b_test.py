"""Round 38b regression checks; execute the shared production helpers offline."""
from pathlib import Path
import importlib.util
import tempfile
import unittest
import online_menu_test as menu
from yougame_console_wiring_test import body

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text()


class Round38bTest(unittest.TestCase):
    def compile(self, name, text):
        menu.OnlineMenuTest().compile_fixture(name, text)

    def test_corner_and_menu_rectangles_at_extreme_aspects(self):
        source = read('port/runtime/gx/replay_bar.cpp')
        start = source.index('struct CornerHint')
        end = source.index('\n}\nvoid draw_corner_hint', start)
        helper = source[start:end]
        self.compile('38b-hint-rect', r'''
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include "yougame_parity.h"
struct ImVec2 { float x,y; ImVec2(float a=0,float b=0):x(a),y(b){} };
struct Font { ImVec2 CalcTextSizeA(float fs,float,float,const char* s) {
  float n=0;while(*s++)++n;return {n*fs*.6f,fs}; } } font;
namespace ImGui { Font* GetFont(){return &font;} }
namespace yougame::console { bool compact=true; bool console_menu_visible(){return compact;} }
''' + helper + r'''
int main() {
 int checks=0;
 for(bool compact:{false,true}) { yougame::console::compact=compact;
 for(float w:{64.f,200.f,450.f,640.f,1920.f,5120.f})
 for(float h:{64.f,200.f,480.f,1080.f,2160.f})
 for(float aspect:{.2f,73.f/60.f,4.f/3.f,16.f/9.f,5.f}) {
   const auto c=corner_hint(w,h,aspect);
   const float ph=std::min(h,w/aspect),top=(h-ph)*.5f;
   const float line_y=std::max(top+ph*.027f,c.max.y+2+ph/480);
   if(c.min.x<-.01f || c.max.x>w+.01f || line_y-2<c.max.y || c.font<=0)std::abort();
   checks+=4;
 }
 }
 printf("hint rectangles: %d checks passed\n",checks);
}
''')
        self.assertIn('corner.max.y+2+picture_height/480.0f', source)
        self.assertIn('screen_label::draw_corner_hint(', body(read('port/runtime/gx/pc_settings.cpp'),
                                                           'draw_back_to_yougame_hint'))

    def test_snapshot_sidecars_are_rebound_to_both_packaged_libraries(self):
        spec = importlib.util.spec_from_file_location('linux_package', ROOT / 'tools/package_linux_release.py')
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            for name, size, offset in [('melee_game.so', 101, '20'), ('melee_game_compat.so', 137, '40')]:
                lib = root / name
                lib.write_bytes(b'X' * size)
                source = root / ('source-' + name)
                source.write_text('# dll_size 99\n' + offset + ' 10\n')
                original = root / ("original-" + name)
                original.write_bytes(b"B" * 99)
                tool.package_snapexcl(source, lib, original)
                self.assertEqual(lib.with_suffix('.snapexcl').read_text(),
                                 f'# dll_size {size}\n{offset} 10\n')
                original.write_bytes(b"stale")
                with self.assertRaises(SystemExit):
                    tool.package_snapexcl(source, lib, original)
        package = read('tools/package_linux_release.py')
        self.assertIn("compat_snapexcl = args.compat_game_build_dir / 'melee_game.snapexcl'", package)
        self.assertIn('compat_game, compat_snapexcl)', package)
        self.assertLess(package.index("'--remove-section=.gnu_debuglink', folder / 'melee_game_compat.so'"),
                        package.index('package_snapexcl(compat_snapexcl'))
        # patchelf grows each library, so its dll_size is written after the library's last patchelf
        for name, source in (('melee_game.so', 'snapexcl'), ('melee_game_compat.so', 'compat_snapexcl')):
            self.assertLess(package.index(f"'--set-rpath', '$ORIGIN/lib', folder / '{name}'"),
                            package.index(f"package_snapexcl({source}, folder / '{name}'"))

    def test_snapshot_loader_executes_for_so_dll_and_compat(self):
        source = read('port/app/source_host.cpp')
        helper = source.index('void append_sound_driver_exclusions(std::vector<MuStateRegion>& exclusions) {')
        start = source.index('  if (!g_loaded_game_path.empty() && host::game_image)', helper)
        end = source.index('\n}\n', start)
        loader = source[start:end]
        self.compile('38b-snapexcl', r'''
#include <filesystem>
#include <chrono>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
struct MuStateRegion { void* address; uint32_t size; };
std::filesystem::path g_loaded_game_path;
namespace host { uint8_t* game_image=(uint8_t*)0x82800000; std::string logged;
void log(const char* f,...) {char b[512];va_list a;va_start(a,f);vsnprintf(b,sizeof b,f,a);va_end(a);logged=b;} }
std::vector<MuStateRegion> load() { std::vector<MuStateRegion> exclusions;
''' + loader + r'''
return exclusions; }
int main(){
 auto dir=std::filesystem::temp_directory_path()/("38b-snapexcl-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directories(dir);
 int checks=0;
 for(auto name:{"melee_game.so","melee_game.dll","melee_game_compat.so","melee_game_compat.dll"}) {
   g_loaded_game_path=dir/name; std::ofstream(g_loaded_game_path)<<"library";
   auto side=g_loaded_game_path;side.replace_extension(".snapexcl");
   std::filesystem::remove(side);
   if(!load().empty() || host::logged.find("file missing")==std::string::npos)std::abort();++checks;
   std::ofstream(side)<<"# dll_size 8\n20 10\n";
   if(!load().empty() || host::logged.find("size mismatch")==std::string::npos)std::abort();++checks;
   std::ofstream(side)<<"# dll_size 7\n20 10\n";
   auto spans=load();if(spans.size()!=1 || uintptr_t(spans[0].address)!=0x82800020 || spans[0].size!=16)std::abort();++checks;
   if(host::logged.find("sound driver state left out of snapshots")==std::string::npos)std::abort();++checks;
 }
 g_loaded_game_path.clear();if(!load().empty() || host::logged.find("library not found")==std::string::npos)std::abort();++checks;
 std::filesystem::remove_all(dir);printf("snapshot loader: %d checks passed\n",checks);
}
''')
        self.assertIn('dladdr(reinterpret_cast<void*>(entry)', source)
        self.assertNotIn('GetModuleHandleA', body(source, 'begin_native_savestates'))

    def test_loaded_library_path_compiles_in_every_platform_branch(self):
        from yougame_0962_merge_test import conditional_source
        source = read('port/app/source_host.cpp')
        start = source.index('#ifdef __APPLE__\n  g_loaded_game_path = g_dll;')
        end = source.index('  g_update_ranked_busy', start)
        for platform in ('mac', 'windows', 'linux'):
            defines = {'__APPLE__'} if platform == 'mac' else {'_WIN32'} if platform == 'windows' else set()
            code = conditional_source(source[start:end], defines)
            self.compile('38b-path-' + platform, r'''
#include <filesystem>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
constexpr int MAX_PATH=260;void* module=(void*)1;void* entry=(void*)2;
std::filesystem::path g_loaded_game_path;std::string g_dll="fallback/melee_game_compat.dll";
struct Dl_info { const char* dli_fname; };
int dladdr(void*,Dl_info* out){out->dli_fname="loaded/melee_game_compat.so";return 1;}
int GetModuleFileNameA(void*,char* out,int){std::strcpy(out,"loaded/melee_game_compat.dll");return 1;}
int main(){
''' + code + '\nif(g_loaded_game_path.empty())std::abort();puts("library path: ' + platform + ' branch compiled and executed");}\n')

    def test_script_p1_drives_only_this_seats_local_sample(self):
        window = read('port/runtime/host/window.cpp')
        self.compile('38b-script-local', r'''
#include <cstdio>
#include <cstdlib>
struct PadState { int button=0,err=-1; };
static bool g_console_script_profile=false,active=true;static int calls=0,logs=0;
bool input_script_active(){return active;}
unsigned retrace_count(){return 115;}
void script_pads(unsigned frame,bool update,PadState out[4]) {
 if(frame!=115 || !update)std::abort(); ++calls;out[0]={256,0};out[1]={512,0};
}
void log(const char*,int){++logs;}
void input_console_script(PadState out[4],int local_port) {
''' + body(window, 'input_console_script') + r'''
}
int main(){
 PadState pads[4]={{1,0},{2,0},{3,0},{4,0}};
 input_console_script(pads,2);if(calls || pads[0].button!=1)std::abort();
 g_console_script_profile=true;
 for(int seat=1;seat<=4;++seat){
  input_console_script(pads,seat);
  if(pads[0].button!=256 || pads[1].button!=2 || pads[2].button!=3 || pads[3].button!=4)std::abort();
 }
 active=false;input_console_script(pads,2);input_console_script(pads,0);
 if(calls!=4 || logs!=1)std::abort();
 puts("console script: 7 checks passed (all local seats; remote script entries ignored)");
}
''')
        self.assertIn('#ifdef YOUGAME_AGENT_TEST_HOOK\n// Console hardware slots', window)
        host = body(read('port/app/source_host.cpp'), 'console_hardware')
        self.assertIn('#ifdef YOUGAME_AGENT_TEST_HOOK\n  host::input_console_script', host)
        self.assertIn('g_console_script_profile = secured;', window)

    def test_retry_waits_for_worker_and_accepts_retail_menus(self):
        source = read('port/runtime/hle/native_practice.cpp')
        self.assertIn('safe && yougame::identity().ok && yougame::console::entry.available()', source)
        self.compile('38b-entry-retry', r'''
#include "yougame_console_entry.h"
#include <cstdlib>
#include <cstdio>
using namespace yougame::console;
int main(){
 Entry e; if(!e.begin())std::abort();
 e.stop();if(e.available() || e.begin())std::abort();
 e.finished();if(!e.available() || !e.begin())std::abort();
 e.finished();if(e.active() || !e.available())std::abort();
 yougame::friends::Events invites;
 invites.current_room("OldRoom");invites.current_room({});
 invites.menu_scene(yougame::friends::menu_safe(2,0));
 if(!invites.push({{"type","join"},{"room","NewRoom"}},1))std::abort();
 if(invites.push({{"type","join"},{"room","OldRoom"}},2))std::abort();
 if(!invites.join_pending() || invites.take_join(true)!="NewRoom")std::abort();
 puts("invite retry: 8 checks passed (failed worker, new room, stale push, retail CSS)");
}
''')


if __name__ == '__main__':
    unittest.main()
