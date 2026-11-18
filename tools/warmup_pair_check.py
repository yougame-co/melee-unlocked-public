"""Warm-up room, local pair: two hidden Source Port games in a Direct pair on local peering.

Both boot straight into the Direct search (--online-test), play game 1 (bots that run off the stage,
so it ends in seconds), and land back on the online character select still connected. Then each
side plays its part (MELEE_TEST_WARMUP, native_practice.cpp):

    warm   goes to the warm-up room after --enter frames, stays --stay frames in Training, comes
           back, locks in after --lock frames
    wait   locks in after --lock frames and waits (the warm side's Training says "is ready")
    leave  leaves the connection after --lock frames (the warm side's Training says "left")
    idle   does nothing

    python tools/warmup_pair_check.py                                                        # wait + warm
Slot 1 runs off the stage first, so it loses game 1 and (MELEE_TEST_ARENA_AUTOLOCK, as the arena
tests) locks in with the drawn stage as soon as the select comes back; slot 2, the winner, is the one
that can warm up then.
    python tools/warmup_pair_check.py --roles warm,warm --stay 3600                        # both warm up
    python tools/warmup_pair_check.py --roles warm,leave --games 1                          # opponent leaves

Checks per slot: one connection for the whole run (no new search), the warm-up went and came back,
the banners the other side's state should show, both games started, every game's checksums
compared and none mismatched, no crash. All processes are this run's own (ended by PID).
"""
import tempfile
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def bot(index, static=False, pick=17, teams=False):
    # Both engines reach the pair through the real menus (the --online-test harness plays its
    # matches in a scene of its own, with no character select between games): Online Play -> Direct
    # -> pick a character -> Start opens the code entry -> Z takes the suggested code (online_quad
    # writes direct-codes.json) -> Start jumps to OK -> A confirms, as port/scripts/online_teams.txt.
    # The Static Recomp boots into Slippi's online menu (Direct one row down); the Source Port's
    # first page has the cursor on Slippi (A opens its page, Direct one row down; arena_room_check.py).
    # In matches: run left off the stage every four seconds, a little later for slot 1, so game 1
    # ends with a winner in about 16 seconds.
    # Teams (--players 4): two rows down, and slots 3 and 4 first turn their team blue (the hand starts
    # on the HMN button: right onto the team button, A, back), as port/scripts/online_teams_blue.txt.
    run_from = 20 + 50 * index
    blue = teams and index >= 2
    if static:
        menus = ["280 sy=-80", "288"] + (["296 sy=-80", "304"] if teams else []) + ["320 A", "330"] + \
                (["800 sx=80 sy=-8", "809", "812 A", "814", "816 sx=-80 sy=8", "825"] if blue else []) + \
                ["840 sx=61 sy=127", "857", "880 A", "890", "940 START", "950", "1040 Z", "1046", "1080 START", "1086", "1120 A", "1126"]
    else:
        menus = ["500 A", "510", "620 sy=-80", "628"] + (["636 sy=-80", "644"] if teams else []) + ["700 A", "710", "@scene 8"] + \
                (["20 sx=80 sy=-8", "29", "32 A", "34", "36 sx=-80 sy=8", "45"] if blue else []) + \
                ["60 sx=61 sy=127", f"{60 + pick}", "100 A", "110", "160 START", "170", "260 Z", "266", "300 START", "306", "340 A", "346"]
    return "\n".join(menus + ["@match", "@loop 240", "0", f"{run_from} sx=-127", "238", ""])


def parse(path):
    out = {"connected": 0, "starts": 0, "games": [], "warm_to": 0, "warm_back": 0, "warm_arrived": 0,
           "ready_banner": 0, "left_banner": 0, "peer_warming": 0, "peer_back": 0, "cleanups": [], "crash": None,
           "desync": [], "locked": 0, "sparring": [], "refused": [], "bounce": 0}
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError:
        return out
    game = None
    for n, line in enumerate(lines):
        if "slippi: online connection successful" in line:
            out["connected"] += 1
        if "slippi: online game starts" in line:
            out["starts"] += 1
            game = {"compared": 0, "mismatched": 0, "line": n}
            out["games"].append(game)
        m = re.search(r"checksums agree through frame \d+ \((\d+) compared, (\d+) mismatched\)", line)
        if m and game is not None:
            game["compared"], game["mismatched"] = int(m.group(1)), int(m.group(2))
        if "DESYNC" in line:
            out["desync"].append(line.strip())
        if "warm-up: to Training" in line:
            out["warm_to"] += 1
        if "warm-up: back to the online character select" in line:
            out["warm_back"] += 1
        if "warm-up: on the online character select again" in line:
            out["warm_arrived"] += 1
        if "warm-up: the opponent is ready" in line:
            out["ready_banner"] += 1
        if "warm-up: the opponent left" in line:
            out["left_banner"] += 1
        if "went to warm up in Training" in line:
            out["peer_warming"] += 1
        if "is back on the character select" in line:
            out["peer_back"] += 1
        if "slippi: connection cleanup" in line and out["connected"]:   # the main menu's own cleanup at boot is no drop
            out["cleanups"].append((n, line.strip()))
        if "warm-up test: locked in" in line:
            out["locked"] += 1
        if "warm-up: " in line and ("AI plays the CPU" in line or "the CPU is " in line):
            out["sparring"].append(line.split("warm-up: ", 1)[1].strip())
        if "warm-up refused" in line or "warm-up not now" in line:
            out["refused"].append(line.strip())
        if "the opponent left during the warm-up; the player stays on the select" in line:
            out["bounce"] += 1
        if line.startswith("CRASH:") or "game crash" in line:
            out["crash"] = line.strip()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", type=Path, default=ROOT / "build-avx2/port/Release")
    ap.add_argument("--players", type=int, default=2, choices=[2, 4], help="4: a Teams match (two red, two blue)")
    ap.add_argument("--engines", default="native,native", help="slot 1, slot 2 (comma or slash): native|static")
    ap.add_argument("--roles", default="wait,warm", help="slot 1, slot 2 (comma or slash): warm|wait|leave|idle")
    ap.add_argument("--enter", type=int, default=120, help="frames on the select before going")
    ap.add_argument("--stay", type=int, default=7200, help="frames in Training (7200: two minutes)")
    ap.add_argument("--lock", type=int, default=600, help="frames on the select before locking in")
    ap.add_argument("--games", type=int, default=2, help="games that must start (2: game 1 and the rematch)")
    ap.add_argument("--lag-ms", type=int, default=0)
    ap.add_argument("--no-autolock", action="store_true", help="no MELEE_TEST_ARENA_AUTOLOCK: the loser can warm up too, no rematch")
    ap.add_argument("--pro-ai", default="", help="MELEE_PRO_AI_DIR for both slots (no spaces): Fight a pro's files")
    ap.add_argument("--pick", default="17,17", help="frames each slot's bot holds the stick toward the top right on the select (17: Mario)")
    ap.add_argument("--settings", default=str(Path(tempfile.gettempdir()) / "mu-pair-settings.ini"), help="the Source Port slots' settings file (no spaces)")
    ap.add_argument("--shots", default="", help="slot 1 draws the overlay and saves a picture every --shot-every frames here (no spaces)")
    ap.add_argument("--shot-every", type=int, default=120)
    ap.add_argument("--shots-slot", type=int, default=0, help="the slot (0-based) that draws and saves the pictures")
    ap.add_argument("--autolock-after", type=int, default=0,
                    help="the loser's own lock-in waits this many frames on the select (MELEE_TEST_ARENA_AUTOLOCK_AFTER)")
    ap.add_argument("--frames", type=int, default=0, help="0: enough for the run")
    ap.add_argument("--out", default="reports/warmup-pair")
    ap.add_argument("--compare-only", action="store_true")
    args = ap.parse_args()

    n = args.players
    roles = [r.strip() for r in args.roles.replace("/", ",").split(",")]
    if len(roles) != n or any(r not in ("warm", "lr", "wait", "leave", "idle") for r in roles):
        sys.exit(f"--roles takes {n} of warm, wait, leave, idle")
    out = (ROOT / args.out).resolve()
    if not args.compare_only:
        engines = [e.strip() for e in args.engines.replace("/", ",").split(",")]
        if len(engines) < n:
            engines += [engines[-1]] * (n - len(engines))
        exes = [(args.bin / ("melee_port.exe" if e == "static" else "melee_source.exe")).resolve() for e in engines]
        for exe in exes:
            if not exe.is_file():
                sys.exit(f"missing {exe}")
        out.mkdir(parents=True, exist_ok=True)
        scripts = []
        for i in range(n):
            p = out / f"bot{i}.txt"
            picks = [int(x) for x in args.pick.replace("/", ",").split(",")]
            p.write_text(bot(i, engines[i] == "static", picks[i] if i < len(picks) else picks[-1], n == 4))
            scripts.append(str(p))
        frames = args.frames or (2400 + 1800 + args.enter + args.stay + args.lock + 2400 + 1200)
        env = dict(os.environ)
        # The Source Port starts on the online menu (startup 0) with its spectator server off (two
        # games on one PC would both want its port), as arena_room_check.py runs it. The harness splits
        # these words on spaces, so the file sits where the path has none.
        settings = Path(args.settings)
        settings.parent.mkdir(parents=True, exist_ok=True)
        # PAIR_SETTINGS_EXTRA: more settings lines for both slots, "|" between them (tests: warmup_button 0, ...).
        extra_settings = "".join(x + "\n" for x in os.environ.get("PAIR_SETTINGS_EXTRA", "").split("|") if x.strip())
        settings.write_text("startup 0\nvolume 0\nslippi_spectator 0\n" + extra_settings)
        for i, role in enumerate(roles):
            # MELEE_TEST_ARENA_AUTOLOCK: the loser of a game locks in with the drawn stage instead of
            # the stage select (which takes no input in hidden runs, with or without a warm-up).
            env[f"QUAD_SLOT{i}_ENV"] = (f"MELEE_TEST_WARMUP={role}:{args.enter}:{args.stay}:{args.lock}" +
                                        ("" if args.no_autolock else " MELEE_TEST_ARENA_AUTOLOCK=1") +
                                        (f" MELEE_PRO_AI_DIR={args.pro_ai}" if args.pro_ai else "") +
                                        (f" MELEE_TEST_ARENA_AUTOLOCK_AFTER={args.autolock_after}" if args.autolock_after else ""))
            extra = f"--settings-path {settings.as_posix()} --load-settings" if engines[i] != "static" else ""
            if i == args.shots_slot and args.shots:
                # Pictures need the overlay (--pc-settings) and D3D11's capture, as arena_room_check.py does.
                shots = Path(args.shots)
                shots.mkdir(parents=True, exist_ok=True)
                for old in shots.glob("*.ppm"):
                    old.unlink()
                shot_settings = shots / "settings.ini"
                shot_settings.write_text("startup 0\nvolume 0\nslippi_spectator 0\nbackend d3d11\n" + extra_settings)
                extra = (f"--settings-path {shot_settings.as_posix()} --load-settings --pc-settings --window 1280x720 "
                         f"--capture {(shots / 'shot.ppm').as_posix()} --capture-every {args.shot_every}")
            if extra:
                env[f"QUAD_SLOT{i}_EXTRA"] = extra
        cmd = [sys.executable, str(ROOT / "tools/online_quad.py"), "--players", str(n), "--engines", ",".join(str(e) for e in exes),
               "--scripts", ",".join(scripts), "--frames", str(frames), "--out", str(out),
               "--timeout", str(frames / 50 + 120)]
        if args.lag_ms:
            cmd += ["--lag-ms", str(args.lag_ms)]
        print(" ".join(cmd), flush=True)
        subprocess.run(cmd, cwd=ROOT, env=env)

    ok = True
    problems = []
    n = len(roles)
    logs = [parse(out / f"p{i + 1}" / "log.txt") for i in range(n)]
    for i, (role, r) in enumerate(zip(roles, logs)):
        games = ", ".join(f"game {k + 1}: {g['compared']} compared {g['mismatched']} mismatched" for k, g in enumerate(r["games"]))
        print(f"P{i + 1} {role:5} connections {r['connected']}  games started {r['starts']}  warm-up to/back/arrived "
              f"{r['warm_to']}/{r['warm_back']}/{r['warm_arrived']}  banners ready {r['ready_banner']} left {r['left_banner']}  "
              f"opponent warming {r['peer_warming']} back {r['peer_back']}  locked {r['locked']}")
        print(f"     {games}")
        for s in r["sparring"]:
            print(f"     sparring: {s}")
        for s in r["refused"][:3]:
            print(f"     {s}")
        if r["crash"]:
            problems.append(f"P{i + 1} crashed: {r['crash']}")
        if r["desync"]:
            problems.append(f"P{i + 1} desync: {r['desync'][0]}")
        if r["connected"] != 1:
            problems.append(f"P{i + 1} connected {r['connected']} times (one connection for the whole run)")
        want_games = 1 if "leave" in roles or "idle" in roles or args.no_autolock else args.games
        if r["starts"] < want_games:
            problems.append(f"P{i + 1} started {r['starts']} games (wanted {want_games})")
        for k, g in enumerate(r["games"][:want_games]):
            if g["compared"] == 0 or g["mismatched"]:
                problems.append(f"P{i + 1} game {k + 1}: {g['compared']} compared, {g['mismatched']} mismatched")
        # No cleanup before the last game started: the pair stayed connected through the warm-up.
        if r["games"] and len(r["games"]) >= want_games:
            last_start = r["games"][want_games - 1]["line"]
            early = [c for c in r["cleanups"] if c[0] < last_start]
            if early:
                problems.append(f"P{i + 1} the connection was cleaned up before game {want_games}: {early[0][1]}")
        if role in ("warm", "lr"):
            if r["warm_to"] < 1 or r["warm_back"] < 1 or r["warm_arrived"] < 1:
                problems.append(f"P{i + 1} the warm-up did not go and come back ({r['warm_to']}/{r['warm_back']}/{r['warm_arrived']})")
            others = [j for j in range(n) if j != i]
            if n == 2 and roles[others[0]] == "wait" and logs[others[0]]["locked"] and r["ready_banner"] < 1:
                problems.append(f"P{i + 1} never showed that the opponent is ready")
            if any(roles[j] == "leave" for j in others) and r["left_banner"] < 1:
                problems.append(f"P{i + 1} never showed that the opponent left")
            if any(roles[j] in ("warm", "lr") and logs[j]["warm_to"] for j in others) and r["peer_warming"] < 1:
                problems.append(f"P{i + 1} never heard that the other player went to warm up")
        else:
            if any(roles[j] in ("warm", "lr") for j in range(n) if j != i) and role not in ("leave", "idle") and r["peer_warming"] < 1:
                problems.append(f"P{i + 1} never heard that the other player went to warm up")
    for p in problems:
        print("  -", p)
        ok = False
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
