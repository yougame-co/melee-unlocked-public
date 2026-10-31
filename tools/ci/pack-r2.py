"""pc2 packer adapted for CI: stage-extras <stage> <launch-spec> | zip <stage> <zip>.

The yg2 archive dependency is replaced by an explicitly supplied launch spec.
The original delta validation, omission and menu-patch refusal remain intact.
"""
import hashlib, json, re, sys, zipfile
from pathlib import Path
cmd, stage = sys.argv[1], Path(sys.argv[2])
if cmd == "stage-extras":
    spec = Path(sys.argv[3])
    if not isinstance(json.loads(spec.read_text(encoding="utf-8-sig")), dict):
        sys.exit("launch spec must be a JSON object")
    (stage / "yougame-native.json").write_bytes(spec.read_bytes())
elif cmd == "zip":
    out = Path(sys.argv[3])
    if out.exists(): sys.exit(f"{out} exists; not overwriting")
    files = sorted(p for p in stage.rglob("*") if p.is_file())
    # G31: ship patches, never Slippi's two disc-derived Sys assets. Check upstream bytes before omitting them.
    omitted = set()
    manifest = stage / "deltas/manifest.json"
    if manifest.is_file():
        try:
            entries = json.loads(manifest.read_text(encoding="utf-8"))["files"]
            required = {"GameSetup_gui.dat", "slpCSS.dat"}
            if not isinstance(entries, list) or len(entries) != 2 or {f["target"] for f in entries} != required:
                raise ValueError("expected both Sys targets exactly once")
            for f in entries:
                if f["delta"] != f["target"] + ".vcdiff":
                    raise ValueError(f"{f['target']}: unexpected delta filename")
                if not isinstance(f["sys"], list) or not f["sys"] or any(s not in ("Sys", "SysPlayback", "slippi_sys_general") for s in f["sys"]):
                    raise ValueError(f"{f['target']}: invalid Sys folders")
                for kind in ("target", "delta"):
                    if not isinstance(f[kind + "_sha1"], str) or not re.fullmatch(r"[a-fA-F0-9]{40}", f[kind + "_sha1"]):
                        raise ValueError(f"{f['target']}: invalid {kind} SHA-1")
                    if type(f[kind + "_size"]) is not int or f[kind + "_size"] <= 0:
                        raise ValueError(f"{f['target']}: invalid {kind} size")
                delta = stage / "deltas" / f["delta"]
                if not delta.is_file() or delta.stat().st_size != f["delta_size"] or hashlib.sha1(delta.read_bytes()).hexdigest() != f["delta_sha1"].lower():
                    raise ValueError(f"deltas/{f['delta']}: delta size/SHA-1 mismatch or missing; regenerate deltas")
                for folder in f["sys"]:
                    # sysgen also accepts the historical flat Sys layout.
                    for p in (stage / folder / "GameFiles/GALE01" / f["target"], stage / folder / f["target"]):
                        if p.is_file():
                            if p.stat().st_size != f["target_size"] or hashlib.sha1(p.read_bytes()).hexdigest() != f["target_sha1"].lower():
                                raise ValueError(f"{p.relative_to(stage).as_posix()}: upstream target size/SHA-1 changed; regenerate deltas")
                            omitted.add(p)
        except (OSError, ValueError, KeyError, TypeError) as e:
            sys.exit(f"refusing to zip invalid deltas/manifest.json: {e}")
    files = [p for p in files if p not in omitted]
    # Compliance 10-28: never zip a disc image, a built look pack or a cosmetics folder (player files beside a settings file).
    banned = [p for p in files if re.search(r"\.(iso|gcm|ciso|gcz|rvz|wia|wbfs|nkit\.[a-z0-9]+)$", p.name, re.I)
              or p.name.lower() in ("gamesetup_gui.dat", "slpcss.dat", "main.dol", "melee_port.exe", "melee_port_playback.exe")
              or p.suffix.lower() in (".dol", ".dmp", ".mdmp")
              or "recomp" in {part.lower() for part in p.relative_to(stage).parts[:-1]}
              or {"looks", "cosmeticmods"} & {part.lower() for part in p.relative_to(stage).parts[:-1]}]
    if banned: sys.exit(f"refusing to zip disc images or player look/cosmetic files, disc-derived Sys files (deltas required) or dumps: {[p.relative_to(stage).as_posix() for p in banned]}")
    # A deleted file's GIT binary patch contains the entire original. Refuse any diff for these exact basenames.
    menu_diff = re.compile(r'^diff --git [^\r\n]*[\\/](?:GameSetup_gui\.dat|slpCSS\.dat)"?(?=[ \t\r\n]|$)', re.I | re.M)
    for p in files:
        name = p.relative_to(stage).as_posix().replace("\\", "/")
        if re.search(r"(^|/)source/[^/]+\.patch$", name, re.I) and menu_diff.search(p.read_bytes().decode("utf-8", errors="replace")):
            sys.exit(f"refusing to zip source patch containing forbidden Slippi menu diff (including binary hunks): {name}")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in files: z.write(p, p.relative_to(stage).as_posix())
    print("left out disc-derived Sys files:", sorted(p.relative_to(stage).as_posix() for p in omitted) or "none")
    h = hashlib.sha256(out.read_bytes()).hexdigest()
    print(f"zip {out} {out.stat().st_size} bytes sha256 {h} entries {len(files)}")
else:
    sys.exit("expected stage-extras or zip")
