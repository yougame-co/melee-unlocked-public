#!/usr/bin/env python3
# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
"""Linux-only, isolated two-copy Squad Strike proof over loopback. No servers/accounts.

Build the full melee_source host with tools/build_linux.py --squad-strike --agent-test-hook
--port-gen /path/to/generated-headers (hle_decls.h AND guest_symbols.h required).
The full proof plan is --all: no delay, 60:20 delay, then cancel with 60:20 delay.
--delay uses additive jitter and delays outgoing inputs on BOTH peers.
Example: python3 port/linux/squad_pair.py --host /build/melee_source --iso /disc.iso --out /proof
A defaults to Kirby(4), Ice Climbers(14), Zelda(18), Fox(2); B to Sheik(19),
Ice Climbers(14), Kirby(4). These are decimal external CKind ids, costume zero.
Fountain (stage 2) has walk-off ledges: holding right leaves the stage and
respawn platform repeatedly. Linux execution must establish the ~3000-frame KO bound.
"""
import argparse
# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
import copy
import csv
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'port/mac'))
from compare_digest import load

DEFAULT_A = '4/0,14/0,18/0,9/0'
DEFAULT_B = '19/0,14/0,4/0,2/0'
REPLAY_SKIP = 'replay: not kept: Squad Strike swaps cannot be played back yet'
PROTOCOL_ERROR = 'squad: cancel: Squad Strike protocol error'
FALLBACK = "[game] online test: matchmaking failed: Squad Strike setup didn't match. Match not started."
# Current production diagnostic (slippi_online.cpp), plus every case-sensitive DESYNC marker.
DESYNC = 'slippi: DESYNC: checksum mismatch at frame '
GAME_DESYNC = 'online: DESYNC at frame '
# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
LIMITS = '''LIMITS
1. Loopback --local-peer never touches YouGame matchmaking: ticket, the "sq" compat-key queue isolation and room roster are not proven here (unit-tested; the real queue waits for a staging run).
2. Delay legs simulate latency in-process; real loss and reordering on the internet are not covered.
3. One VM: identical CPU and build on both sides; cross-OS comes later.
4. The harness enters debug VS and imports the exchanged setup itself (mu_replay_prepare_scene, mu_online_start_melee hooks gated on the harness); the production CSS/splash preload path is exercised by the Windows offline smoke and the later staging run, not here.
5. The cancel leg forces the order (late 0xE3 first); it does not prove every interleaving. The order-free check is the m2j transport fixture (squad_transport_fixture, "first lock-in after connection").
6. Confirmed rows cover one match per process; a second match in the same process records no new rows, so it fails the coverage check rather than passing silently.
'''


def delay_spec(text):
    if not re.fullmatch(r'[0-9]+(?::[0-9]+)?', text):
        raise argparse.ArgumentTypeError('delay must be MS[:JITTER], delay 1..250, jitter 0..100')
    parts = [int(value) for value in text.split(':')]
    if not 1 <= parts[0] <= 250 or (len(parts) == 2 and not 0 <= parts[1] <= 100):
        raise argparse.ArgumentTypeError('delay must be MS[:JITTER], delay 1..250, jitter 0..100')
    return text


def isolation(args, index):
    work = args.out / f'peer-{index}'
    work.mkdir()  # never reuse a profile from an earlier proof
    environment = {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'LANG': 'C', 'LC_ALL': 'C',
                   'MELEE_NO_GC_ADAPTER': '1', 'SDL_AUDIODRIVER': 'dummy',
                   'MELEE_TEST_SCRIPT_PAD_FRAMES': '1', 'MELEE_SOURCE_RECORD': '1'}
    for key, sub in [('HOME', 'home'), ('XDG_DATA_HOME', 'data'), ('XDG_CONFIG_HOME', 'config'),
                     ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state'),
                     ('XDG_RUNTIME_DIR', 'run'), ('APPDATA', 'appdata'),
                     ('LOCALAPPDATA', 'localappdata'), ('TMPDIR', 'tmp')]:
        path = work / sub
        path.mkdir(mode=0o700)
        environment[key] = str(path)
    # Private Sys too: generated files cannot change the sibling process's view.
    # host/sysgen.cpp rebuilds the two disc-derived Sys files from deltas/ beside the Sys folder (its parent) into
    # --user-dir, which must be outside that parent: Sys and deltas get their own 'game' folder next to user/.
    shutil.copytree(args.host.parent / 'Sys', work / 'game/Sys')
    if (args.host.parent / 'deltas').is_dir():
        shutil.copytree(args.host.parent / 'deltas', work / 'game/deltas')
    else:
        print(f'peer-{index}: deltas/: none beside host (Sys generation will fail)', flush=True)
    local, remote = (43171, 43172) if index == 0 else (43172, 43171)
    lineup = args.lineup_a if index == 0 else args.lineup_b
    command = [str(args.host), '--iso', str(args.iso), '--headless', '--volume', '0',
               '--settings-path', str(work / 'settings.ini'), '--sys-dir', str(work / 'game/Sys'),
               '--user-dir', str(work / 'user'), '--card-dir', str(work / 'cards'),
               '--replay-dir', str(work / 'replays'), '--log-file', str(work / 'host.log'),
               '--shader-cache', str(work / 'shaders'), '--frames', str(args.frames),
               '--online-test', 'squad:' + lineup,
               '--local-peer', f'{index}:{local}:127.0.0.1:{remote}',
               '--test-stage', str(args.stage), '--rng-seed', '0x12345678', '--time-base', '0',
               '--script', str(ROOT / 'port/scripts/squad_sd.txt'),
               '--state-digest', str(work / 'digest.csv')]
    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    if args.delay:
        command += ['--squad-test-net-delay', args.delay]
    if args.cancel and index == 1:
        command.append('--squad-test-invalid-lineup')
    (work / 'command.txt').write_text(shlex.join(command) + '\n')
    (work / 'environment.json').write_text(json.dumps(environment, indent=2) + '\n')
    return work, environment, command


# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
def digest_evidence(out):
    paths = [out / f'peer-{index}/digest.csv' for index in range(2)]
    presented = [load(path) for path in paths]
    confirmed = [load(str(path) + '.confirmed.csv') for path in paths]
    if not all(rows for rows, _ in presented):
        raise ValueError('both presented digests must contain rows')
    fields = [key for key in presented[0][0][0] if key != 'frame']
    for rows, frames in confirmed:
        sequence = [row['match_frame'] for row in rows]
        if any(frame <= 0 for frame in sequence) or sequence != sorted(set(sequence)):
            raise ValueError('confirmed rows must be positive, ascending, without duplicates')
        if len(sequence) != len(frames):
            raise ValueError('invalid confirmed rows')
    shared = confirmed[0][1].keys() & confirmed[1][1].keys()
    # Inspect each direction explicitly, across every field of every shared confirmed frame.
    mismatches = sorted({frame for own, other in ((confirmed[0][1], confirmed[1][1]),
                                                 (confirmed[1][1], confirmed[0][1]))
                         for frame in own.keys() & other.keys()
                         if any(own[frame][key] != other[frame][key] for key in fields)})
    maximum = max((row['match_frame'] for rows, _ in presented + confirmed for row in rows), default=0)
    coverage = len(shared) / maximum if maximum else 0
    # Presented duplicates are informational: count differing rows in both directions,
    # checking every occurrence (a duplicate cannot hide a difference).
    differing = [row['match_frame'] for (rows, _), (_, other) in
                 ((presented[0], presented[1]), (presented[1], presented[0]))
                 for row in rows if row['match_frame'] > 0 and row['match_frame'] in other
                 and any(row[key] != other[row['match_frame']][key] for key in fields)]
    return dict(compared_frames=len(shared), confirmed_mismatches=mismatches,
                missing_on_a=sorted(confirmed[1][1].keys() - confirmed[0][1].keys()),
                missing_on_b=sorted(confirmed[0][1].keys() - confirmed[1][1].keys()),
                max_match_frame=maximum, coverage=coverage, presented_differing=len(differing)), presented, confirmed


# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
LATE_HEADER = ['body', 'match_frame', 'confirmed_match_frame', 'frontier_at_confirm']
HELD_BODIES = re.compile(r'squad digest: (\d+) new bodies on a held match frame')
LATE_COUNTS = re.compile(r'squad digest: (\d+) late resims with the same state, (\d+) beyond the kept window')


def late_evidence(out, logs):
    """Final bodies re-simulated later: per peer (late rows, same, beyond window, held-counter bodies)."""
    result = []
    for index, log in enumerate(logs):
        with (out / f'peer-{index}/digest.csv.late.csv').open(newline='') as source:
            reader = csv.reader(source)
            if next(reader, None) != LATE_HEADER:
                raise ValueError(f'peer-{index} late file header')
            rows = [row for row in reader if row]
        counts = LATE_COUNTS.findall(log)
        same, beyond = (int(value) for value in counts[-1]) if counts else (None, None)
        held = HELD_BODIES.findall(log)
        result.append((rows, same, beyond, int(held[-1]) if held else None))
    return result


# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
def check_presented_confirmed(out, presented, confirmed):
    checked = 0
    mismatches = []
    held = []
    for index, ((rows, _), (_, final)) in enumerate(zip(presented, confirmed)):
        path = out / f'peer-{index}/digest.csv.presented-frontier.csv'
        with path.open(newline='') as source:
            tags = list(csv.DictReader(source))
        if not tags:
            raise ValueError(f'no presented frontier tags: {path}')
        by_retrace = {}
        for tag in tags:
            values = {key: int(tag[key]) for key in ('frame', 'match_frame', 'body_frame', 'confirmed_frontier')}
            if values['frame'] in by_retrace:
                raise ValueError('duplicate presented frontier tag')
            by_retrace[values['frame']] = values
        last_frame, shown, after = max(final), False, 0
        for row in rows:
            frame = row['match_frame']
            if frame <= 0 or frame not in final:
                continue
            if frame == last_frame:
                # The match ended: its last frame number stays on screen while GAME! animates new bodies, and the host
                # stops tagging presents. Only the first presentation of that frame is the confirmed state.
                if shown:
                    after += 1
                    continue
                shown = True
            tag = by_retrace.get(row['frame'])
            if tag is None or tag['match_frame'] != frame:
                raise ValueError(f'missing/mismatched presented frontier tag: peer-{index} retrace {row["frame"]}')
            if tag['body_frame'] < 0 or tag['body_frame'] > tag['confirmed_frontier']:
                continue # loopback can predict: assert only already-reconciled presentations
            checked += 1
            if any(row[key] != final[frame][key] for key in row if key != 'frame'):
                mismatches.append((index, frame, row['frame']))
        held.append(after)
    return checked, mismatches, held


# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
def proof(out, cancel=False, codes=(0, 0), timed_out=False, delay=None):
    lines = []
    def check(ok, label):
        lines.append(('PASS ' if ok else 'FAIL ') + label)
    logs = []
    counts = []
    for index in range(2):
        work = out / f'peer-{index}'
        logs.append((work / 'host.log').read_text(errors='replace') if (work / 'host.log').exists() else '')
        try:
            rows, _ = load(work / 'digest.csv')
            counts.append(sum(row['match_frame'] > 0 for row in rows))
        except (OSError, ValueError):
            counts.append(0)
    check(not timed_out, 'hard timeout did not fire (only owned child PIDs are killed)')
    check(len(codes) == 2 and all(code == 0 for code in codes), f'process exit codes {list(codes)}')
    for index, log in enumerate(logs):
        check(bool(log), f'peer-{index} host log present')
        check('DESYNC' not in log and DESYNC not in log and GAME_DESYNC not in log,
              f'peer-{index} grep DESYNC = {log.count("DESYNC")}')
    starts = [re.findall(r'squad: P[1-4][^\r\n]*', log) for log in logs]
    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    # Rollbacks legitimately repeat swap diagnostics a different number of times on each peer.
    # Compare the complete unique timeline, retaining frame, port, fighter and costume.
    swaps = [sorted(set(re.findall(r'squad: swap P[1-4] -> \d+:\d+ at frame \d+', log)),
                    key=lambda line: (int(line.rsplit(' ', 1)[1]), line)) for log in logs]
    if cancel:
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        lines.append('TEST cancel leg: B searches with a valid lineup, then the same lineup command rejects '
                     'a new one before connection; the first lock-in after connection refreshes it and '
                     'cancels (unit: squad_transport_fixture, "first lock-in after connection"'
                     + ('; with --delay the cancel waits for A\'s late 0xE3' if delay else '') + ')')
        for index, log in enumerate(logs):
            check(PROTOCOL_ERROR in log, f'peer-{index} squad protocol error')
        if delay:
            check(any('squad: cancel race: late 0xE3 after invalid lineup' in log for log in logs),
                  'cancel raced a late 0xE3 after B invalid lineup')
        # After a failed search the test host falls back to a local debug VS; its rows are not an online match.
        fallback = all(FALLBACK in log for log in logs)
        if fallback and any(counts):
            lines.append(f'INFO local fallback VS after the failed search on both sides: digest rows {counts} (not online)')
        check((counts == [0, 0] or fallback) and not any(starts) and not any(swaps) and
              all('online: match starts,' not in log and 'slippi: online game starts,' not in log for log in logs),
              f'neither side started an online match; digest match rows {counts}')
    else:
        check(all(counts), f'both reached match frames; digest match rows {counts}')
        try:
            result, presented, confirmed = digest_evidence(out)
            (out / 'digest-comparison.json').write_text(json.dumps(result, indent=2) + '\n')
            check(result['compared_frames'] > 0 and not result['confirmed_mismatches'],
                  f'confirmed digests identical by match_frame in both directions ({result["compared_frames"]} frames; '
                  f'{len(result["confirmed_mismatches"])} mismatches)')
            check(result['coverage'] >= 0.95,
                  f'confirmed coverage on BOTH peers: {result["compared_frames"]}/{result["max_match_frame"]} '
                  f'({result["coverage"]:.2%}; require >=95%)')
            lines.append(f'INFO presented rows differing: {result["presented_differing"]} (predicted frames are expected under delay)')
            if not delay:
                checked, mismatches, held = check_presented_confirmed(out, presented, confirmed)
                lines.append(f'INFO rows after the first presentation of the final frame {max(confirmed[0][1])} (GAME!): {held}')
                check(checked > 0 and not mismatches,
                      f'leg-1 presented/confirmed agree for every already-confirmed presentation '
                      f'({checked} rows; {len(mismatches)} mismatches; loopback may predict)')
        except (OSError, ValueError, KeyError) as error:
            check(False, f'confirmed digest evidence ({error})')
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        try:
            for index, (rows, same, beyond, held) in enumerate(late_evidence(out, logs)):
                check(not rows, f'peer-{index} no confirmed body re-simulated with a different state ({len(rows)} late rows'
                                + (f'; first body {rows[0][0]}' if rows else '') + ')')
                check(same is not None and beyond == 0,
                      f'peer-{index} every late resim was checked against its confirmed row ({beyond} beyond the kept window)')
                lines.append(f'INFO peer-{index} late resims with the same state: {same} (selftest rollbacks land here)')
                lines.append(f'INFO peer-{index} new bodies on a held match frame: {held} (startup, GO, GAME!, pause)')
        except (OSError, ValueError, StopIteration) as error:
            check(False, f'late resim evidence ({error})')
        check(len(starts[0]) == 2 and starts[0] == starts[1], 'squad: P<n> start lines identical on both sides')
        check(len(swaps[0]) >= 2 and swaps[0] == swaps[1],
              f'squad: swap lines identical, same order/frames, at least 2 swaps ({len(swaps[0])})')
        if delay:
            evidence = []
            for index, log in enumerate(logs):
                for frame, origin in re.findall(r'squad: swap resimulated at frame (\d+) \(rollback from (\d+)\)', log):
                    if int(origin) <= int(frame) and any(line.endswith('at frame ' + frame) for line in swaps[index]):
                        evidence.append(f'peer-{index} swap frame {frame} resimulated (rollback from {origin})')
            check(bool(evidence), 'rollback-window swap: ' + ('; '.join(sorted(set(evidence))) or 'no evidence'))
        for index, log in enumerate(logs):
            check(REPLAY_SKIP in log, f'peer-{index} {REPLAY_SKIP} (recording on)')
            check(not re.search(r'squad: (?:cancel|.*protocol error|.*match cancelled|malformed lineup rejected)', log),
                  f'peer-{index} no squad: cancel/protocol-error lines')
    (out / 'proof.txt').write_text('\n'.join(lines) + '\n\n' + LIMITS)
    return not any(line.startswith('FAIL ') for line in lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=4200)
    parser.add_argument('--lineup-a', default=DEFAULT_A)
    parser.add_argument('--lineup-b', default=DEFAULT_B)
    parser.add_argument('--stage', type=int, choices=(2, 3, 8, 28, 31, 32), default=2)
    # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
    parser.add_argument('--all', action='store_true', help='full three-leg proof plan in leg-1..3')
    parser.add_argument('--delay', type=delay_spec, help='outgoing input delay on both peers: MS[:JITTER]')
    parser.add_argument('--cancel', action='store_true', help='B submits invalid ck 26 through the lineup command after search starts')
    parser.add_argument('--timeout', type=float, default=180, help='hard wall-clock deadline for the pair, seconds')
    args = parser.parse_args()
    if args.all and (args.delay or args.cancel):
        parser.error('--all selects its own delay/cancel plan')
    if sys.platform != 'linux':
        parser.error('run the two game processes on Linux only')
    if args.frames < 1 or args.timeout <= 0:
        parser.error('--frames and --timeout must be positive')
    args.host, args.iso, args.out = args.host.resolve(), args.iso.resolve(), args.out.resolve()
    if not args.host.is_file() or not args.iso.is_file() or not (args.host.parent / 'Sys').is_dir():
        parser.error('--host, --iso and the host\'s adjacent Sys directory must exist')
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be empty or new (evidence is never overwritten)')
    args.out.mkdir(parents=True, exist_ok=True)
    return run_plan(args)


# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
def run_plan(args, run_leg=None):
    run_leg = run_leg or run_pair
    if not args.all:
        return run_leg(args)
    results, sections = [], []
    for index, (delay, cancel) in enumerate(((None, False), ('60:20', False), ('60:20', True)), 1):
        leg = copy.copy(args)
        leg.all, leg.delay, leg.cancel = False, delay, cancel
        leg.out = args.out / f'leg-{index}'
        leg.out.mkdir()
        code = run_leg(leg)
        results.append(code)
        sections.append(f'LEG {index}: delay={delay or "none"} cancel={cancel}\n' +
                        (leg.out / 'proof.txt').read_text().split('\nLIMITS', 1)[0].rstrip())
    summary = '\n\n'.join(sections) + '\n\n' + LIMITS
    summary += '\n' + ('PASS' if not any(results) else 'FAIL') + ' all three proof legs\n'
    (args.out / 'proof.txt').write_text(summary)
    print(summary, end='')
    return int(any(results))


def run_pair(args):
    children, streams, codes = [], [], []
    timed_out = False
    deadline = time.monotonic() + args.timeout
    try:
        for index in range(2):
            work, environment, command = isolation(args, index)
            print(shlex.join(command), flush=True)
            stream = (work / 'console.log').open('w')
            streams.append(stream)
            children.append(subprocess.Popen(command, cwd=work, env=environment,
                            stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT))
        for child in children:
            try:
                codes.append(child.wait(timeout=max(0, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                timed_out = True
                break
    finally:
        # No killall, process names, process groups or unrelated PIDs.
        for child in children:
            if child.poll() is None:
                child.kill()
            child.wait()
        for stream in streams:
            stream.close()
        codes = [child.returncode for child in children]
        # Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
        passed = proof(args.out, args.cancel, codes, timed_out, args.delay)
    print((args.out / 'proof.txt').read_text(), end='')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
