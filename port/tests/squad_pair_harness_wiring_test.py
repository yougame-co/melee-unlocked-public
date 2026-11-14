"""Squad harness admission, Linux switch and offline proof-checker fixtures.

Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
"""
import argparse
import contextlib
import importlib.util
import io
import json
# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
import re
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from squad_wiring_test import ROOT, function, read, without_squad


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class SquadPairHarness(unittest.TestCase):
    def test_linux_flag_both_calls_and_byte_identical_off(self):
        build = module('squad_build_linux', 'tools/build_linux.py')
        def commands(extra):
            with mock.patch.object(sys, 'argv', ['build_linux.py', '--build-dir', '/host',
                                  '--game-build-dir', '/game', '--port-gen', '/missing-gen', *extra]), \
                 mock.patch.object(sys, 'platform', 'linux'), \
                 mock.patch.object(build.shutil, 'which', return_value='/tool'), \
                 mock.patch.object(build, 'run') as run, contextlib.redirect_stdout(io.StringIO()):
                build.main()
                return [[str(x) for x in call.args] for call in run.call_args_list]
        # Exact configure argv before this harness existed (including order), plus the Doubles switch.
        expected_game = ['cmake', '-S', str(ROOT / 'sourceport/game'), '-B', '/game', '-G', 'Ninja',
                         '-DCMAKE_C_COMPILER=gcc-14', '-DCMAKE_CXX_COMPILER=g++-14',
                         '-DCMAKE_BUILD_TYPE=Release', '-DMELEE_CPU_BASELINE=AVX2',
                         '-DYOUGAME_AGENT_TEST_HOOK=OFF', '-DYOUGAME_RANKED_BRANCH=ON', '-DMELEE_DOUBLES=OFF']
        decomp = ROOT / 'build-native/decomp'
        if (decomp / 'src/melee').is_dir():
            expected_game += ['-DMELEE_DECOMP_DIR=' + str(decomp)]
        expected_host = ['cmake', '-S', str(ROOT), '-B', '/host', '-G', 'Ninja',
                         '-DCMAKE_CXX_COMPILER=g++-14', '-DCMAKE_BUILD_TYPE=Release',
                         '-DMELEE_BUILD_NATIVE_LINUX=ON', '-DMELEE_CPU_BASELINE=AVX2',
                         '-DMELEE_LINUX_GAME_LIBRARY=/game/melee_game.so', '-DMELEE_LINUX_GRAPHICS=ON',
                         '-DYOUGAME_AGENT_TEST_HOOK=OFF', '-DYOUGAME_RANKED_BRANCH=ON', '-DMELEE_DOUBLES=OFF',
                         '-DMELEE_PORT_GEN=']
        off = commands([])
        self.assertEqual(off, commands(['--no-squad-strike']))
        self.assertEqual(off[0], expected_game)
        self.assertEqual(off[2], expected_host)
        on = commands(['--squad-strike'])
        for index in (0, 2):
            self.assertEqual(on[index].count('-DMELEE_SQUAD_STRIKE=ON'), 1)
            self.assertEqual([arg for arg in on[index] if arg != '-DMELEE_SQUAD_STRIKE=ON'], off[index])
        for extra in ([], ['--squad-strike']):
            p = subprocess.run([sys.executable, str(ROOT / 'tools/build_linux.py'), '--print-cmake', *extra],
                               capture_output=True, text=True, check=True)
            self.assertEqual(p.stdout.count('-DMELEE_SQUAD_STRIKE=ON'), 2 if extra else 0)
            self.assertIn('MELEE_SQUAD_STRIKE=' + ('ON' if extra else 'OFF'), p.stdout)

    def test_linux_game_host_consumer_define_wiring(self):
        linux = read(ROOT / 'port/linux/CMakeLists.txt')
        full = read(ROOT / 'port/linux/full/CMakeLists.txt')
        game = read(ROOT / 'sourceport/game/CMakeLists.txt')
        self.assertIn('/port/cmake/squad.cmake', linux)
        self.assertIn('mu_squad_target(melee_linux)', linux)
        self.assertIn('mu_squad_target(full_runtime)', full)
        self.assertIn('target_link_libraries(melee_source PRIVATE full_runtime', full)
        self.assertIn('MU_SQUAD=1', game)
        self.assertIn('PUBLIC MU_SQUAD=1', read(ROOT / 'port/cmake/squad.cmake'))

    def test_mode_absent_off_local_peer_required_after_argument_parse(self):
        host = read(ROOT / 'port/app/source_host.cpp')
        main = read(ROOT / 'port/app/main.cpp')
        for text in (host, main, read(ROOT / 'port/runtime/hle/squad_test_spec.h')):
            off = without_squad(text)
            self.assertNotIn('g_online_test_squad', off)
            self.assertNotIn('squad_test::parse', off)
            self.assertNotIn('--squad-test-invalid-lineup', off)
            self.assertNotIn('squad:<ck>', off)
        gate = function(host, 'check_squad_test')
        self.assertIn('!local_peer', gate)
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        self.assertIn('(invalid_lineup || net_delay) && !g_online_test_squad.count', gate)
        self.assertGreater(main.index('source_port::check_squad_test'), main.index('else if (a == "--online-test")'))
        parse = function(host, 'set_online_test')
        self.assertIn('g_online_test_mode = 1', parse)
        self.assertIn('slippi::squad_test::parse', parse)

    def test_real_mode_parser_and_gate_off_on(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler required')
        host = read(ROOT / 'port/app/source_host.cpp')
        parse = function(host, 'set_online_test')
        gate = function(host, 'check_squad_test')
        fixture = r'''#include <string>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include "squad_test_spec.h"
int g_online_test_mode = -1;
uint8_t g_online_test_character = 2, g_online_test_color = 0;
namespace yougame::online_test { struct QuitAfter { QuitAfter(int=0,const char*=nullptr); }; }
struct { yougame::online_test::QuitAfter quit; } g_ranked_harness;
#if MU_SQUAD
MuSquadSide g_online_test_squad{};
bool g_online_test_squad_cancel = false;
#endif
'''.replace('const char*=', 'const char* =')
        fixture = fixture.replace('QuitAfter(int=0,const char* =nullptr);', 'QuitAfter(int=0,const char* =nullptr) {}')
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        fixture += parse + '\n#if MU_SQUAD\n' + gate.replace('bool net_delay)', 'bool net_delay = false)') + '\n#endif\n'
        fixture += r'''
int main() {
  assert(set_online_test("unranked"));
#if MU_SQUAD
  assert(set_online_test("squad:4/0,14/0,18/0,9/0"));
  assert(g_online_test_mode == 1 && g_online_test_character == 4);
  assert(g_online_test_squad.count == MU_SQUAD_LINEUP && g_online_test_squad.members[2].kind == 18);
  assert(g_online_test_squad.members[3].kind == 9);
  // Exactly four fighters: one, two, three and five are refused and leave the last lineup alone.
  for (const char* bad : {"squad:4/0", "squad:4/0,14/0", "squad:4/0,14/0,18/0",
                          "squad:4/0,14/0,18/0,9/0,2/0", "squad:4/0,14/0,18/0,26/0"}) {
    assert(!set_online_test(bad));
    assert(g_online_test_squad.count == MU_SQUAD_LINEUP && g_online_test_squad.members[0].kind == 4);
  }
  assert(!check_squad_test(false, false));
  assert(!check_squad_test(false, true));
  assert(check_squad_test(true, false));
  assert(check_squad_test(true, true));
  assert(!set_online_test("squad:26/0"));
  assert(set_online_test("direct"));
  assert(!check_squad_test(true, true));
#else
  assert(!set_online_test("squad:4/0,14/0,18/0,9/0"));
#endif
  assert(!set_online_test("unknown"));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'mode.cpp'; cpp.write_text(fixture)
            for enabled in (False, True):
                exe = Path(tmp) / ('on' if enabled else 'off')
                subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                *(['-DMU_SQUAD=1'] if enabled else []),
                                '-I', str(ROOT / 'port/runtime/hle'), str(cpp), '-o', str(exe)],
                               capture_output=True, text=True, check=True)
                subprocess.run([str(exe)], check=True)

    def test_harness_uses_production_lineup_budget_search_setup_and_entry(self):
        host = function(read(ROOT / 'port/app/source_host.cpp'), 'h_online_test_match')
        game = function(read(ROOT / 'sourceport/game/shim/mu_squad_online.c'), 'gmSquad_OnlineTest')
        for call in ('gmSquad_OnlineChoose(true)', 'gmSquad_OnlineCss()', 'gmSquad_OnlineSelect(',
                     'gmSquad_OnlineSearchReady()'):
            self.assertIn(call, game)
        self.assertIn('MU_SQUAD_TEST_SETUP', host)
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        # The menu stand-in refuses unless called from inside the host's launch answer.
        self.assertLess(game.index('if (!mu_online_launch_query()) return -1;'), game.index('gmSquad_OnlineChoose(true)'))
        online = read(ROOT / 'sourceport/game/shim/mu_online.c')
        launch = function(online, 'launch_kind')
        squad = launch[launch.index('#if MU_SQUAD'):launch.index('#else')]
        self.assertLess(squad.index('launch_query = 1;'), squad.index('mu_online_abi_test_match('))
        self.assertLess(squad.index('mu_online_abi_test_match('), squad.index('launch_query = 0;'))
        self.assertEqual(online.count('launch_query = 1;'), 1)
        self.assertIn('native_start_match(', host)
        self.assertLess(host.index('MU_SQUAD_TEST_SETUP'), host.index('slippi::online::native_start_match('))
        self.assertNotIn('SetSquadSelection(', host)
        self.assertNotIn('MuSquadSetup', host)
        preload = function(read(ROOT / 'sourceport/game/shim/mu_replay.c'), 'mu_replay_prepare_scene')
        # Setup is imported before lbDvd_80018254, whose lbDvd_80017960 hook adds the squad preloads once the heaps
        # exist; a direct call here queued into a destroyed heap 4 (Linux test PC, 11-04).
        self.assertLess(preload.index('gmSquad_OnlineSetup('), preload.index('lbDvd_80018254('))
        self.assertNotIn('gmSquad_AddPreloads(', preload)
        self.assertIn('+    gmSquad_AddPreloads(game_cache);', read(ROOT / 'sourceport/patches/melee-native.patch'))
        entry = function(read(ROOT / 'sourceport/game/shim/mu_online.c'), 'mu_online_start_melee')
        self.assertIn('mu_online.test && gm_GetCurrentGameMode() == GM_DEBUG_VS', entry)
        self.assertIn('gmSquad_OnEnterVs(data)', entry)
        online = read(ROOT / 'port/runtime/hle/slippi_online.cpp')
        self.assertIn('search.squad = yougame::squad::selected(search.mode, g_squad_row', online)
        self.assertIn('if (g_last_search.squad) g_netplay->SetSquadSelection(g_squad_side)', online)

    def test_script_feeds_netplay_before_delay_and_rollback(self):
        script = read(ROOT / 'port/scripts/squad_sd.txt')
        self.assertIn('@match\n0 sx=100', script)
        self.assertIn('host::input_poll(pads)', function(read(ROOT / 'port/app/source_host.cpp'), 'h_pad_read'))
        window = read(ROOT / 'port/runtime/host/window.cpp')
        self.assertIn('script_pads(retrace_count(), true, out)', function(window, 'input_poll'))
        game = read(ROOT / 'sourceport/game/shim/mu_online.c')
        for code in ('source = &stat[mu_online.input_source]', 'memcpy(tx + 13, mu_online.last_local, PAD_SIZE)',
                     'command(CMD_ONLINE_INPUTS, tx, sizeof tx', 'mu_online.delay_buffer[mu_online.delay_index]'):
            self.assertIn(code, game)
        online = read(ROOT / 'port/runtime/hle/slippi_online.cpp')
        self.assertIn('SendSlippiPad(std::make_unique<Pad>(frame + delay, finalized, finalized_checksum, inputs),', online)

    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def fixture(self, out, cancel=False, frames=2):
        runner = module('squad_pair_fixture', 'port/linux/squad_pair.py')
        for index in range(2):
            work = out / f'peer-{index}'
            work.mkdir()
            (work / 'host.log').write_text(runner.PROTOCOL_ERROR if cancel else
                'squad: P1 4:0 14:0 18:0\nsquad: P2 19:0 14:0 4:0\n'
                'squad: swap P1 -> 14:0 at frame 500\nsquad: swap P1 -> 18:0 at frame 900\n' + runner.REPLAY_SKIP +
                '\nsquad digest: 0 late resims with the same state, 0 beyond the kept window\n'
                'squad digest: 40 new bodies on a held match frame\n')
            # Use the production comparator's exact field set and format.
            from compare_digest import RAW
            rows = [','.join(RAW)]
            for f in range(frames + 1):
                rows.append(','.join(str(f) if key == 'frame' else format(f, 'X') if key == 'match_frame' else '0' for key in RAW))
            if cancel:
                rows = rows[:2]
            (work / 'digest.csv').write_text('\n'.join(rows) + '\n')
            # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
            (work / 'digest.csv.confirmed.csv').write_text('\n'.join([rows[0], *rows[2:]]) + '\n')
            (work / 'digest.csv.presented-frontier.csv').write_text(
                'frame,match_frame,body_frame,confirmed_frontier\n' +
                ''.join(f'{f},{f},{f+100},{f+100}\n' for f in range(1, frames + 1)))
            (work / 'digest.csv.late.csv').write_text(','.join(runner.LATE_HEADER) + '\n')
        return runner

    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def test_late_resim_rows_missing_file_or_unchecked_resims_fail(self):
        for defect in ('late-row-a', 'late-row-b', 'no-file', 'beyond-window', 'no-count'):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp); runner = self.fixture(out)
                peer = out / ('peer-0' if defect == 'late-row-a' else 'peer-1')
                late, log = peer / 'digest.csv.late.csv', peer / 'host.log'
                if defect.startswith('late-row'):
                    late.write_text(late.read_text() + '2,104,100,100\n')
                elif defect == 'no-file':
                    late.unlink()
                elif defect == 'beyond-window':
                    log.write_text(log.read_text().replace('0 beyond', '1 beyond'))
                else:
                    log.write_text(log.read_text().replace('squad digest:', 'other:'))
                self.assertFalse(runner.proof(out))
                text = (out / 'proof.txt').read_text()
                self.assertIn('FAIL ', text)
                if defect.startswith('late-row'):
                    self.assertIn('late rows; first body 2', text)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); runner = self.fixture(out)
            for index in range(2):
                log = out / f'peer-{index}/host.log'
                log.write_text(log.read_text().replace('0 late resims', '7 late resims'))
            self.assertTrue(runner.proof(out))
            text = (out / 'proof.txt').read_text()
            self.assertIn('INFO peer-1 late resims with the same state: 7', text)
            self.assertIn('INFO peer-0 new bodies on a held match frame: 40', text)

    def test_proof_passes_matching_and_cancel_fixtures(self):
        for cancel in (False, True):
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp)
                runner = self.fixture(out, cancel)
                self.assertTrue(runner.proof(out, cancel))
                proof = (out / 'proof.txt').read_text()
                self.assertNotIn('FAIL ', proof)
                if not cancel:
                    for label in ('both reached match frames', 'confirmed digests identical by match_frame',
                                  'grep DESYNC = 0', 'start lines identical', 'at least 2 swaps',
                                  runner.REPLAY_SKIP, 'no squad: cancel/protocol-error'):
                        self.assertIn(label, proof)

    def test_proof_rejects_missing_swaps_desync_mismatch_and_partial_cancel(self):
        for defect in ('DESYNC', 'swaps', 'digest', 'start', 'replay', 'cancel', 'timeout', 'exit'):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp)
                runner = self.fixture(out)
                log = out / 'peer-1/host.log'
                text = log.read_text()
                if defect == 'DESYNC': text += '\nslippi: DESYNC: checksum mismatch at frame 2'
                if defect == 'swaps': text = text.replace('at frame 900', 'at frame 901')
                if defect == 'start': text = text.replace('P1 4:0', 'P1 4:1')
                if defect == 'replay': text = text.replace(runner.REPLAY_SKIP, '')
                if defect == 'cancel': text += '\nsquad: cancel'
                log.write_text(text)
                if defect == 'digest':
                    path = out / 'peer-1/digest.csv.confirmed.csv'
                    rows = path.read_text().splitlines(); values = rows[-1].split(','); values[-1] = '1'
                    rows[-1] = ','.join(values); path.write_text('\n'.join(rows) + '\n')
                self.assertFalse(runner.proof(out, codes=(1, 0) if defect == 'exit' else (0, 0), timed_out=defect == 'timeout'))
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); runner = self.fixture(out, True)
            (out / 'peer-1/host.log').write_text('no protocol error')
            self.assertFalse(runner.proof(out, True))

    def test_runner_isolation_command_and_owned_pid_timeout(self):
        runner = module('squad_pair_command', 'port/linux/squad_pair.py')
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); (out / 'bin/Sys').mkdir(parents=True)
            # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
            args = mock.Mock(out=out, host=out / 'bin/melee_source', iso=out / 'disc.iso',
                             lineup_a=runner.DEFAULT_A, lineup_b=runner.DEFAULT_B,
                             frames=4200, stage=2, cancel=True, delay='60:20')
            for index in range(2):
                work, env, cmd = runner.isolation(args, index)
                self.assertIn(f'{index}:{43171+index}:127.0.0.1:{43172-index}', cmd)
                self.assertEqual(cmd[cmd.index('--squad-test-net-delay') + 1], '60:20')
                for flag in ('--settings-path', '--sys-dir', '--user-dir', '--card-dir', '--replay-dir',
                             '--log-file', '--shader-cache', '--state-digest', '--script'):
                    self.assertIn(flag, cmd)
                self.assertEqual(env['MELEE_TEST_SCRIPT_PAD_FRAMES'], '1')
                self.assertEqual(env['MELEE_SOURCE_RECORD'], '1')
                self.assertTrue(env['HOME'].startswith(str(work)))
                self.assertEqual('--squad-test-invalid-lineup' in cmd, index == 1)
        text = read(ROOT / 'port/linux/squad_pair.py')
        self.assertIn("sys.platform != 'linux'", text)
        self.assertIn('child.kill()', text)
        self.assertNotIn('os.kill(', text)
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        self.assertIn('confirmed digests identical by match_frame in both directions', text)


    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def test_all_plan_combines_fixture_legs_and_limits_without_processes(self):
        runner = module('squad_all_fixture', 'port/linux/squad_pair.py')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = root / 'proof'
            (root / 'bin/Sys').mkdir(parents=True)
            host, iso = root / 'bin/melee_source', root / 'disc.iso'
            host.touch(); iso.touch()
            plans = []
            def leg(arg):
                plans.append((arg.out.name, arg.delay, arg.cancel))
                self.fixture(arg.out, arg.cancel)
                for index in range(2):
                    path = arg.out / f'peer-{index}/host.log'
                    with path.open('a') as log:
                        log.write('\nsquad: swap resimulated at frame 500 (rollback from 498)\n' if not arg.cancel else
                                  '\nsquad: cancel race: late 0xE3 after invalid lineup\n')
                return int(not runner.proof(arg.out, arg.cancel, delay=arg.delay))
            with mock.patch.object(runner.subprocess, 'Popen', side_effect=AssertionError('no processes')), \
                 mock.patch.object(runner, 'run_pair', side_effect=leg), \
                 mock.patch.object(sys, 'platform', 'linux'), \
                 mock.patch.object(sys, 'argv', ['squad_pair.py', '--all', '--host', str(host),
                                               '--iso', str(iso), '--out', str(out)]), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.main(), 0)
            self.assertEqual(plans, [('leg-1', None, False), ('leg-2', '60:20', False), ('leg-3', '60:20', True)])
            text = (out / 'proof.txt').read_text()
            # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
            self.assertEqual(len(re.findall(r'^\d+\. ', runner.LIMITS, re.M)), 6)
            self.assertIn('fails the coverage check rather than passing silently', runner.LIMITS)
            self.assertEqual(text.count('LIMITS'), 1)
            self.assertIn(runner.LIMITS, text)
            self.assertIn('PASS all three proof legs', text)
            self.assertIn('swap frame 500 resimulated (rollback from 498)', text)
            self.assertIn('cancel raced a late 0xE3', text)
            self.assertIn('TEST cancel leg:', text)
            self.assertIn('squad_transport_fixture, "first lock-in after connection"', text)

    def test_delay_requires_swap_resimulation_and_cancel_race_evidence(self):
        for cancel in (False, True):
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp); runner = self.fixture(out, cancel)
                self.assertFalse(runner.proof(out, cancel, delay='60:20'))

    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def test_confirmed_mismatch_missing_coverage_duplicates_and_order_fail(self):
        for defect in ('missing-left', 'missing-right', 'changed-middle', 'duplicate', 'descending'):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp); runner = self.fixture(out)
                path = out / ('peer-0/digest.csv.confirmed.csv' if defect == 'missing-left' else 'peer-1/digest.csv.confirmed.csv')
                rows = path.read_text().splitlines()
                if defect.startswith('missing'):
                    rows.pop()
                elif defect == 'descending':
                    rows[1:] = reversed(rows[1:])
                elif defect == 'duplicate':
                    rows.append(rows[-1])
                else:
                    values = rows[1].split(','); values[1] = 'BAD'
                    rows[1] = ','.join(values)
                path.write_text('\n'.join(rows) + '\n')
                self.assertFalse(runner.proof(out))

    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def test_presented_mismatch_only_is_info_and_passes_under_delay(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); runner = self.fixture(out)
            path = out / 'peer-1/digest.csv'
            rows = path.read_text().splitlines()
            values = rows[-1].split(','); values[1] = 'BAD'
            rows.append(','.join(values)) # every occurrence counts, even a later duplicate
            path.write_text('\n'.join(rows) + '\n')
            for index in range(2):
                log = out / f'peer-{index}/host.log'
                log.write_text(log.read_text() + '\nsquad: swap resimulated at frame 500 (rollback from 498)\n')
            self.assertTrue(runner.proof(out, delay='60:20'))
            text = (out / 'proof.txt').read_text()
            self.assertIn('INFO presented rows differing: 1 (predicted frames are expected under delay)', text)
            self.assertNotRegex(text, r'(?:PASS|FAIL) presented rows differing:')

    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def test_coverage_95_percent_boundary_and_larger_peer_denominator(self):
        for keep, expected in ((94, False), (95, True)):
            with self.subTest(keep=keep), tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp); runner = self.fixture(out, frames=100)
                for index in range(2):
                    path = out / f'peer-{index}/digest.csv.confirmed.csv'
                    path.write_text('\n'.join(path.read_text().splitlines()[:keep+1]) + '\n')
                self.assertEqual(runner.proof(out), expected)
                text = (out / 'proof.txt').read_text()
                self.assertIn(f'confirmed coverage on BOTH peers: {keep}/100', text)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); runner = self.fixture(out, frames=100)
            path = out / 'peer-1/digest.csv'
            rows = path.read_text().splitlines(); values = rows[-1].split(',')
            values[0] = '110'; values[rows[0].split(',').index('match_frame')] = '6E'; rows.append(','.join(values))
            path.write_text('\n'.join(rows) + '\n')
            self.assertFalse(runner.proof(out))
            self.assertIn('100/110', (out / 'proof.txt').read_text())

    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def test_leg_one_rejects_already_confirmed_presented_disagreement(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); runner = self.fixture(out)
            path = out / 'peer-1/digest.csv'
            rows = path.read_text().splitlines(); values = rows[-1].split(','); values[1] = 'BAD'
            rows[-1] = ','.join(values); path.write_text('\n'.join(rows) + '\n')
            self.assertFalse(runner.proof(out))
            text = (out / 'proof.txt').read_text()
            self.assertIn('FAIL leg-1 presented/confirmed agree', text)
            # Code permits prediction even on loopback. A tagged predicted presentation
            # may differ; the confirmed cross-peer comparison still decides its final state.
            tags = out / 'peer-1/digest.csv.presented-frontier.csv'
            tags.write_text(tags.read_text().replace('2,2,102,102', '2,2,102,101'))
            self.assertTrue(runner.proof(out))

    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def test_leg_one_requires_frontier_metadata_and_nonvacuous_check(self):
        for defect in ('missing', 'duplicate', 'all-predicted'):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp); runner = self.fixture(out)
                for index in range(2):
                    tags = out / f'peer-{index}/digest.csv.presented-frontier.csv'
                    if defect == 'missing': tags.unlink()
                    elif defect == 'duplicate': tags.write_text(tags.read_text() + '2,2,102,102\n')
                    else: tags.write_text('frame,match_frame,body_frame,confirmed_frontier\n1,1,101,0\n2,2,102,0\n')
                self.assertFalse(runner.proof(out))

    def test_delay_cli_ranges(self):
        runner = module('squad_delay_spec', 'port/linux/squad_pair.py')
        for text in ('1', '250', '60:20', '1:0', '250:100'):
            self.assertEqual(runner.delay_spec(text), text)
        for text in ('', '0', '251', '-1', '+1', '1:', ':1', '1:101', '1:-1', '1:2:3', '1x', ' 1'):
            with self.subTest(text=text), self.assertRaises(argparse.ArgumentTypeError):
                runner.delay_spec(text)

    def test_debug_vs_squad_paths_are_harness_only(self):
        # A user-approved lobby launch also enters debug VS with mu_online_pending(): only the
        # --online-test harness (launch kind 1) may take the squad paths there.
        squad = read(ROOT / 'sourceport/game/shim/mu_squad.c')
        lines = [x for x in squad.splitlines() if 'GM_DEBUG_VS' in x and not x.lstrip().startswith('*')]
        self.assertEqual(len(lines), 3)
        for line in lines:
            self.assertIn('mu_online_harness()', line)
        replay = read(ROOT / 'sourceport/game/shim/mu_replay.c')
        self.assertIn('if (mu_online_pending() && mu_online_harness() && gmSquad_OnlineChosen()) {', replay)
        online = read(ROOT / 'sourceport/game/shim/mu_online.c')
        self.assertIn('if (mu_online.test && gm_GetCurrentGameMode() == GM_DEBUG_VS && gmSquad_OnlineChosen())', online)
        harness = function(online, 'mu_online_harness')
        self.assertIn('return mu_online.test;', harness)
        self.assertIn('mu_online.test = kind == 1;', online)


if __name__ == '__main__':
    unittest.main()
