"""Execute console boot, frozen host choice and shim with offline card/scene stubs."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from online_menu_test import compiler_command
from yougame_console_wiring_test import body

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text()


class ConsoleSaveBoot(unittest.TestCase):
    def fixture(self, variant, mutant=False):
        host = read('port/app/source_host.cpp')
        pad = read('sourceport/game/shim/mu_pad.c')
        boot = body(read('sourceport/game/shim/' + variant), 'mu_replay_boot_mode')
        if mutant:
            # Revert exactly the production boot fix, leaving the loader available.
            boot = boot.replace('        mu_console_load_save();\n', '')
        return r'''
#include "mu_host.h"
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>
using u8=unsigned char;
enum { GM_BOOT=0x28, GM_VS=2, GM_DEBUG_VS=6 };
enum lbCardResult { LbCardResult_Ready=0, LbCardResult_2=2, LbCardResult_NoFile=4 };
static bool g_console_boot, active;
namespace yougame::console {
struct Config { std::string card_digest; int my_port; } config;
struct Engine { const Config& config() const { return console::config; } } engine;
Engine& console_engine() { return engine; }
bool console_active() { return active; }
}
int32_t h_console_has_save() {
''' + body(host, 'h_console_has_save') + r'''
}
MuHostApi api{};
const MuHostApi* mu_host=&api;
int32_t seed(uint32_t* out) { *out=123;return active; }
int mu_console_active() {
''' + body(pad, 'mu_console_active') + r'''
}
int mu_console_has_save() {
''' + body(pad, 'mu_console_has_save') + r'''
}
static int replay, online_test, tourney, result;
static u8 mode;
static std::vector<std::string> events;
struct Save { unsigned unlocks, rules, names; bool operator==(const Save& b) const {
  return unlocks==b.unlocks && rules==b.rules && names==b.names;
} } fresh{1,2,0}, host_save{0x1ffffff,8,42}, state;
void check(bool ok) { if (!ok) throw std::runtime_error("boot contract failed"); }
static std::vector<std::string> logged;
void mu_console_log(const char* t) { logged.push_back(t); }
void lbCardNew_AllocWorkArea() {
  check(mode==GM_BOOT); events.push_back("allocate");
}
lbCardResult lb_8001CBBC() {
  check(mode==GM_BOOT); events.push_back("read");state=host_save;
  return static_cast<lbCardResult>(result);
}
void gmMainLib_8015FA34(int status) {
  check(mode==GM_BOOT && status==result && (state==host_save || (status!=0 && status!=2)));
  events.push_back("finalize");
}
void mu_console_load_save() {
''' + body(read('sourceport/game/shim/mu_console_boot.c'), 'mu_console_load_save') + r'''
}
int mu_replay_on() { return replay; }
int mu_online_is_test_run() { return online_test; }
int mu_tourney_boot() { return tourney; }
void boot(u8* mode) {
''' + boot + r'''
}
void reset() { events.clear();state=fresh;mode=GM_BOOT; }
int main() {
 try {
  api.version=MU_HOST_API_VERSION;api.console_seed=seed;
  api.console_has_save=h_console_has_save;
  int checks=0;
  for (int players : {2,3,4}) for (int seat=1;seat<=players;++seat)
  for (bool saved : {false,true}) for (int status : {0,2}) {
    active=g_console_boot=true; result=status;replay=online_test=tourney=1;
    yougame::console::config={saved?"agreed-sha256":"",seat};
    reset();boot(&mode);check(mode==GM_VS);
    // These are the earliest consumers after gm_801A4510's boot hook returns.
    events.push_back("VS");check(state==(saved?host_save:fresh));
    events.push_back("frame-0 digest");
    check(events==(saved?std::vector<std::string>{"allocate","read","finalize","VS","frame-0 digest"}:
                         std::vector<std::string>{"VS","frame-0 digest"}));
    ++checks;
  }
  // Ordinary, replay, online harness and tourney preserve their original routing,
  // even with a populated digest left in the host configuration.
  active=g_console_boot=false;yougame::console::config.card_digest="agreed-sha256";
  for (replay=0;replay<2;++replay) for (online_test=0;online_test<2;++online_test)
  for (tourney=0;tourney<2;++tourney) {
    reset();boot(&mode);
    check(mode==''' + ('(online_test?GM_DEBUG_VS:GM_BOOT)' if variant == 'mu_p2p_entry.c' else
                      '(replay||online_test?GM_DEBUG_VS:tourney?GM_VS:GM_BOOT)') + r''');
    check(events.empty() && state==fresh);++checks;
  }
  // The loader itself is also session gated. A stale digest cannot load a local save.
  reset();mu_console_load_save();check(events.empty());++checks;
  active=true;g_console_boot=false;reset();mu_console_load_save();check(events.empty());++checks;
  active=g_console_boot=true;
  // An unreadable agreed card finalizes like GS_MEMCARD (defaults), logs once and still boots VS.
  result=LbCardResult_NoFile;reset();logged.clear();boot(&mode);
  check(mode==GM_VS && events==std::vector<std::string>{"allocate","read","finalize"} &&
        logged.size()==1 && logged[0].find("could not be read")!=std::string::npos);++checks;
  // ABI guard: never dereference the new slot from an older table.
  api.version=27;reset();mu_console_load_save();check(events.empty());++checks;
  std::printf("save boot: %d contracts passed, all seats, default and non-console paths\n",checks);
  return 0;
 } catch (const std::runtime_error& e) { std::fprintf(stderr,"%s\n",e.what());return 1; }
}
'''

    def run_fixture(self, variant, mutant=False):
        with tempfile.TemporaryDirectory(prefix='console-save-') as directory:
            source, exe = Path(directory) / 'fixture.cpp', Path(directory) / 'fixture'
            source.write_text(self.fixture(variant, mutant))
            sdk = ['-isysroot', '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'] if sys.platform == 'darwin' else []
            subprocess.run([*compiler_command(), '-std=c++17', *sdk,
                            '-I', str(ROOT / 'port/runtime/abi'), str(source), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], capture_output=True, text=True)

    def test_production_boot_and_reverted_mutant(self):
        for variant in ('mu_p2p_entry.c', 'mu_replay.c'):
            with self.subTest(variant=variant):
                good = self.run_fixture(variant)
                self.assertEqual(good.returncode, 0, good.stderr)
                print(variant + ': ' + good.stdout.strip())
                mutant = self.run_fixture(variant, mutant=True)
                self.assertNotEqual(mutant.returncode, 0, 'reverted boot must fail')
                self.assertIn('boot contract failed', mutant.stderr)
                print(variant + ': reverted boot mutant rejected')

    def test_wiring_before_first_scene_and_both_builds(self):
        patch = read('sourceport/patches/melee-native.patch')
        boot = patch[patch.index('void gm_801A4510(void)'):]
        self.assertLess(boot.index('mu_replay_boot_mode('), boot.index('runGameMode('))
        cmake = read('sourceport/game/CMakeLists.txt')
        self.assertIn('shim/*.c', cmake)  # shared loader builds in both variants
        self.assertIn('set(MU_NO_SLIPPI_ONLY mu_noslippi.c mu_match_info.c mu_p2p_entry.c)', cmake)
        self.assertIn('mu_replay.c mu_replay_abi.c', cmake)
        self.assertIn('h.console_has_save = h_console_has_save;', read('port/app/source_host.cpp'))
        # gmmain initializes these before reaching the patched boot hook.
        self.assertNotIn('mu_console_load_save', body(read('sourceport/game/shim/mu_replay.c'), 'mu_replay_prepare_scene'))


if __name__ == '__main__':
    unittest.main()
