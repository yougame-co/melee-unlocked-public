#!/bin/bash
# mk-mac102-src.sh <srcarchive dir> <Mac build id dNNNN>: build 102 Mac corresponding source. HOSTC and LIBC required.
# Uses the proven four-host-file recipe; all checks stay enforced. Run this after the Mac host build is ready.
set -eu
S=${1:?usage: mk-mac102-src.sh <srcarchive dir> <Mac build id dNNNN>}; ID=${2:?usage: mk-mac102-src.sh <srcarchive dir> <Mac build id dNNNN>}
[[ $ID =~ ^d0[0-9]{3}$ ]] || { echo "STOP build id $ID: want dNNNN for build 102"; exit 1; }
[ -d "$S" ] || { echo "STOP no dir $S"; exit 1; }
HOSTC=${HOSTC:?set the build 102 host source commit}; LIBC=${LIBC:?set the build 102 game source commit}
[[ $HOSTC =~ ^[0-9a-f]{40}$ ]] && [[ $LIBC =~ ^[0-9a-f]{40}$ ]] || { echo "STOP HOSTC / LIBC: want full 40-hex shas"; exit 1; }
T=${MACSRC102_TOOLS:?set MACSRC102_TOOLS to the release tools folder}
O=${MACSRC102_OUT:?set MACSRC102_OUT to an output folder}; N=melee-unlocked-$ID-mac-source; D=$O/$N/source
[ -e "$O" ] && { echo "STOP $O exists (move it aside)"; exit 1; }
[ -f "$T/name-guard.py" ] || { echo "STOP no $T/name-guard.py"; exit 1; }
(cd "$S" && shasum -a 256 -c SHA256SUMS) >/dev/null || { echo "STOP srcarchive SHA256SUMS check failed"; exit 1; }
B=$(ls "$S" | grep "^BUILD-mac-$ID" | head -1 || true); [ -n "$B" ] || { echo "STOP no BUILD-mac-$ID* in $S"; exit 1; }
grep -q "Host source: $HOSTC" "$S/$B" || { echo "STOP $B does not name host $HOSTC (HOSTC=<sha> if the Mac host was built from another host tree)"; exit 1; }
grep -q "Library source: $LIBC" "$S/$B" || { echo "STOP $B does not name library $LIBC"; exit 1; }
grep -q '0\.9\.120-online-test\.102' "$S/$B" || { echo "STOP $B does not name 0.9.120-online-test.102"; exit 1; }
mkdir -p "$D"
for f in "$S"/*; do b=$(basename "$f"); case "$b" in *-source.tar.gz|SHA256SUMS) continue;; esac; [ -f "$f" ] || { echo "STOP unexpected non-file $b"; exit 1; }; cp -p "$f" "$D/"; done
[ -f "$D/yougame-mac.patch" ] && [ -f "$D/README.txt" ] || { echo "STOP yougame-mac.patch or README.txt missing"; exit 1; }
# 96 (97 keeps the list; the README check stops if it differs): the patch leaves out four host files (README: "Take them from the host source tarball"). The tarball is not in this package,
# so the four files go in host-files/ exactly as the host tarball has them.
HT=$(ls "$S" | grep -- "-yougame-${HOSTC:0:7}-source.tar.gz$" | head -1 || true); [ -n "$HT" ] || { echo "STOP no host source tarball for ${HOSTC:0:7} in $S"; exit 1; }
HP=${HT%.tar.gz}; mkdir -p "$D/host-files"
for f in port/mac/embed_mac.mm port/mac/embed_mac_state.h port/mac/embed_mac_test.cpp port/tests/yougame_0962_merge_test.py; do
  grep -q -F "$f" "$D/README.txt" || { echo "STOP README does not name $f"; exit 1; }
  mkdir -p "$D/host-files/$(dirname "$f")"; tar -xzOf "$S/$HT" "$HP/$f" > "$D/host-files/$f"; [ -s "$D/host-files/$f" ] || { echo "STOP empty $f"; exit 1; }
done
grep -q -i 'GALE01\|\.iso\b\|\.rvz\b\|\.wia\b\|main\.dol' <(ls "$D") && { echo "STOP disc-looking file"; exit 1; }
HOST=${HOSTC:0:7}
cat > "$D/GENERATED.txt" <<G
Melee Unlocked for YouGame, Mac build $ID (0.9.120-online-test.102): corresponding source.
Host: YouGame Mac host $HOST. Game library: yg-build-102 ${LIBC:0:8} built with the nosp recipe in this folder (no stack protector, for the Mac loader).
yougame-mac.patch applies to upstream Melee Unlocked v0.9.120 (77fdda4b). The $B file has the full build steps; README.txt the licences and notes.
The full host and game trees are the same trees the patch produces; the game tree is also the public source attached to the 102 Windows and Linux builds.
The four host files yougame-mac.patch leaves out (README.txt) are in host-files/, as the host source has them.
No game disc data is included.
G
(cd "$D" && find . -type f ! -name SHA256SUMS ! -name GENERATED.txt | sed 's|^\./||' | sort | xargs shasum -a 256 > SHA256SUMS)
ng() { if python3 -I $T/name-guard.py "$@" > "$LOG" 2>&1; then tail -1 "$LOG"; return 0; fi
       if [ "${NAMEGUARD_ADVISORY:-0}" = 1 ]; then echo "NAME-GUARD ADVISORY (NAMEGUARD_ADVISORY=1): $(tail -1 "$LOG")"; return 0; fi
       grep '^NAME-GUARD HIT' "$LOG" | head -20; echo "STOP name-guard $1: $(tail -1 "$LOG") (full list $LOG)"; exit 1; }
LOG=$O/name-guard-dir.log; ng --dir "$D"
(cd "$O" && COPYFILE_DISABLE=1 tar -czf "$N.tar.gz" "$N")
LOG=$O/name-guard-tar.log; ng --tar "$O/$N.tar.gz"
tar -tzf "$O/$N.tar.gz"
echo "MACSRC102 OK $(shasum -a 256 "$O/$N.tar.gz" | cut -c1-64) $O/$N.tar.gz"
