"""Disc-free pc2 verify-package.ps1 checks, plus native PE and delta validation."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unsigned_x64_pe(data):
    require(data[:2] == b"MZ", "missing PE DOS header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    require(data[pe:pe + 4] == b"PE\0\0", "missing PE signature")
    require(struct.unpack_from("<H", data, pe + 4)[0] == 0x8664, "expected x64 native PE")
    optional = pe + 24
    require(struct.unpack_from("<H", data, optional)[0] == 0x20B, "expected PE32+")
    require(struct.unpack_from("<II", data, optional + 112 + 4 * 8) == (0, 0), "native PE must be unsigned")


def verify(path, hook, net, direct):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) == len({n.casefold() for n in names}), "duplicate package path")
        for name in names:
            p = PurePosixPath(name)
            require(not p.is_absolute() and ".." not in p.parts and "\\" not in name and ":" not in name,
                    "unsafe package path")
            require(not re.search(r"\.(iso|gcm|ciso|gcz|rvz|wia|wbfs|nkit\.[a-z0-9]+|dol|dmp|mdmp)$", name, re.I)
                    and not re.search(r"(^|/)(Looks|CosmeticMods|recomp)/", name, re.I)
                    and not re.search(r"(^|/)(GameSetup_gui\.dat|slpCSS\.dat|melee_port(?:_playback)?\.exe)$", name, re.I),
                    "forbidden disc, translation, menu, dump or player file")
            if re.search(r"(^|/)source/[^/]+\.patch$", name, re.I):
                text = archive.read(name).decode("utf-8", errors="replace")
                require(not re.search(r'^diff --git [^\r\n]*[\\/](?:GameSetup_gui\.dat|slpCSS\.dat)"?(?=[ \t\r\n]|$)', text, re.I | re.M),
                        "forbidden menu diff in source patch")
        for name in ("melee_source.exe", "melee_game.dll", "melee_game.snapexcl", "yougame-native.json",
                     "source/yougame-online.patch", "source/README.txt", "README-MeleeUnlocked.txt", "dxr_pathtrace.dxil",
                     "deltas/manifest.json", "deltas/GameSetup_gui.dat.vcdiff", "deltas/slpCSS.dat.vcdiff"):
            require(name in names and archive.getinfo(name).file_size > 0, f"missing/non-root/empty package input: {name}")
        require(sum(PurePosixPath(n).name == "melee_source.exe" for n in names) == 1, "ambiguous executable")
        require(sum(PurePosixPath(n).name == "yougame-native.json" for n in names) == 1, "ambiguous launch spec")
        for directory in ("Sys/", "lang/", "licenses/", "Shaders/"):
            require(any(n.startswith(directory) for n in names), f"missing runtime directory: {directory}")
        spec = json.loads(archive.read("yougame-native.json"))
        require("--vanilla-only" in spec["args"], "vanilla-only missing")
        require(spec["net"] == net, "launch spec net mismatch")
        require(spec.get("direct") is True if direct else "direct" not in spec, "launch spec direct mismatch")
        exe = archive.read("melee_source.exe")
        for needle in (b"agent test: padBack", b"agent-test.json"):
            require((needle in exe) == hook, "staging/production test hook mismatch")
        unsigned_x64_pe(exe)
        unsigned_x64_pe(archive.read("melee_game.dll"))
        manifest = json.loads(archive.read("deltas/manifest.json"))
        entries = manifest["files"]
        require(len(entries) == 2 and {e["target"] for e in entries} == {"GameSetup_gui.dat", "slpCSS.dat"},
                "delta manifest must contain both targets exactly once")
        for entry in entries:
            require(entry["delta"] == entry["target"] + ".vcdiff", "unexpected delta filename")
            data = archive.read("deltas/" + entry["delta"])
            require(len(data) == entry["delta_size"] and hashlib.sha1(data).hexdigest() == entry["delta_sha1"].lower(),
                    "delta size/SHA-1 mismatch")
        inventory = {}
        for name in names:
            with archive.open(name) as stream:
                inventory[name] = "<executable>" if name == "melee_source.exe" else hashlib.file_digest(stream, "sha256").hexdigest()
        print(f"PASS {path.name}: {len(names)} files, native x64 unsigned, hook={hook}, net={net}, direct={direct}")
        return inventory


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("packages", type=Path)
    ap.add_argument("--net", required=True)
    ap.add_argument("--direct", choices=("true", "false"), required=True)
    args = ap.parse_args()
    files = list(args.packages.glob("*.zip"))
    staging = [p for p in files if not p.name.endswith("-prod-win64.zip")]
    prod = [p for p in files if p.name.endswith("-prod-win64.zip")]
    require(len(staging) == len(prod) == 1, "expected exactly one staging and one production zip")
    first = verify(staging[0], True, args.net, args.direct == "true")
    second = verify(prod[0], False, args.net, args.direct == "true")
    require(first == second, "package contents differ outside melee_source.exe")
    print("PASS staging/production file lists and contents match outside melee_source.exe")


if __name__ == "__main__":
    main()
