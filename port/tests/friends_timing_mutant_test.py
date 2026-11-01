#!/usr/bin/env python3
"""Compile the production fixes with one regression restored; each real host test must reject it."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HLE = ROOT / 'port/runtime/hle'
TESTS = ROOT / 'port/tests'

class FriendsTimingMutants(unittest.TestCase):
    def test_game_boundary_wiring(self):
        source = (ROOT/'port/app/source_host.cpp').read_text()
        branch = source.split('if (q->operation == Operation::Base) {',1)[1].split('// No game poll',1)[0]
        self.assertIn('capture_base(run.identity,run.spans,state,*result,why,q->storage)',branch)
        self.assertNotIn('handoff_state::encode(',branch)
        self.assertIn('wait_paused(q->operation,*result,*channel,began,',source)
        self.assertIn('q->storage = std::make_shared<Buffer>(info->capture_bytes)',
                      (HLE/'friends_handoff_transfer.h').read_text())

    def test_regressions_are_killed(self):
        with tempfile.TemporaryDirectory(prefix='friends-timing-mutants-') as name:
            tmp = Path(name)
            common = [os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread', '-w',
                      '-I'+str(tmp), '-I'+str(HLE), '-I'+str(ROOT/'port/third_party')]
            def build(label, sources, flags=()):
                exe = tmp / label
                proc = subprocess.run(common + list(flags) + list(map(str, sources)) + ['-o', str(exe)],
                                      capture_output=True, text=True)
                self.assertEqual(proc.returncode, 0, proc.stderr[-6000:])
                return exe
            def killed(exe, *args):
                proc = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=120)
                self.assertNotEqual(proc.returncode, 0, 'mutant survived: '+str(exe))
                self.assertTrue('FAIL' in proc.stderr or 'line ' in proc.stderr, proc.stderr)
                print(exe.name+': killed: '+proc.stderr.strip().splitlines()[-1])
            # Compile the portable codec once; the production old encoder is the reference oracle.
            objects = []
            for source in [HLE/'handoff_state.cpp', HLE/'native_savestate.cpp']:
                obj = tmp / (source.stem+'.o')
                subprocess.run(common+['-c', str(source), '-o', str(obj)], check=True)
                objects.append(obj)
            test = tmp/'friends_timing_test.cpp'
            test.write_text((TESTS/'friends_timing_test.cpp').read_text())
            capture = (HLE/'friends_handoff_capture.h').read_text()
            transfer = (HLE/'friends_handoff_transfer.h').read_text()
            (tmp/'friends_handoff_transfer.h').write_text(transfer)
            # Restore the old eager capture: the real test must reject work done on the caller.
            at = capture.index('  const auto began =')
            eager = capture[:at]+'''  auto bytes=std::make_shared<Buffer>(); handoff_state::Sizes sizes;
  if (!handoff_state::encode(identity,spans,state,*bytes,sizes,why)) return false;
  result.bytes=bytes; return true;
}
} // namespace
'''
            (tmp/'friends_handoff_capture.h').write_text(eager)
            killed(build('eager-capture', [test, *objects]), '--capture')
            (tmp/'friends_handoff_capture.h').write_text(capture)
            start = transfer.index('  const uint64_t n =', transfer.index('inline uint64_t transfer_budget('))
            end = transfer.index('\n}', start)
            (tmp/'friends_handoff_transfer.h').write_text(transfer[:start]+'  return 10000;'+transfer[end:])
            killed(build('fixed-budget', [test, *objects]), '--transfer')
            # Execute the extracted production pause loop, then restore its old 20 s guard.
            (tmp/'friends_handoff_transfer.h').write_text(transfer)
            baseline = build('baseline-timing', [test, *objects])
            subprocess.run([str(baseline), '--pause'], check=True)
            before = 'transfer_budget(result.bytes->size())'
            self.assertIn(before, transfer)
            (tmp/'friends_handoff_transfer.h').write_text(transfer.replace(before, '20000'))
            killed(build('fixed-pause-guard', [test, *objects]), '--pause')
            (tmp/'friends_handoff_transfer.h').write_text(transfer)
            # Start a real Link without startup's direct datagrams, then exercise both
            # room-only recovery and sticky route/control/data evidence independently.
            link = (HLE/'yougame_console_link.cpp').read_text()
            evidence = tmp/'friends_link_evidence_test.cpp'
            evidence.write_text((TESTS/evidence.name).read_text())
            def link_build(label, source):
                (tmp/'yougame_console_link.cpp').write_text(source)
                return build(label, [evidence, HLE/'yougame_console_engine.cpp'])
            subprocess.run([str(link_build('baseline-evidence', link))], check=True)
            before = 'room_carried &&'
            self.assertIn(before, link)
            killed(link_build('late-room-loss', link.replace(before, 'false &&')))
            before = 'const bool room_carried = !p.direct_seen;'
            self.assertIn(before, link)
            killed(link_build('always-room-carried', link.replace(before, 'const bool room_carried = true;')))
            before = '(room_carried && (io.room_rejoining() || p.away))'
            self.assertIn(before, link)
            killed(link_build('ignore-peer-away', link.replace(before, '(room_carried && io.room_rejoining())')))
            # Datagrams as evidence would mark every link direct during startup's handshake.
            before = '    p.heard = io.clock();\n'
            self.assertEqual(link.count(before), 1)
            mutant = link.replace(before, '    p.heard = io.clock(); p.direct_seen = true;\n')
            control = '      p.heard = now;\n      if (b[2] == 1)'
            self.assertEqual(link.count(control), 1)
            mutant = mutant.replace(control, '      p.heard = now; p.direct_seen = true;\n      if (b[2] == 1)')
            killed(link_build('datagram-evidence', mutant))
            (tmp/'yougame_console_link.cpp').write_text(mutant)
            (tmp/'friends_handoff_transfer.h').write_text(transfer)
            killed(build('datagram-evidence-startup', [TESTS/'yougame_console_link_test.cpp', tmp/'yougame_console_link.cpp',
                                                      HLE/'yougame_console_engine.cpp'], ['-DYOUGAME_AGENT_TEST_HOOK']), '--drop-timing')
            before = 'const bool room_carried = !p.direct_seen;'
            killed(link_build('transient-route-only', link.replace(before,
                'const bool room_carried = route != "lan" && route != "direct" && route != "relay";')))
            # Revert only the one-second missing-input notice. Engine's 3370 ms scenario must fail.
            engine = (HLE/'yougame_console_engine.cpp').read_text()
            before = 'input_waiting_ = !ready && waited >= 1000;'
            self.assertIn(before, engine)
            sdk = ['-I'+str(TESTS/'console_pad_sdk'), '-I'+str(ROOT/'sourceport/game/include'),
                   '-I'+str(ROOT/'port/runtime/abi')]
            (tmp/'yougame_console_engine.cpp').write_text(engine)
            baseline = build('baseline-engine', [TESTS/'yougame_console_engine_test.cpp',
                                                 tmp/'yougame_console_engine.cpp', objects[1]], sdk)
            subprocess.run([str(baseline)], check=True, capture_output=True)
            (tmp/'yougame_console_engine.cpp').write_text(engine.replace(before, 'input_waiting_ = false;'))
            killed(build('late-notice', [TESTS/'yougame_console_engine_test.cpp', tmp/'yougame_console_engine.cpp',
                                       objects[1]], sdk))
            before = 'recovering ? kReconnecting : kWaitingForFriend'
            self.assertIn(before, engine)
            (tmp/'yougame_console_engine.cpp').write_text(engine.replace(before, 'kReconnecting'))
            killed(build('false-reconnecting', [TESTS/'yougame_console_engine_test.cpp', tmp/'yougame_console_engine.cpp',
                                              objects[1]], sdk))
            # The explicit rejoin signal must also wait one second, rather than flashing at t=0.
            before = 'now-*relink_started_ < 1000'
            self.assertIn(before, engine)
            (tmp/'yougame_console_engine.cpp').write_text(engine.replace(before, 'false'))
            killed(build('early-notice', [TESTS/'yougame_console_engine_test.cpp', tmp/'yougame_console_engine.cpp',
                                        objects[1]], sdk))

if __name__ == '__main__':
    unittest.main()
