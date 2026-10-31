"""Anonymous, digest-checked CI inputs and selective upstream runtime extraction."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import urllib.error
import urllib.parse
import urllib.request
import zipfile


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def public_url(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != "https" or parsed.hostname not in
            {"github.com", "raw.githubusercontent.com"} or parsed.username or
            parsed.password or parsed.query or parsed.fragment or parsed.port):
        raise ValueError("input URL must be anonymous HTTPS on github.com or raw.githubusercontent.com, without query/fragment/port")
    return url


def download(url, sha, dest):
    public_url(url)
    if not re.fullmatch(r"[a-fA-F0-9]{64}", sha):
        raise ValueError("expected a 64-character SHA-256")
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists():
        partial = dest.with_suffix(dest.suffix + ".partial")
        try:
            # No Authorization header, GitHub token or credential helper.
            with urllib.request.urlopen(url, timeout=180) as response, partial.open("wb") as out:
                if urllib.parse.urlsplit(response.url).scheme != "https":
                    raise ValueError("download redirected away from HTTPS")
                shutil.copyfileobj(response, out)
            partial.replace(dest)
        except (urllib.error.URLError, OSError):
            partial.unlink(missing_ok=True)
            # Do not expose redirect URLs (which can contain signed query strings).
            raise ValueError("anonymous download failed; check the supplied public URL") from None
    if digest(dest).lower() != sha.lower():
        dest.unlink()
        raise ValueError("SHA-256 mismatch; input deleted")
    print(f"verified {dest.name}: SHA-256 {sha.lower()}")


def extract_runtime(archive, dest):
    """Never extract upstream executables; consume only pc2's runtime inputs."""
    allowed_dirs = {"Sys", "lang", "licenses", "Shaders"}
    with zipfile.ZipFile(archive) as src:
        roots = [PurePosixPath(info.filename).parent for info in src.infolist()
                 if PurePosixPath(info.filename).name == "dxr_pathtrace.dxil"]
        if len(roots) != 1:
            raise ValueError("upstream zip must have exactly one runtime root")
        root = roots[0]
        seen = set()
        for info in src.infolist():
            name = info.filename.replace("\\", "/")
            path = PurePosixPath(name)
            if path.is_absolute() or any(p in {"..", "."} or ":" in p for p in path.parts):
                raise ValueError("unsafe upstream zip path")
            try:
                rel = path.relative_to(root)
            except ValueError:
                continue
            if not rel.parts or info.is_dir():
                continue
            wanted = (len(rel.parts) == 1 and
                      (rel.suffix.lower() == ".dll" or rel.name in {"dxr_pathtrace.dxil", "README.txt"}))
            wanted |= rel.parts[0] in allowed_dirs
            if not wanted:
                continue
            if (rel.suffix.lower() in {".exe", ".dol", ".iso", ".gcm", ".rvz", ".gcz", ".ciso", ".wia", ".wbfs"}
                    or "recomp" in {p.lower() for p in rel.parts}):
                raise ValueError("forbidden input in upstream runtime folders")
            key = rel.as_posix().lower()
            if key in seen:
                raise ValueError("duplicate upstream runtime path")
            seen.add(key)
            target = dest.joinpath(*rel.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with src.open(info) as data, target.open("wb") as out:
                shutil.copyfileobj(data, out)
    for name in (*sorted(allowed_dirs), "dxr_pathtrace.dxil", "README.txt", "sl.interposer.dll", "sl.common.dll"):
        if not (dest / name).exists():
            raise ValueError(f"missing upstream runtime input: {name}")
    print(f"extracted {len(seen)} upstream runtime files; no upstream executables")


def source_patch(path):
    text = path.read_text(encoding="utf-8-sig")
    diffs = re.findall(r"^diff --git (.+)$", text, re.M)
    if not diffs:
        raise ValueError("source patch has no git diffs")
    for line in diffs:
        if (re.search(r"[\\/](GameSetup_gui\.dat|slpCSS\.dat)(?:\"|\s|$)", line, re.I)
                or re.search(r"\.(dol|iso|gcm|ciso|gcz|rvz|wia|wbfs)(?:\"|\s|$)", line, re.I)
                or re.search(r"[\\/](generated(?:_playback|_vanilla)?|recomp)[\\/].*\.(cpp|o|obj|lib|exe)(?:\"|\s|$)", line, re.I)):
            raise ValueError("source patch contains a forbidden disc/menu/translation diff")
    print(f"source patch: {len(diffs)} diffs, no forbidden menu/disc/translation diff")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    dl = sub.add_parser("download")
    dl.add_argument("url_env")
    dl.add_argument("sha_env")
    dl.add_argument("dest", type=Path)
    ex = sub.add_parser("extract-runtime")
    ex.add_argument("archive", type=Path)
    ex.add_argument("dest", type=Path)
    patch = sub.add_parser("check-patch")
    patch.add_argument("path", type=Path)
    spec = sub.add_parser("check-spec")
    spec.add_argument("path", type=Path)
    args = ap.parse_args()
    if args.command == "download":
        download(os.environ[args.url_env], os.environ[args.sha_env], args.dest)
    elif args.command == "extract-runtime":
        extract_runtime(args.archive, args.dest)
    elif args.command == "check-patch":
        source_patch(args.path)
    elif args.command == "check-spec":
        value = json.loads(args.path.read_text(encoding="utf-8-sig"))
        if (not isinstance(value, dict) or not isinstance(value.get("args"), list)
                or not all(isinstance(a, str) for a in value["args"])
                or not isinstance(value.get("net"), str)):
            raise ValueError("launch spec needs an args string array and net string")
        print("launch spec JSON shape verified (contents not logged)")


if __name__ == "__main__":
    main()
