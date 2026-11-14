"""Harness-only CLI admission, delay queue and read-only rollback evidence.

Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from squad_wiring_test import ROOT, function, read, without_squad


class SquadDelay(unittest.TestCase):
    def test_production_flag_branch_off_without_hook_and_with_hook(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler required')
        main = read(ROOT / 'port/app/main.cpp')
        branch = main[main.index('    else if (a == "--squad-test-net-delay")'):]
        branch = branch.split('\n#endif\n    else if (a == "--online-test")', 1)[0]
        gate = function(read(ROOT / 'port/app/source_host.cpp'), 'check_squad_test')
        fixture = r'''
#include <cassert>
#include <cstdio>
#include <string>
#include "yougame_net_test.h"
#include "squad_test_spec.h"
#if MU_SQUAD
MuSquadSide g_online_test_squad{};
bool g_online_test_squad_cancel = false;
'''+gate+r'''
#endif
int flag(const char* value, bool squad, bool local) {
  const std::string a = "--squad-test-net-delay";
  auto next = [&]() { return value; };
  (void)next; (void)squad; (void)local;
#if MU_SQUAD
  bool squad_test_net_delay = false;
#ifdef YOUGAME_AGENT_TEST_HOOK
  yougame::net_test::Config squad_net_config;
#endif
  if (false) {}
'''+branch+r'''
  else return 2;
  if (!check_squad_test(local, false, squad_test_net_delay)) return 2;
#ifdef YOUGAME_AGENT_TEST_HOOK
  assert(squad_net_config.squad_local_test && squad_net_config.delay_ms > 0);
#endif
  return 0;
#else
  return 2;
#endif
}
int main() {
#if MU_SQUAD
  g_online_test_squad.count = 1;
#endif
  for (const char* text : {"1", "250", "1:0", "250:100", "60:20"}) {
#if MU_SQUAD && defined(YOUGAME_AGENT_TEST_HOOK)
    assert(flag(text, true, true) == 0);
#else
    assert(flag(text, true, true) == 2);
#endif
  }
  for (const char* text : {"", "0", "251", "-1", "+1", "1:", ":1", "1:101", "1:-1", "1:2:3", "1x", " 1", "99999999999999999999"})
    assert(flag(text, true, true) == 2);
  assert(flag("60:20", true, false) == 2);
#if MU_SQUAD
  g_online_test_squad = {};
#endif
  assert(flag("60:20", false, true) == 2);
#if MU_SQUAD && defined(YOUGAME_AGENT_TEST_HOOK)
  using namespace yougame::net_test;
  Config c; assert(parse_squad_delay("60:20", &c));
  assert(mode_allowed(c, 1)); assert(!mode_allowed(c, 0));
  assert(!mode_allowed(c, 3)); assert(!mode_allowed(c, 4));
  c.squad_local_test = false; assert(!mode_allowed(c, 1)); assert(mode_allowed(c, 2));
  // Each peer has its own outgoing queue: A->B and B->A both hold inputs;
  // control 0xE3 bypasses both, matching agent-test semantics.
  DelayQueue<int> a(c), b(c); a.reset(1); b.reset(1);
  assert(a.push(0x80, 100, 1, 1)); assert(b.push(0xE1, 100, 1, 2));
  assert(!a.push(0xE3, 100, 1, 3));
  int total = 0;
  assert(!a.drain(159, [&](int value) { total += value; }));
  assert(!b.drain(159, [&](int value) { total += value; }));
  assert(a.drain(180, [&](int value) { total += value; }));
  assert(b.drain(180, [&](int value) { total += value; })); assert(total == 3);
#endif
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'flag.cpp'; cpp.write_text(fixture)
            for flags in ([], ['-DMU_SQUAD=1'], ['-DMU_SQUAD=1', '-DYOUGAME_AGENT_TEST_HOOK=1']):
                exe = Path(tmp) / 'test'
                result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                         *flags, '-I', str(ROOT / 'port/runtime/hle'), str(cpp), '-o', str(exe)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_install_after_admission_and_optional_code_removed(self):
        main = read(ROOT / 'port/app/main.cpp')
        start = main.index('if (!source_port::check_squad_test(')
        install = main.index('yougame::net_test::install(squad_net_config)')
        self.assertLess(start, install)
        self.assertIn('squad_net_config.squad_cancel_test = squad_test_invalid_lineup', main[start:install])
        for path in ('port/app/main.cpp', 'port/app/source_host.cpp', 'port/runtime/hle/yougame_net_test.h',
                     'port/runtime/hle/slippi_net.cpp', 'port/runtime/hle/slippi_net.h'):
            off = without_squad(read(ROOT / path))
            for token in ('--squad-test-net-delay', 'squad_local_test', 'squad_cancel_test', 'swap resimulated', 'squad_test_late_lineup_'):
                self.assertNotIn(token, off)

    def test_resimulation_evidence_uses_existing_swap_and_engine_clock(self):
        host = read(ROOT / 'port/app/source_host.cpp')
        log = function(host, 'h_log')
        block = log.split('if (yougame::net_test::config().squad_local_test)', 1)[1].split('#endif', 1)[0]
        self.assertIn('line.rfind("squad: swap P", 0)', block)
        self.assertIn('std::sscanf', block)
        self.assertIn('if (g_resim && repeated)', block)
        self.assertIn('g_squad_test_seen_swaps.insert((uint64_t(frame) << 32) | port)', block)
        self.assertIn('g_squad_test_seen_swaps.clear()', block)
        self.assertIn('squad: swap resimulated at frame %u (rollback from %u)', block)
        self.assertNotIn('g_game.', block)  # read-only: no callback into game state
        phase = function(host, 'h_resim_phase')
        self.assertIn('g_squad_test_rollback_from = rollback_engine_frame()', phase)
        self.assertIn('g_resim = entering != 0', phase)
        # Both clocks refer to the snapshotted engine-body counter.
        self.assertIn('mu_gm_engine_state()->unk_0', function(read(ROOT / 'sourceport/game/shim/mu_squad.c'), 'gmSquad_Frame'))

        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler required')
        body = log.split('  if (yougame::net_test::config().squad_local_test)', 1)[1].split('#endif', 1)[0]
        generated = r'''#include <string>
#include <unordered_set>
#include <cstdio>
#include <cassert>
#include "yougame_net_test.h"
bool g_resim = false;
unsigned g_squad_test_rollback_from = 498;
std::unordered_set<uint64_t> g_squad_test_seen_swaps;
int evidence = 0;
namespace host { template<class... T> void log(const char*, T...) { ++evidence; } }
void observe(std::string line) {
  if (yougame::net_test::config().squad_local_test)''' + body + '\n}\n' + r'''
int main() {
  observe("squad: swap P1 -> 2:0 at frame 500"); assert(g_squad_test_seen_swaps.empty());
  yougame::net_test::Config config; config.squad_local_test = true;
  yougame::net_test::install(config);
  observe("squad: swap P1 -> 2:0 at frame 500");
  g_resim = true;
  observe("squad: swap P2 -> 2:0 at frame 500"); assert(evidence == 0);
  observe("squad: swap P1 -> 2:0 at frame 500"); assert(evidence == 1);
  observe("squad: swap P1 -> 2:0 at frame 501"); assert(evidence == 1);
  observe("online: match starts,");
  observe("squad: swap P1 -> 2:0 at frame 500"); assert(evidence == 1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp, exe = Path(tmp) / 'observe.cpp', Path(tmp) / 'observe'
            cpp.write_text(generated)
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-DMU_SQUAD=1', '-DYOUGAME_AGENT_TEST_HOOK=1',
                                     '-I', str(ROOT / 'port/runtime/hle'), str(cpp), '-o', str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(exe)], check=True)

    def test_cancel_leg_holds_setup_and_uses_production_invalid_path(self):
        net = read(ROOT / 'port/runtime/hle/slippi_net.cpp')
        case = net.split('case NP_MSG_MU_SQUAD: {', 1)[1].split('if (!squad_search_', 1)[0]
        self.assertIn('config().squad_cancel_test', case)
        self.assertLess(case.index('squad_test_late_lineup_ = true'), case.index('SetMatchSelections(local)'))
        self.assertIn('late 0xE3 after invalid lineup', case)
        invalid = function(net, 'NetplayClient::SetMatchSelections')
        self.assertLess(invalid.index('!squad_test_late_lineup_'), invalid.index('CancelSquad("squad: invalid outgoing lineup'))
        receive = net.split('case ENET_EVENT_TYPE_RECEIVE: {', 1)[1]
        self.assertIn('squad_delay.squad_cancel_test ? squad_delay.delay_ms : 0', receive)
        self.assertIn('time_ms() + (cancel_lag ? cancel_lag : lag_ms)', receive)


if __name__ == '__main__':
    unittest.main()
