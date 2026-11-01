"""Apply the released native adaptations to the pinned public Melee submodule."""
import argparse
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BASE = '039c4bf4ca33338c35d21901ad19b7ede19d19ad'


def prepare(decomp, archive=None):
    patch = ROOT / 'sourceport/patches/melee-native.patch'
    def git(*args):
        return subprocess.run(['git', '-C', str(decomp), *args], capture_output=True, text=True)
    if archive and git('rev-parse', '--show-toplevel').stdout.strip() != str(decomp.resolve()):
        subprocess.run(['git', '-C', str(decomp), 'init', '-q'], check=True)
    if git('apply', '--reverse', '--check', str(patch)).returncode == 0:
        print('Native Melee sources are already prepared.')
        return
    if archive:
        # git archives lack .git. Verify both their embedded pinned commit and
        # every extracted file before applying; never accept an arbitrary tree.
        with archive.open('rb') as stream:
            commit = subprocess.run(['git', 'get-tar-commit-id'], stdin=stream,
                                    capture_output=True, text=True)
        if commit.returncode or commit.stdout.strip() != BASE:
            raise SystemExit('Archive is not the pinned Melee commit')
        with tarfile.open(archive) as tree:
            for member in tree.getmembers():
                if member.isdir():
                    continue
                if not member.isfile() or Path(member.name).is_absolute() or '..' in Path(member.name).parts:
                    raise SystemExit('Unsupported archive member')
                path = decomp / member.name
                if not path.is_file() or path.is_symlink() or path.read_bytes() != tree.extractfile(member).read():
                    raise SystemExit('Extracted Melee sources differ from the pinned archive')
    else:
        head = git('rev-parse', 'HEAD')
        if head.returncode or head.stdout.strip() != BASE:
            raise SystemExit('Initialize the pinned sources first: git submodule update --init sourceport/extern/melee')
        status = git('status', '--porcelain', '--untracked-files=no')
        if status.returncode or status.stdout.strip():
            raise SystemExit('Melee sources have local changes. Use a clean checkout before applying the native patch.')
    check = git('apply', '--check', str(patch))
    if check.returncode:
        raise SystemExit(check.stderr)
    result = git('apply', str(patch))
    if result.returncode:
        raise SystemExit(result.stderr)
    print('Prepared the released native Melee sources.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decomp', type=Path, default=ROOT / 'sourceport/extern/melee')
    parser.add_argument('--archive', type=Path, help='pinned git archive used to populate --decomp')
    args = parser.parse_args()
    prepare(args.decomp, args.archive)
