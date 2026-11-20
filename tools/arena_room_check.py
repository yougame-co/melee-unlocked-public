"""Arena room check: N hidden games in one test room on loopback (--arena-test), through the real
online menus, for several games in a row.

Each game reaches Online Play > Direct and picks a character; from there the room does the rest:
it starts the Direct search between the two members it puts in the ring, locks them in (test rooms
only), and between games moves the lineup by the room's mode. Members who are not fighting get the
live feed and open a viewer window (hidden here) on it. In a match every bot runs off the stage, the
lower member index sooner, so games end in seconds with a known winner.

    py -3.12 tools/arena_room_check.py --bin build-avx2/port/Release --players 3 --out .tmp/room3
           [--frames 9000] [--lag-ms 40] [--base-port 44000]

Checks: everyone joins; games end and the room counts them; the lineup moves as Winner stays says;
every member plays; each viewer recording equals the fighter's own recording of the same game on
every frame the feed delivered; no desync on any fighter. Exit 0 pass, 1 fail, 2 a run broke.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from melee_iso import require_iso
import replay_compare

ROOT = Path(__file__).resolve().parents[1]
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
CODE_HISTORY = '[\n  {\n    "connectCode": "PEER#001",\n    "lastPlayed": ""\n  }\n]'


def bot_script(index, hand=False):
    """Online Play > Direct > pick a character, then in each match: run left (off the stage) from
    a point in every 4-second cycle that comes later for a higher member index. hand: also push the
    character select hand into the left panel, move it down the edge, press A, then B (which a second B would take back the pick)."""
    run_from = 20 + 50 * index
    hand_steps = ["400 sx=-127", "560", "600 sy=-127", "606", "640 sy=-127", "646",
                  "700 A", "706", "900 B", "906"] if hand else []
    return "\n".join([
        "# The online menu's first page has the cursor on Slippi: A opens Slippi's page (Unranked,",
        "# Direct, Teams, Party), one row down is Direct. On the Direct CSS (scene 8) move onto a",
        "# character and pick it. No Start: the room starts the search and locks the pick in.",
        "500 A", "510", "620 sy=-80", "628", "700 A", "710",
        "@scene 8", "60 sx=61 sy=127", "77", "100 A", "110", *hand_steps,
        "@match", "@loop 240", "0", f"{run_from} sx=-127", "238", ""])


def parse_log(path):
    out = {"arena": [], "desync": [], "compared": 0, "mismatched": 0, "crash": None, "connected": 0}
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError:
        return out
    for line in lines:
        if "arena: " in line:
            out["arena"].append(line.split("arena: ", 1)[1].strip())
        if "DESYNC" in line:
            out["desync"].append(line.strip())
        if "online connection successful" in line:
            out["connected"] += 1
        if line.startswith("CRASH:") or "game crash" in line:
            out["crash"] = line.strip()
        m = re.search(r"checksums agree through frame \d+ \((\d+) compared, (\d+) mismatched\)", line)
        if m:
            out["compared"] = max(out["compared"], int(m.group(1)))
            out["mismatched"] = max(out["mismatched"], int(m.group(2)))
    return out


def game_key(path):
    """The Game Start payload's settings and seed: the same in every recording of one game."""
    start, _, _ = replay_compare.parse_slp(path)
    return start[1:0x1A1]


def entry_ulp(original, shown):
    """Differences that are the replay viewer's known entry-animation case (replay_compare.py's
    KNOWN_PREFRAME_ULP, seen with more values here): before frame 0, the height only, one float step.
    Any other field or frame still counts."""
    import struct
    count = 0
    for frame, players in original.items():
        if frame >= 0 or frame not in shown:
            continue
        for key, a in players.items():
            b = shown[frame].get(key)
            if not b:
                continue
            bad = [f for f in replay_compare.FIELDS if f in a and f in b and not replay_compare.same_field(a, b, f)]
            if bad == ["y"]:
                ia = struct.unpack(">I", struct.pack(">f", a["y"]))[0]
                ib = struct.unpack(">I", struct.pack(">f", b["y"]))[0]
                if abs(ia - ib) == 1 and not replay_compare.known_preframe_difference(frame, key, "y", a["y"], b["y"]):
                    count += 1
    return count


def own_processes(marker):
    """PIDs of processes whose command line mentions this run's folder (the viewers the games opened)."""
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*" + marker.replace("'", "''") +
          "*' } | ForEach-Object { $_.ProcessId }")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, creationflags=NO_WINDOW)
    return [int(x) for x in r.stdout.split() if x.strip().isdigit()]


def busy_udp_ports():
    """UDP ports something on this PC already holds. A program bound to 127.0.0.1 on a test port takes
    the loopback packets meant for the game there (PunkBuster's services sit on 44301 and 45301)."""
    r = subprocess.run(["netstat", "-ano", "-p", "udp"], capture_output=True, text=True, creationflags=NO_WINDOW)
    ports = set()
    for line in r.stdout.splitlines():
        m = re.search(r"UDP\s+\S+:(\d+)\s", line)
        if m:
            ports.add(int(m.group(1)))
    return ports


def free_base(start, players):
    """The first base port from `start` (in steps of 1000) whose room links and match ports are all free."""
    busy = busy_udp_ports()
    for base in range(start, 60000 - 1000, 1000):
        wanted = [base + off + i for off in (100, 200) for i in range(players)] + list(range(base + 300, base + 500))
        if not busy.intersection(wanted):
            return base
    return start


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", type=Path, default=ROOT / "build-avx2/port/Release")
    ap.add_argument("--players", type=int, default=3)
    ap.add_argument("--frames", type=int, default=9000)
    ap.add_argument("--lag-ms", type=int, default=0)
    ap.add_argument("--base-port", type=int, default=44000)
    ap.add_argument("--stagger", type=float, default=1.0)
    ap.add_argument("--min-games", type=int, default=3)
    ap.add_argument("--out", type=Path, default=ROOT / ".tmp/arena-room")
    ap.add_argument("--iso")
    ap.add_argument("--hand-test", type=int, default=-1, help="this member pushes its CSS hand into the left panel")
    ap.add_argument("--no-viewer", action="store_true",
                    help="watchers keep the live recording without opening a viewer (rooms of 6+ within the 8-game cap)")
    ap.add_argument("--capture", type=int, default=0,
                    help="screenshots: these members (bit per member) draw at 1280x720 on D3D11 and save a picture every 120 frames")
    ap.add_argument("--compare-only", action="store_true", help="check an earlier run's files again (nothing is started)")
    ap.add_argument("--looks", default="", help="panel look per member, e.g. 0,1,2 (0 corners+cards, 1 card, 2 split, 3 corners, 4 ring)")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[])
    ap.add_argument("--press-start", action="store_true",
                    help="the room does not lock fighters in: each member presses Start on the select itself (MELEE_TEST_ARENA_PRESS_START)")
    args = ap.parse_args()
    iso = require_iso(args.iso)
    exe = (args.bin / "melee_source.exe").resolve()
    if not exe.is_file():
        print("missing", exe)
        return 2
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, MELEE_NO_GC_ADAPTER="1", MELEE_ARENA_KEEP_LIVE="1")
    if args.press_start:
        env["MELEE_TEST_ARENA_PRESS_START"] = "1"
    if args.no_viewer:
        env["MELEE_ARENA_NO_VIEWER"] = "1"
    if args.lag_ms > 0:
        env["MELEE_NET_LAG_MS"] = str(args.lag_ms)
    else:
        env.pop("MELEE_NET_LAG_MS", None)
    if args.compare_only:
        return check(args, out)
    base = free_base(args.base_port, args.players)
    if base != args.base_port:
        print(f"ports near {args.base_port} are taken on this PC; using base {base}")
    procs = []
    for i in range(args.players):
        d = out / f"m{i}"
        user = d / "user"
        user.mkdir(parents=True, exist_ok=True)
        for f in d.rglob("*.slp"):
            f.unlink()
        for name in ("direct-codes.json", "teams-codes.json"):
            (user / name).write_text(CODE_HISTORY)
        looks = [int(x) for x in args.looks.split(",") if x.strip()] if args.looks else []
        if i < len(looks):
            (user / "arena").mkdir(parents=True, exist_ok=True)
            (user / "arena" / "panel.txt").write_text(f"{looks[i]} 0\n")
        script = d / "bot.txt"
        script.write_text(bot_script(i, hand=i == args.hand_test))
        settings = d / "settings.ini"
        shots = bool(args.capture & (1 << i)) or i == args.hand_test   # the panel lives in the overlay
        # ARENA_SETTINGS_EXTRA: more settings lines for every member, "|" between them (tests).
        extra_settings = "".join(x + "\n" for x in os.environ.get("ARENA_SETTINGS_EXTRA", "").split("|") if x.strip())
        settings.write_text("startup 0\nvolume 0\nslippi_spectator 0\n" + ("backend d3d11\n" if shots else "") + extra_settings)
        cmd = [str(exe), "--iso", str(iso), "--hidden", "--volume", "0", "--frames", str(args.frames),
               "--script", str(script), "--replay-dir", str(d), "--log-file", str(d / "port.log"),
               "--user-dir", str(user), "--settings-path", str(settings), "--load-settings", "--time-base", "1",
               "--arena-test", f"{i}/{args.players}/{base}"] + args.extra
        if shots:
            (d / "shots").mkdir(exist_ok=True)
            # ARENA_SHOT_SIZE: the picture-taking members' window (default 1280x720), e.g. 1920x1080 for full HD pictures.
            size = os.environ.get("ARENA_SHOT_SIZE", "1280x720")
            cmd += ["--pc-settings", "--window", size, "--capture", str(d / "shots" / "shot.ppm"), "--capture-every", "120"]
        log = open(d / "log.txt", "w")
        p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        procs.append((p, log))
        print(f"m{i} pid {p.pid}", flush=True)
        time.sleep(args.stagger)
    deadline = time.time() + args.frames / 30 + 120
    for i, (p, log) in enumerate(procs):
        try:
            p.wait(timeout=max(1, deadline - time.time()))
        except subprocess.TimeoutExpired:
            print(f"m{i} timed out, ending pid {p.pid}")
            p.kill()
            p.wait()
        log.close()
    time.sleep(2)
    leftovers = [pid for pid in own_processes(str(out)) if pid != os.getpid()]
    for pid in leftovers:   # viewers still open: our own processes, by id
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True, creationflags=NO_WINDOW)
    if leftovers:
        print(f"ended {len(leftovers)} viewer process(es) still running")

    return check(args, out)


def check(args, out):
    ok = True
    problems = []
    logs = [parse_log(out / f"m{i}" / "log.txt") for i in range(args.players)]
    host = logs[0]["arena"]
    joined = [e for e in host if " joined as " in e]
    ended = [e for e in host if re.match(r"game [0-9A-F]{8} ended", e)]
    revs = [e for e in host if e.startswith("room rev")]
    print(f"host: {len(joined)} joined, {len(ended)} games ended, {len(revs)} room changes")
    for e in ended:
        print("   ", e)
    no_winner = [e for e in ended if e.endswith("winner none")]
    if no_winner:
        problems.append(f"{len(no_winner)} of {len(ended)} games ended with no winner reported")
    if len(joined) != args.players - 1:
        problems.append(f"{len(joined)} of {args.players - 1} members joined")
    if len(ended) < args.min_games:
        problems.append(f"only {len(ended)} games ended (wanted {args.min_games})")
    def ring_of(e):
        m = re.search(r"ring (\d+) (\d+)(?: vs (\d+) (\d+))?", e or "")
        return {int(x) for x in m.groups() if x is not None} - {255} if m else set()
    fought = set()
    for e in revs:
        fought |= ring_of(e)
    print("members who were in the ring:", sorted(fought))
    # Winner stays (the default): after each game with a winner, the winner is still in the ring.
    ids = {"ARENA#0": 0}
    for e in joined:
        m = re.match(r"(ARENA#\d+) joined as (\d+)", e)
        if m:
            ids[m.group(1)] = int(m.group(2))
    plan = os.environ.get("MELEE_ARENA_TEST_PLAN", "")
    if "mode:" not in plan and "doubles" not in plan:
        for k, e in enumerate(host):
            m = re.match(r"game [0-9A-F]{8} ended, winner (ARENA#\d+)", e)
            if not m:
                continue
            after = next((x for x in host[k + 1:] if x.startswith("room rev")), None)
            ring = ring_of(after)
            winner = ids.get(m.group(1))
            if ring and winner is not None and winner not in ring:
                problems.append(f"winner {m.group(1)} left the ring under Winner stays: {after}")
    if len(ended) >= args.players and len(fought) < args.players:
        problems.append(f"only members {sorted(fought)} ever fought")
    for i, lg in enumerate(logs):
        if lg["crash"]:
            problems.append(f"m{i} crashed: {lg['crash']}")
        if lg["desync"] or lg["mismatched"]:
            problems.append(f"m{i} desync: {lg['desync'][:2]} mismatched {lg['mismatched']}")
        print(f"m{i}: {lg['connected']} connections, {lg['compared']} checksums compared, {len(lg['arena'])} arena events")
        for e in lg["arena"]:
            if any(k in e for k in ("refused", "could not", "lost", "gaps", "did not")):
                print(f"    m{i}: {e}")
    # Viewer recordings against the fighters' own.
    fighter_slps = {}
    for i in range(args.players):
        for f in (out / f"m{i}").glob("*.slp"):
            try:
                fighter_slps.setdefault(game_key(f), []).append(f)
            except (OSError, ValueError):
                pass
    viewed = 0
    for i in range(args.players):
        for f in sorted((out / f"m{i}" / "user" / "arena" / "viewer").glob("*.slp")):
            try:
                key = game_key(f)
                live_pre = replay_compare.parse_slp(f)[1]
            except (OSError, ValueError) as exc:
                problems.append(f"m{i} viewer recording {f.name} unreadable: {exc}")
                continue
            refs = fighter_slps.get(key)
            if not refs:
                problems.append(f"m{i} viewer recording {f.name} matches no fighter recording")
                continue
            # Against each fighter's own recording; the sender's is the one the feed carries (the two
            # fighters' files can differ in the pre-game frames' inputs, which no feed changes).
            shown = live_pre
            first, last = min(shown), max(shown)
            result = None
            for ref in refs:
                _, original, _ = replay_compare.parse_slp(ref)
                reference = {fr: p for fr, p in original.items() if first <= fr <= last}
                r = replay_compare.compare_posts(reference, shown, allow_preframe_ulp=True)
                r["entry_ulp"] = entry_ulp(reference, shown)
                r["unexpected_mismatches"] -= min(r["unexpected_mismatches"], r["entry_ulp"])
                if result is None or r["unexpected_mismatches"] < result["unexpected_mismatches"]:
                    result = r
            viewed += 1
            line = (f"m{i} watched {f.name}: frames {first}..{last} of the fighter's {min(original)}..{max(original)}, "
                    f"{result['player_frames']} player-frames, {result['unexpected_mismatches']} mismatches"
                    + (f" ({result['allowed_preframe_mismatches'] + result['entry_ulp']} one-step height differences in the entry animation, "
                       f"a known replay playback difference)" if result['allowed_preframe_mismatches'] or result['entry_ulp'] else ""))
            print(line)
            if result["unexpected_mismatches"] or result["player_frames"] == 0:
                problems.append(line + f" first {result['first']}")
    print(f"{viewed} viewer recordings compared")
    # The live recordings themselves: every frame the feed delivered carries the fighter's own inputs.
    lived = 0
    for i in range(args.players):
        for f in sorted((out / f"m{i}" / "user" / "arena").glob("live-*.slp")):
            try:
                key = game_key(f)
                _, _, fed = replay_compare.parse_slp(f)
            except (OSError, ValueError) as exc:
                problems.append(f"m{i} live recording {f.name} unreadable: {exc}")
                continue
            refs = fighter_slps.get(key)
            if not refs or not fed:
                problems.append(f"m{i} live recording {f.name} matches no fighter recording")
                continue
            first, last = min(fed), max(fed)
            missing = [fr for fr in range(first, last + 1) if fr not in fed]
            differ = None
            for ref in refs:   # the sender's own recording is the one it must equal
                _, _, pre = replay_compare.parse_slp(ref)
                d = [fr for fr in range(first, last + 1) if fr in fed and fed[fr] != pre.get(fr)]
                if differ is None or len(d) < len(differ):
                    differ = d
            lived += 1
            line = (f"m{i} live {f.name}: frames {first}..{last} of the fighter's {min(pre)}..{max(pre)}, "
                    f"{len(missing)} missing, {len(differ)} with other inputs")
            print(line)
            if missing or differ or first != min(pre):
                problems.append(line)
    print(f"{lived} live recordings compared")
    # Viewers that caught up with the live game and held for a frame (their logs say so).
    for i in range(args.players):
        for lg in sorted((out / f"m{i}" / "user" / "arena").glob("live-*.log")):
            text = lg.read_text(errors="replace")
            held = text.count("replay: waiting for frame")
            came = text.count("replay: frame ")
            if held:
                print(f"    m{i} viewer of {lg.stem}: held {held} time(s) for a frame still on its way, {came} came")
            # A game still going when the run stopped leaves its viewer waiting: only a finished
            # game (its live file got its Game End) counts.
            member_log = (out / f"m{i}" / "log.txt")
            finished = member_log.is_file() and f"game {lg.stem[5:]} over:" in member_log.read_text(errors="replace")
            if held > came and finished and "replay: playback finished" not in text:
                problems.append(f"m{i} viewer of {lg.stem} waited for a frame that never came")
    # How close to live the viewers played: each says when it caught up with the live game.
    for i in range(args.players):
        for lg in sorted((out / f"m{i}" / "user" / "arena").glob("live-*.log")):
            caught = [l for l in lg.read_text(errors="replace").splitlines() if "caught up with the live game" in l]
            if caught:
                print(f"    m{i} viewer of {lg.stem}: {caught[0].split('replay: ')[-1]}")
    if args.hand_test >= 0:
        hl = (out / f"m{args.hand_test}" / "log.txt").read_text(errors="replace")
        steps = [l.split("arena panel: ")[-1] for l in hl.splitlines() if "arena panel:" in l]
        print(f"    m{args.hand_test} hand: " + "; ".join(steps))
        if not any("went into the left panel" in l for l in steps):
            problems.append(f"m{args.hand_test}: the hand never went into the panel")
    watchers = args.players > 2   # two members both fight every game: nobody watches
    if watchers and viewed == 0 and lived == 0:
        problems.append("no viewer or live recording to compare")
    if watchers and not args.no_viewer and viewed == 0:
        problems.append("no viewer recording to compare")
    (out / "result.json").write_text(json.dumps({"problems": problems, "ended": ended, "fought": sorted(fought),
                                                 "viewed": viewed}, indent=2))
    for p in problems:
        print("PROBLEM:", p)
    ok = not problems
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
