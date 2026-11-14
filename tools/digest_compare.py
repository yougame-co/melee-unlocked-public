"""Compare 2 or 4 games' per-frame state of one online match, from frame 0, and report the first
frame where they disagree.

    python3 tools/digest_compare.py a.csv b.csv [c.csv d.csv] [--common-only] [--fields p0_,p1_]

Each input is either:
  * a host --state-digest CSV (header "frame,rng,scene,..."). Rows are keyed by match_frame. The
    match's frame 0 is the last match_frame 0 row of the VS scene right before match_frame 1 (the
    counter holds at 0 over several startup frames; the last one is what frame 1 is built on).
    Only the first match in the file is read. The "ram" column is machine-local and never compared.
    These rows are what each machine PRESENTED: a frame shown while a remote input was still
    predicted can differ between two healthy games. A difference here is a lead, not proof;
    confirm it with the trace logs below.
  * a game log with MELEE_TRACE_P2P_PADS=1 lines "state f <frame> sum <hex> seed <hex>: ...",
    keyed by the online frame from 0. A rolled-back frame is logged again; the last line for a
    frame is its final simulation, so the last one is kept. Only the first match in the log is read.

Every input is compared with the first. Exit 0: "identical N frames from frame 0". Exit 1: a
difference (the frame, the input and the field), or one game has frames another lacks (unless
--common-only, which compares only the frames every input has). Exit 2: an unreadable input.
"""
import argparse
import csv
import re
import sys
from pathlib import Path

STATE_LINE = re.compile(r"state f (-?\d+) sum ([0-9A-Fa-f]{8}) seed ([0-9A-Fa-f]{8}):(.*)$")
NEVER_COMPARED = {"frame", "match_frame", "ram"}
ROLLBACK_REACH = 64   # a re-simulation goes back a few frames; a jump back further is a new match


def read_digest_csv(path, text):
    """{match_frame: {field: value}} for the first match in a --state-digest CSV."""
    reader = csv.DictReader(text.splitlines())
    fields = reader.fieldnames or []
    if "match_frame" not in fields or "scene_major" not in fields:
        raise ValueError(f"{path}: no match_frame/scene_major columns (needs a build that writes them)")
    rows = []
    for row in reader:
        if None in row or any(value is None for value in row.values()):
            raise ValueError(f"{path}: malformed row at line {reader.line_num}")
        rows.append(row)
    start = next((i for i, row in enumerate(rows) if int(row["match_frame"], 16) == 1), None)
    if start is None:
        raise ValueError(f"{path}: match_frame never reaches 1, no match to compare")
    frames = {}
    # Frame 0: the last match_frame 0 row of the same scene just before frame 1.
    if start > 0 and int(rows[start - 1]["match_frame"], 16) == 0 and \
            rows[start - 1]["scene_major"] == rows[start]["scene_major"]:
        frames[0] = rows[start - 1]
    for row in rows[start:]:
        match_frame = int(row["match_frame"], 16)
        if match_frame == 0:
            break   # back to the menus: the first match is over
        if frames and match_frame < max(frames):
            break   # the counter went back: another match
        frames[match_frame] = row   # a repeated frame (a stall): the last presentation wins
    compared = [f for f in fields if f not in NEVER_COMPARED]
    return {frame: {f: row[f] for f in compared} for frame, row in frames.items()}


def read_trace_log(path, text):
    """{frame: {field: value}} from MELEE_TRACE_P2P_PADS state lines (the last one per frame)."""
    frames = {}
    for line in text.splitlines():
        match = STATE_LINE.search(line)
        if not match:
            continue
        frame = int(match.group(1))
        if frames and frame < max(frames) - ROLLBACK_REACH:
            break   # far further back than any rollback: the next match in the same log
        values ={"sum": match.group(2).upper(), "seed": match.group(3).upper()}
        # " p0 m 14 x 3F800000 y ... s 4 p1 m ...": per port, key/value pairs named p0_m, p0_x, ...
        port, tokens = "", match.group(4).split()
        index = 0
        while index < len(tokens):
            token = tokens[index]
            if re.fullmatch(r"p\d", token):
                port, index = token, index + 1
                continue
            values[f"{port}_{token}"] = tokens[index + 1] if index + 1 < len(tokens) else ""
            index += 2
        frames[frame] = values
    if not frames:
        raise ValueError(f"{path}: no 'state f N sum ...' lines (run with MELEE_TRACE_P2P_PADS=1)")
    return frames


def read_input(path):
    text = Path(path).read_text(encoding="utf-8-sig", errors="replace")
    first = text.split("\n", 1)[0].strip()
    if first.startswith("frame,"):
        return "csv", read_digest_csv(path, text)
    return "log", read_trace_log(path, text)


def compare(inputs, common_only=False, field_prefixes=None):
    """inputs: [(label, {frame: {field: value}})]. Returns (ok, message)."""
    base_label, base = inputs[0]
    frames = set(base)
    for _, other in inputs[1:]:
        frames &= set(other)
    everything = set(base)
    for _, other in inputs[1:]:
        everything |= set(other)
    if 0 not in frames:
        return False, "frame 0 is missing from " + ", ".join(
            label for label, data in inputs if 0 not in data)
    order = sorted(everything if not common_only else frames)
    compared = 0
    for frame in order:
        if frame not in frames:
            lacking = [label for label, data in inputs if frame not in data]
            return False, (f"identical {compared} frames from frame 0, then frame {frame} is missing "
                           f"from {', '.join(lacking)} (the games ran different lengths; "
                           f"--common-only compares only shared frames)")
        a = base[frame]
        for label, other in inputs[1:]:
            b = other[frame]
            for field in sorted(set(a) | set(b)):
                if field_prefixes and not any(field.startswith(p) for p in field_prefixes):
                    continue
                if a.get(field) != b.get(field):
                    return False, (f"first difference at frame {frame}, field {field}: "
                                   f"{base_label}={a.get(field)} {label}={b.get(field)} "
                                   f"(identical {compared} frames from frame 0 before it)")
        compared += 1
    return True, f"identical {compared} frames from frame 0"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="+", help="2 or 4 --state-digest CSVs or trace logs, one per game")
    parser.add_argument("--common-only", action="store_true",
                        help="compare only the frames every input has (a game that quit a frame early)")
    parser.add_argument("--fields", default="",
                        help="comma-separated field prefixes to compare (default: all)")
    args = parser.parse_args(argv)
    if len(args.inputs) not in (2, 4):
        print("give 2 or 4 inputs (one per game in the match)", file=sys.stderr)
        return 2
    try:
        loaded = [read_input(path) for path in args.inputs]
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2
    kinds = {kind for kind, _ in loaded}
    if len(kinds) != 1:
        print("mix of CSVs and trace logs: give inputs of one kind", file=sys.stderr)
        return 2
    prefixes = [p for p in args.fields.split(",") if p]
    ok, message = compare([(path, data) for path, (_, data) in zip(args.inputs, loaded)],
                          common_only=args.common_only, field_prefixes=prefixes or None)
    print(message)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
