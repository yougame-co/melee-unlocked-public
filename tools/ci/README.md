This is the manual, unsigned Windows Source package trial for
`yougame-co/melee-unlocked`, initially `yg-ci-windows` at `2f4aa72`
(upstream v0.9.81 plus YouGame). Nothing here commits, pushes, signs, publishes a
release, uploads to R2, or deploys. The workflow has only `workflow_dispatch` and
must never be a required branch check. There is one `windows-latest` job.

**Before the first dispatch:** supply the two reviewed files described in
[inputs/README.md](inputs/README.md), and independently establish the upstream zip
and WinLibs SHA-256 values. Neither the source patch nor the canonical launch spec
was supplied with the reference helpers. No digest or launch schema is invented.
The workflow fails closed if any input is absent, mismatched or forbidden.

Files and changes:

| File | Purpose |
| --- | --- |
| `.github/workflows/yougame-windows-build.yml` | Manual job, minimal permission, input wiring, cache and artifact upload. |
| `port/generated/hle_decls.h` | Exact approved 0.9.81 HLE prototype facts. SHA-256 `65a8b1ea8119d18313601dff2efc55dea50607a6235bd5922bd2e160ea433f42`. |
| `port/generated/guest_symbols.h` | Exact approved symbol/address facts. SHA-256 `32f6aad8794ec63835dabc0be2907686036810b37e37ea4e0fdad26e5947c7f0`. |
| `tools/ci/build-windows.ps1` | Windows step implementation, native exit-code handling, MSVC discovery, staging/prod builds and packaging. |
| `tools/ci/inputs.py` | Anonymous verified downloads, source patch screening, selective runtime extraction; never extracts upstream executables. |
| `tools/ci/pack-r2.py` | Copied pc2 packer; uses an explicit launch spec instead of yg2.zip. Original menu/delta refusal and omission remain; also rejects DOL/recomp inputs. |
| `tools/ci/srcreadme.ps1` | Byte-for-byte copy of pc2's source README writer. CI appends its fork commit and build configuration reference. |
| `tools/ci/verify-package.py` | Disc-free equivalent of pc2 package checks, parameterized for the inputs; also checks unsigned native x64 PE files and delta hashes. |
| `tools/ci/inputs/README.md` | Instructions for the two missing reviewed repository inputs. |
| `tools/ci/README.md` | This handoff, contract, limitations and dispatch commands. |
| `.gitignore` (modified) | Allows only the two approved files in `port/generated`; every other generated file stays ignored. |
| `.gitattributes` (modified) | Preserves the approved headers and supplied patch/spec bytes on Windows. |
| `port/CMakeLists.txt` (modified) | Adds `MELEE_SOURCE_ONLY`; excludes guest/recomp targets and the RAM translator while preserving the normal Slippi Source runtime. |

The Source configuration is:

```text
-G "Visual Studio 17 2022" -A x64
-DMELEE_BUILD_EXPERIMENTAL_PORT=ON
-DMELEE_SOURCE_ONLY=ON
-DMELEE_REQUIRE_STREAMLINE=ON
-DYOUGAME_AGENT_TEST_HOOK=ON       # staging; reconfigure OFF for production
cmake --build ... --config Release --target melee_source --parallel 2
```

`MELEE_NO_SLIPPI` remains OFF: selecting it would change the shipping product.
`MELEE_SOURCE_ONLY` skips `guest_sources.cmake`, guest libraries, generated
translation/function-name/Gecko units, static port/probe/parity targets and the
RAM translator configuration. The existing handwritten
`app/source_guest_boundary.cpp` supplies empty guest/Gecko registries and rejects
PowerPC execution. `hle_decls.h` and `guest_symbols.h` are the only files read
from `port/generated`. No `functions.h`, `function_names.cpp`, `gecko_data.cpp`,
translated units, `main.dol`, ISO, or recompiler invocation is needed.
Existing authored-source generators still run: native replay code metadata from
the bundled Slippi INI tables, the FObj host adapter from tracked public C, and
the Windows SDK DXC shader compiler. Their outputs are build products, never new
committed generated files. Packaging uses the upstream `dxr_pathtrace.dxil`, as pc2 does.

The game DLL uses `sourceport/cmake/mingw-w64-x86_64.cmake` (the reference command
resolves its `../cmake/...` path relative to `sourceport/game`). CI passes an
absolute path. `prepare_native_sources.py` checks public decomp commit
`039c4bf4ca33338c35d21901ad19b7ede19d19ad` and applies
`sourceport/patches/melee-native.patch`; it does not use the disc or make network
requests. Only `sourceport/extern/melee` is initialized, from the hardcoded public
`https://github.com/doldecomp/melee.git`. Aurora is not needed by either target.

Toolchain selection: WinLibs **GCC 15.2.0, MinGW-w64 UCRT 13.0.0, POSIX/SEH,
x86-64, r3**, a pinned GCC >=14 alternative to pc2's GCC 16. URL:

```text
https://github.com/brechtsanders/winlibs_mingw/releases/download/15.2.0posix-13.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-15.2.0-mingw-w64ucrt-13.0.0-r3.7z
```

**This URL's availability and its digest could not be verified offline.** The
supervising session must verify the URL and independently obtain its SHA-256
before dispatch (and correct the pin/cache key if this release is unavailable).
CI checks that supplied digest before extraction/execution, including cache hits.
The cache contains only this `.7z`, keyed by release and digest. It is not a build
artifact and contains no game inputs. VS 2022 is discovered explicitly with
`vswhere` on `windows-latest`; absence of VS 17.x or SDK DXC is a hard failure.
Python is 3.13. No new installed dependency records input or reads other apps.

Exact dispatch input contract (all required; defaults below):

| Input | Type / default | Meaning |
| --- | --- | --- |
| `version` | string / `0.9.81` | Must equal VERSION and the approved header version; currently only 0.9.81. |
| `upstream_zip_sha256` | string / none | 64 hex SHA-256 for the upstream release zip. |
| `build_key` | string / `yg-ci0001` | 1–64 characters, first alphanumeric, then letters/digits/underscore/hyphen. |
| `win_net` | string / `internet` | Launch spec net; lowercase letter then up to 31 lowercase letters/digits/underscore/hyphen. Caller chooses a value supported by YouGame. |
| `win_direct` | boolean / `false` | True adds `direct: true`; false removes `direct`. |
| `source_patch_path` | string / `tools/ci/inputs/yougame-online.patch` | Matching G31-free YouGame v2 patch, repository file. |
| `source_patch_sha256` | string / none | 64 hex SHA-256 of that file, checked before use. |
| `launch_spec_path` | string / `tools/ci/inputs/yougame-native.json` | Canonical yg2 launch spec, repository file. |
| `launch_spec_sha256` | string / none | 64 hex SHA-256 of that file, checked before use. |
| `winlibs_sha256` | string / none | 64 hex SHA-256 of the exact pinned WinLibs `.7z`. |

The two path inputs allow only regular files directly under `tools/ci/inputs/`,
with simple letters/digits/underscore/hyphen basenames and `.patch`/`.json`
extensions. The patch is screened for forbidden diffs and checked with
`git apply --reverse --check`, excluding `.gitignore`, `.gitattributes` and
`port/CMakeLists.txt` because this CI change modifies them. The input patch is
included as source documentation, never applied during the build.

The upstream runtime URL is constructed, not caller-controlled:

```text
https://github.com/hero88go/melee-unlocked/releases/download/v<version>/MeleeUnlocked-<version>-win64.zip
```

Its digest is verified before opening it. Only root DLLs, `dxr_pathtrace.dxil`,
`README.txt`, `Sys/`, `lang/`, `licenses/` and `Shaders/` are extracted. Upstream
EXEs are not extracted or executed. The two known menu files may be present only
as transient upstream inputs: the original packer checks them against the delta
manifest's sizes/SHA-1s and omits them from the output. Other copies are refused.
Only the manifest and the two VCDIFFs enter `deltas/`.

Exact outputs: the `windows-package` job exports `artifact_name`, `artifact_id`,
`artifact_url`, `artifact_digest` from the validation/upload steps. There are no
workflow-call outputs (this is dispatch-only). `artifact_digest` is the Actions
artifact digest, distinct from each package's `.sha256` sidecar.

Successful runs upload one artifact named
`yougame-windows-<version>-<build_key>-<run_id>-<run_attempt>`, retained seven days,
containing `out/` and `logs/`:

```text
out/MeleeUnlocked-YouGame-<version>-<build_key>-win64.zip
out/MeleeUnlocked-YouGame-<version>-<build_key>-win64.zip.sha256
out/MeleeUnlocked-YouGame-<version>-<build_key>-prod-win64.zip
out/MeleeUnlocked-YouGame-<version>-<build_key>-prod-win64.zip.sha256
logs/*.log
logs/audit-staging.json
logs/audit-prod.json
logs/build-record.json
```

Each package has root `melee_source.exe`, `melee_game.dll`,
`melee_game.snapexcl`, upstream runtime DLLs, `dxr_pathtrace.dxil`, `Sys/`, `lang/`,
`licenses/`, `Shaders/`, `README-MeleeUnlocked.txt`, `deltas/`,
`yougame-native.json`, `source/yougame-online.patch`, `source/README.txt`, matching
pc2's layout. Staging and production must have exactly the same file list and
bytes except for `melee_source.exe`. Both newly built PE binaries must be x64 and
unsigned; upstream vendor DLL signatures are preserved.

Job steps and pc2 replacements:

| Job step | Action and reference step |
| --- | --- |
| Checkout fork | `clone` + `patch`: checks out the already integrated fork without persisted credentials/submodules; does not clone or apply the separate YouGame archive. |
| Python 3.13 | pc2's preinstalled `py -3.13`. |
| Validate identity/headers | pc2's source/tag/tree identity guards, adapted to VERSION, build label and exact header SHA-256. |
| Public sources/inputs | `clone`: initializes only the public decomp gitlink; `source-verify`: hashes source inputs and checks patch correspondence; downloads/hash-checks the public runtime instead of pc2's pre-extracted `$Rel`. |
| Cache WinLibs | pc2's persistent preinstalled WinLibs download; archive only, no restore-key fallback. |
| Toolchains | pc2's `$Mingw` / VS paths; verifies/extracts GCC and discovers VS 2022. |
| DLL | `dll`: prepares native public sources, configures Release with MinGW and builds only `melee_game`. Existing DLL image-window/snapshot checks also run as post-build commands. |
| Host staging + prod | `host`: stages Streamline DLLs, requires the complete SDK/runtime, builds only `melee_source` with hook ON, saves staging EXE/map, switches hook OFF and rebuilds in the same tree. |
| Audit/package | `package`: runs existing link-map provenance audits for each variant, stages the pc2 file list, adds checked deltas/source/spec, writes source README, sets vanilla/net/direct and invokes the copied packer twice. |
| Checks/SHA-256 | `verify-package.ps1`: layout, banned files/patch diffs, mandatory deltas, launch flags, hook presence/absence, identical payload outside EXE; adds delta digests and unsigned x64 PE checks. Writes per-zip SHA-256 and moves only checked zips into `out/`. |
| Upload artifact | Replaces pc2's local deliverables with a seven-day Actions artifact; logs are also uploaded after later-step failures. No release/R2/deploy. |

`recomp` is omitted entirely. Neither `extract_dol.py` nor `recomp.py` is run;
neither variant builds `melee_port`, a playback executable, or the unpackaged
launcher. Fixed pc2 hashes/sizes cannot be reused for new compiler outputs.
The old helper's recomp-EXE checks, ISO gameplay/parity checks and comparison to
the unavailable yg2 archive are omitted. The source-archive transfer hash check
becomes a digest check on the actual repository patch/spec inputs. The original
delta/hash/menu checks stay in the packer and also run on each final archive.

No repository/environment secrets are referenced or printed. Private checkout,
cache and artifact actions use GitHub's automatic runner credentials; checkout
has read-only contents permission and does not persist credentials. Public
downloads and submodule initialization use no token. Logs contain command output,
versions, public hashes and build identity; no environment dumps, patch contents,
launch spec contents or credential output. Artifact upload never includes the
checkout, raw inputs, downloaded archives, staging trees, `.dbg`, or generated
code. On failure after validation, logs upload but unverified zips do not.

Offline validation and limitations:

- YAML parsed; dispatch-only and one job confirmed. `actionlint` is not installed.
- Python syntax checked. Approved header copies and the source README writer
  verified byte-for-byte against `ci-ref`.
- CMake configured/generated with the host platform flags simulated on macOS
  and DXC stubbed, including `MELEE_REQUIRE_STREAMLINE=ON` with synthetic DLL
  placeholders. Generated targets include `melee_source` and omit `guest`,
  `melee_port` and the RAM translator. This is a configuration check, not a Windows build.
- Synthetic packaging checks passed for staging/production equality, menu
  omission, DOL/menu-diff refusals, changed upstream menu hashes, corrupted
  deltas, cache SHA-256 verification and selective upstream runtime extraction.
  Fixtures contain only synthetic text/minimal PE headers; no disc data was used.
- No Windows runner, MSVC/MinGW compile/link, PowerShell execution, upstream
  release inventory/digest, WinLibs release URL/digest, real patch correspondence,
  canonical launch spec, artifact upload, cost or runtime/gameplay was verified.
- No network, commit, push or dispatch was performed. No disc input was opened.
  This worktree's existing history was not screened for public release.

The supervising session supplies the two reviewed files and four digests, commits
and pushes. GitHub must have registered this dispatch workflow (normally a copy
on the repository default branch is required before a newly introduced workflow
can be dispatched on another ref). Then run, substituting independently verified
digests (these commands are documentation and have not been executed):

```sh
shasum -a 256 tools/ci/inputs/yougame-online.patch tools/ci/inputs/yougame-native.json

gh workflow run yougame-windows-build.yml \
  --repo yougame-co/melee-unlocked --ref yg-ci-windows \
  -f version=0.9.81 -f build_key=yg-ci0001 \
  -f upstream_zip_sha256=UPSTREAM_ZIP_SHA256 \
  -f win_net=internet -f win_direct=false \
  -f source_patch_path=tools/ci/inputs/yougame-online.patch \
  -f source_patch_sha256=SOURCE_PATCH_SHA256 \
  -f launch_spec_path=tools/ci/inputs/yougame-native.json \
  -f launch_spec_sha256=LAUNCH_SPEC_SHA256 \
  -f winlibs_sha256=WINLIBS_SHA256

gh run list --repo yougame-co/melee-unlocked \
  --workflow yougame-windows-build.yml --branch yg-ci-windows
gh run watch RUN_ID --repo yougame-co/melee-unlocked
gh run download RUN_ID --repo yougame-co/melee-unlocked --dir ./yougame-ci-artifact
```
