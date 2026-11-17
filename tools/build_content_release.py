#!/usr/bin/env python3
"""Builds the files of the content release: the parts of the game that do not change with a version.

    python tools/build_content_release.py [--lab build/lab] [--pro-ai release/ProAI] [--out release/content-1]

The content release (tag content-1) holds what the game downloads the first time it is used instead
of carrying it in every update:

  MeleeUnlocked-Lab.zip   the Lab view's 27 fighter packs (host/updater.cpp lab_packs_download)
  MeleeUnlocked-DLSS-RR.zip  NVIDIA's DLSS Ray Reconstruction runtime (host/updater.cpp rr_download)
  <policy>.onnx           Fight a pro's matchup policies trained here (catalog.json "source")

The Lab zip is written the same way every time (sorted names, fixed dates), so its SHA-256 only
changes when a pack does; that hash is kLabSha256 in port/runtime/host/updater.cpp and
package_release.py refuses a release whose packs no longer match it. Prints what to upload.
"""
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTENT_BASE = "https://github.com/hero88go/melee-unlocked/releases/download/content-1/"
LAB_ZIP = "MeleeUnlocked-Lab.zip"
LAB_PACKS = 27
RR_ZIP = "MeleeUnlocked-DLSS-RR.zip"
RR_FILES = {   # kRrFiles in port/runtime/host/updater.cpp
    "sl.dlss_d.dll": "8dff36fd063094eb1f659e8d4dcd52c7423859a9c4a3df3796084c48412d60ac",
    "nvngx_dlssd.dll": "b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee",
}


def lab_zip_bytes_to(path: Path, lab_dir: Path) -> str:
    packs = [lab_dir / f"{i}.lab" for i in range(LAB_PACKS)]
    missing = [p.name for p in packs if not p.is_file()]
    if missing:
        raise SystemExit(f"Lab packs missing in {lab_dir}: {', '.join(missing)} (run tools/build_lab_assets.py)")
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for pack in packs:
            info = zipfile.ZipInfo(pack.name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, pack.read_bytes(), compresslevel=9)
    return hashlib.sha256(path.read_bytes()).hexdigest()


def lab_zip_matches(path: Path, lab_dir: Path) -> bool:
    """The zip's packs are byte for byte the ones in lab_dir."""
    with zipfile.ZipFile(path) as z:
        names = sorted(z.namelist())
        if names != sorted(f"{i}.lab" for i in range(LAB_PACKS)):
            return False
        return all(z.read(f"{i}.lab") == (lab_dir / f"{i}.lab").read_bytes() for i in range(LAB_PACKS))


def pinned_lab_sha() -> str:
    source = (ROOT / "port/runtime/host/updater.cpp").read_text(encoding="utf-8")
    found = re.search(r'kLabSha256 = "([0-9a-f]{64})"', source)
    return found.group(1) if found else ""


def own_policies(catalog: dict) -> list:
    """Policies that are not Phillip's published ones: they download from the content release."""
    return [m for m in catalog["models"]
            if m["kind"] != "imitation" and not str(m.get("source") or "").startswith("https://huggingface.co/")]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lab", type=Path, default=ROOT / "build/lab")
    ap.add_argument("--pro-ai", type=Path, default=ROOT / "release/ProAI")
    ap.add_argument("--out", type=Path, default=ROOT / "release/content-1")
    ap.add_argument("--rr-dir", type=Path, default=ROOT / "build-avx2/port/Release",
                    help="folder holding sl.dlss_d.dll and nvngx_dlssd.dll (the game's build folder)")
    ap.add_argument("--check", action="store_true", help="report only: leave catalog.json as it is")
    args = ap.parse_args()

    lab_zip = args.out / LAB_ZIP
    sha = lab_zip_bytes_to(lab_zip, args.lab)
    pinned = pinned_lab_sha()
    print(f"{lab_zip} ({lab_zip.stat().st_size / 1e6:.1f} MB) sha256 {sha}")
    if pinned != sha:
        print(f"  kLabSha256 in port/runtime/host/updater.cpp is {pinned or 'not set'}: set it to the hash above")

    rr_zip = args.out / RR_ZIP
    with zipfile.ZipFile(rr_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, digest in RR_FILES.items():
            data = (args.rr_dir / name).read_bytes() if (args.rr_dir / name).is_file() else b""
            if hashlib.sha256(data).hexdigest() != digest:
                raise SystemExit(f"{args.rr_dir / name} is missing or is not the build the game checks for (kRrFiles in updater.cpp)")
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data, compresslevel=9)
        z.write(ROOT / "port/third_party/streamline/license.txt", "nvidia-streamline.txt")
    print(f"{rr_zip} ({rr_zip.stat().st_size / 1e6:.1f} MB)")

    catalog_path = args.pro_ai / "catalog.json"
    catalog = json.loads(catalog_path.read_text())
    upload = [lab_zip, rr_zip]
    changed = False
    for model in own_policies(catalog):
        source = args.pro_ai / model["file"]
        if not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != model["sha256"]:
            raise SystemExit(f"Fight a pro policy missing or changed: {source}")
        url = CONTENT_BASE + model["file"]
        if model.get("source") != url:
            model["source"] = url
            changed = True
        upload.append(source)
    if changed and args.check:
        print(f"{catalog_path}: matchup policies do not name the content release yet (run without --check)")
    elif changed:
        catalog_path.write_text(json.dumps(catalog, indent=1) + "\n")
        print(f"{catalog_path}: matchup policies now name the content release as their source")
    print("upload to the content-1 release:")
    for path in upload:
        print(f"  {path}")


if __name__ == "__main__":
    main()
