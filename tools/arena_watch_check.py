"""Arena feed proof on a real match: two hidden games play Direct on local peering, the feed probe
reads fighter 1's spectator server and writes the live recording, and a third hidden game plays that
recording while the match is still going. Passes when the viewer's own recording of what it showed
equals fighter 1's recording on every player-frame the feed delivered.

    py -3.12 tools/arena_watch_check.py --bin build-avx2/port/Release --out .tmp/watch1 [--lag-ms 40]
                                        [--frames 4200] [--mode direct] [--stage 32] [--full-post]

Everything runs hidden. Exit code 0 pass, 1 mismatch, 2 a run did not get that far.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from melee_iso import require_iso
import replay_compare

ROOT = Path(__file__).resolve().parents[1]
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def newest_slp(folder):
    files = sorted(folder.rglob("*.slp"), key=lambda f: f.stat().st_mtime)
    return files[-1] if files else None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", type=Path, default=ROOT / "build-avx2/port/Release")
    ap.add_argument("--out", type=Path, default=ROOT / ".tmp/arena-watch")
    ap.add_argument("--frames", type=int, default=4200)
    ap.add_argument("--lag-ms", type=int, default=0)
    ap.add_argument("--mode", default="direct")
    ap.add_argument("--stage", default="32")
    ap.add_argument("--scripts", default="port/scripts/online_bot.txt,port/scripts/online_bot.txt")
    ap.add_argument("--base-port", type=int, default=43100)
    ap.add_argument("--spectator-port", type=int, default=51531)
    ap.add_argument("--full-post", action="store_true", help="the feed carries every post-frame event")
    ap.add_argument("--timeout", type=float, default=0)
    ap.add_argument("--iso")
    args = ap.parse_args()
    iso = require_iso(args.iso)
    exe = (args.bin / "melee_source.exe").resolve()
    probe = (args.bin / "port_arena_feed_probe.exe").resolve()
    for f in (exe, probe):
        if not f.is_file():
            print(f"missing {f}")
            return 2
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for old in out.rglob("*.slp"):
        old.unlink()
    live = out / "live.slp"
    timeout = args.timeout or args.frames / 40 + 90

    # Settings files on a path without spaces (online_quad splits the slot's extra arguments on them).
    ini_dir = Path(tempfile.mkdtemp(prefix="arena_watch_"))
    (ini_dir / "p1.ini").write_text(f"startup 0\nvolume 0\nslippi_spectator 1\nslippi_spectator_port {args.spectator_port}\n")
    (ini_dir / "p2.ini").write_text("startup 0\nvolume 0\nslippi_spectator 0\n")
    env = dict(os.environ, MELEE_NO_GC_ADAPTER="1")
    env["QUAD_SLOT0_EXTRA"] = f"--settings-path {ini_dir / 'p1.ini'} --load-settings"
    env["QUAD_SLOT1_EXTRA"] = f"--settings-path {ini_dir / 'p2.ini'} --load-settings"
    pair_cmd = [sys.executable, str(ROOT / "tools/online_quad.py"), "--players", "2", "--engines", f"{exe},{exe}",
                "--scripts", args.scripts, "--frames", str(args.frames), "--timeout", str(int(timeout)),
                "--lag-ms", str(args.lag_ms), "--base-port", str(args.base_port), "--out", str(out / "pair"),
                "--extra", "--online-test", args.mode, "--test-stage", args.stage, "--hidden"]
    probe_cmd = [str(probe), "--port", str(args.spectator_port), "--out", str(live), "--seconds", str(int(timeout))]
    if args.full_post:
        probe_cmd.append("--full-post")
    viewer_dir = out / "viewer"
    viewer_dir.mkdir(exist_ok=True)
    viewer_cmd = [str(exe), "--iso", str(iso), "--replay", str(live), "--hidden", "--volume", "0", "--no-music",
                  "--replay-dir", str(viewer_dir), "--card-dir", str(viewer_dir / "card"),
                  "--log-file", str(viewer_dir / "port.log"), "--settings-path", str(viewer_dir / "settings.ini"),
                  "--user-dir", str(viewer_dir / "User"), "--shader-cache", str(viewer_dir / "shadercache")]

    started = time.time()
    with open(out / "pair.txt", "w") as pair_log, open(out / "probe.txt", "w") as probe_log, \
         open(viewer_dir / "stdout.txt", "w") as viewer_log:
        pair = subprocess.Popen(pair_cmd, cwd=ROOT, env=env, stdout=pair_log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        prb = subprocess.Popen(probe_cmd, cwd=out, stdout=probe_log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        # The viewer starts once the live recording has its Game Start and first frames, as a friend's
        # game would when the match begins.
        viewer = None
        viewer_started = None
        while time.time() - started < timeout:
            if live.is_file() and live.stat().st_size > 4096:
                viewer = subprocess.Popen(viewer_cmd, cwd=ROOT, env=dict(os.environ, MELEE_NO_GC_ADAPTER="1"),
                                          stdout=viewer_log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
                viewer_started = time.time() - started
                break
            if prb.poll() is not None:
                break
            time.sleep(0.2)
        codes = {}
        for name, p in (("pair", pair), ("probe", prb), ("viewer", viewer)):
            if p is None:
                codes[name] = None
                continue
            try:
                codes[name] = p.wait(timeout=max(5, timeout + 60 - (time.time() - started)))
            except subprocess.TimeoutExpired:
                p.kill()   # our own process
                p.wait()
                codes[name] = "timeout"

    result = dict(codes=codes, viewer_started_s=viewer_started, lag_ms=args.lag_ms, frames_limit=args.frames, gate_pass=False)
    probe_text = (out / "probe.txt").read_text(errors="replace")
    result["probe"] = [line for line in probe_text.splitlines() if line.startswith("probe:")]
    print("\n".join(result["probe"]))
    code = 2
    try:
        fighter = newest_slp(out / "pair" / "p1")
        shown = newest_slp(viewer_dir)
        if not fighter:
            raise ValueError("fighter 1 wrote no recording")
        if not live.is_file():
            raise ValueError("the probe wrote no live recording")
        if not shown:
            raise ValueError(f"the viewer wrote no recording (exit {codes.get('viewer')}); see {viewer_dir / 'port.log'}")
        _, original, _ = replay_compare.parse_slp(fighter)
        _, fed, fed_pre = replay_compare.parse_slp(live)
        _, actual, _ = replay_compare.parse_slp(shown)
        delivered = sorted(fed_pre)
        if not delivered:
            raise ValueError("the live recording holds no frames")
        # Only frames the feed delivered can be shown; how many of the fighter's frames that is, is
        # reported beside the comparison.
        reference = {f: p for f, p in original.items() if delivered[0] <= f <= delivered[-1]}
        compared = replay_compare.compare_posts(reference, actual)
        result.update(fighter_frames=[min(original), max(original)], delivered=[delivered[0], delivered[-1]],
                      shown=[compared["recorded_first"], compared["recorded_last"]],
                      player_frames=compared["player_frames"], mismatches=compared["mismatches"],
                      first=compared["first"], error=compared.get("error"))
        result["gate_pass"] = bool(compared["gate_pass"]) and compared["player_frames"] > 0
        print(f"fighter recorded frames {min(original)}..{max(original)}; the feed delivered {delivered[0]}..{delivered[-1]}; "
              f"the viewer showed {compared['recorded_first']}..{compared['recorded_last']}")
        print(f"{compared['player_frames']} player-frames compared, {compared['mismatches']} mismatches")
        if compared["first"]:
            print("first difference:", compared["first"])
        if compared.get("error"):
            print(compared["error"])
        code = 0 if result["gate_pass"] else 1
    except (OSError, ValueError) as exc:
        result["error"] = str(exc)
        print("error:", exc)
    (out / "result.json").write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
    print("PASS" if code == 0 else "FAIL")
    return code


if __name__ == "__main__":
    sys.exit(main())
