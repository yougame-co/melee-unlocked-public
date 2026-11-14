"""Squad replay suppression: guarded ABI wiring and production latch behavior.

Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from squad_wiring_test import CREDIT, GAME, ROOT, function, read, without_squad


HOST = ROOT / 'port/app/source_host.cpp'
LOG = 'replay: not kept: Squad Strike swaps cannot be played back yet'


class SquadReplayWiring(unittest.TestCase):
    def test_match_notification_uses_shared_abi_and_is_guarded(self):
        squad = read(GAME / 'shim/mu_squad.c')
        enter = function(squad, 'gmSquad_OnEnterVs')
        self.assertIn(CREDIT, enter)
        self.assertLess(enter.index('replay_match[2] = { MU_SQUAD_OP_REPLAY_MATCH, 0 }'),
                        enter.index('gmSquad_Disable()'))
        self.assertLess(enter.index('s_live = true'), enter.index('replay_match[1] = 1'))
        self.assertEqual(enter.count('mu_slippi_command(MU_SQUAD_COMMAND,'), 2)
        self.assertNotIn('replay_match', without_squad(squad))
        # mu_entry, unlike mu_slippi_cmd's no-Slippi stub, is built in both variants.
        entry = function(read(GAME / 'shim/mu_entry.c'), 'mu_slippi_command')
        self.assertIn('mu_host->slippi_command(command, payload, payload_size,', entry)
        self.assertNotIn('MU_NO_SLIPPI', entry)
        abi = read(ROOT / 'port/runtime/abi/mu_host.h')
        self.assertNotIn('squad', abi.lower())  # no new ABI table slot

    def test_host_policy_is_optional_and_intercepted_before_dispatch(self):
        host = read(HOST)
        disabled = without_squad(host)
        for name in ['g_replay_squad', 'g_squad_replay_pending', 'MU_SQUAD_OP_REPLAY_MATCH', LOG]:
            self.assertNotIn(name, disabled)
        handler = function(host, 'h_slippi_command')
        self.assertLess(handler.index('MU_SQUAD_OP_REPLAY_MATCH'),
                        handler.index('slippi::online::native_command'))
        self.assertIn('payload_size == 2', handler)
        self.assertIn('payload[1] <= 1', handler)

    def test_game_start_saves_previous_before_resetting_policy(self):
        # The first definition is the no-Slippi stub; select the full implementation.
        host = read(HOST)
        event = function(host[host.index('// ---- --replay:'):], 'h_replay_event')
        begin = event[event.index('if (!g_replay && command == 0x36)'):]
        self.assertLess(begin.index('write_replay_recording()'),
                        begin.index('g_replay_squad = g_squad_replay_pending'))
        self.assertLess(begin.index('g_replay_squad = g_squad_replay_pending'),
                        begin.index('g_squad_replay_pending = false'))
        self.assertLess(begin.index('g_squad_replay_pending = false'),
                        begin.index('g_replay_stream.begin_slippi'))

    def test_skip_returns_before_encoding_or_file_and_logs_once(self):
        host = read(HOST)
        writer = function(host[host.index('// ---- --replay:'):], 'write_replay_recording')
        self.assertEqual(host.count(LOG), 1)
        skipped = writer[writer.index('if (g_replay_squad &&'):writer.index('const auto file')]
        self.assertIn('g_replay_written = true;', skipped)
        self.assertIn('return;', skipped)
        self.assertNotIn('spectate::', skipped)
        self.assertNotIn('net_trace::', skipped)
        self.assertIn('!g_replay_written && g_replay_stream.active()', skipped)
        self.assertLess(writer.index('if (g_replay_squad &&'), writer.index('std::filesystem::'))

    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    def test_every_spectate_call_is_squad_guarded(self):
        event = function(read(HOST).split('// ---- --replay:', 1)[1], 'h_replay_event')
        message = 'spectate: not streamed: Squad Strike swaps cannot be watched yet'
        self.assertEqual(read(HOST).count(message), 1)
        block = event.split('if (g_replay_squad) {', 1)[1].split('g_replay_stream.begin_slippi', 1)[0]
        self.assertIn('set_event_sink(nullptr)', block.split('} else {', 1)[0])
        normal = block.split('} else {', 1)[1]
        self.assertEqual(normal.count('slippi::spectate::'), 2)  # start + sink write
        self.assertEqual(event.count('slippi::spectate::'), 4)
        self.assertEqual(event.count('if (!g_replay_squad)\n#endif\n'), 2)  # both end paths
        self.assertNotIn(message, without_squad(read(HOST)))

    def test_production_latch_ordering_cpp(self):
        cxx = shutil.which('c++')
        if not cxx:
            self.skipTest('C++ compiler unavailable')
        host = read(HOST)
        recording = host[host.index('// ---- --replay:'):]
        handler = function(host, 'h_slippi_command').split('  // Console capture/load', 1)[0]
        writer = function(recording, 'write_replay_recording').split('  const auto file', 1)[0]
        event = function(recording, 'h_replay_event')
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        state = ('bool g_squad_replay_pending = false;\nbool g_replay_squad = false;\n'
                 'bool g_replay_written = false;\nbool g_replay_spectator_only = false;\n')
        generated = state + handler + '  (void) response; (void) response_capacity;\n  return -1;\n}\n' + writer + '  ++writes;\n  g_replay_written = true;\n}\n'
        generated += function(read(GAME / 'shim/mu_entry.c'), 'mu_slippi_command')
        generated += event
        with tempfile.TemporaryDirectory(prefix='squad-replay-', dir=os.environ.get('TMPDIR')) as directory:
            work = Path(directory)
            (work / 'squad_replay_fixture.h').write_text(generated)
            for flags in [[], ['-DMU_NO_SLIPPI']]:
                command = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DMU_SQUAD=1',
                           *flags, '-I' + str(work), '-I' + str(ROOT / 'port/runtime/abi'),
                           str(ROOT / 'port/tests/squad_replay_test.cpp'), '-o', str(work / 'test')]
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(work / 'test')], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
