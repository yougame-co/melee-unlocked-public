# Building Melee Unlocked for YouGame

This fork combines [Melee Unlocked](https://github.com/Hero88go/melee-unlocked)
with YouGame's launch, controller, window and online integration. It is licensed
under **GPL-3.0-or-later**; retain [LICENSE](LICENSE), [NOTICE](NOTICE) and the
third-party notices when redistributing it.

The commands below target this repository's tree for Melee Unlocked **0.9.82**
(tag `yg-build-b002c`), run from the repository root. It includes the Windows CI
files and the two `port/generated` fact headers the source-only build needs.

## Windows x64

YouGame builds the Source Port with the manual GitHub Actions
[workflow](.github/workflows/yougame-windows-build.yml)
and [PowerShell driver](tools/ci/build-windows.ps1),
using Python 3.13, CMake, 7-Zip, Visual Studio 2022 or newer with x64 C++ tools
and the Windows SDK/DXC, and pinned WinLibs GCC 15.2.0 / MinGW-w64 UCRT 13.0.0
POSIX/SEH r3. The workflow prepares these on its Windows runner and obtains the
matching upstream runtime package; Streamline headers, import library and runtime
are required. See the [CI instructions](tools/ci/README.md).

Install GitHub CLI locally and authenticate to an Actions-enabled copy of this
fork. Before dispatch, independently verify the upstream 0.9.82 Windows release
ZIP and pinned WinLibs archive; set `UPSTREAM_ZIP_SHA256` and `WINLIBS_SHA256`
in the PowerShell environment to their SHA-256 digests. Then run this one command:

```powershell
gh workflow run yougame-windows-build.yml --repo <your-fork> --ref <your-branch> -f version=0.9.82 -f build_key=source-test -f win_net=internet -f win_direct=false -f "upstream_zip_sha256=$env:UPSTREAM_ZIP_SHA256" -f "winlibs_sha256=$env:WINLIBS_SHA256" -f source_patch_path=tools/ci/inputs/yougame-online-0982.patch -f "source_patch_sha256=$((Get-FileHash tools/ci/inputs/yougame-online-0982.patch -Algorithm SHA256).Hash.ToLower())" -f launch_spec_path=tools/ci/inputs/yougame-native.json -f "launch_spec_sha256=$((Get-FileHash tools/ci/inputs/yougame-native.json -Algorithm SHA256).Hash.ToLower())"
```

Download the resulting Actions artifact. It contains unsigned staging and
production ZIPs, checksums and build logs. The host uses `MELEE_SOURCE_ONLY=ON`
and the committed fact headers; **no disc, `main.dol` or static recompilation is
needed to build it**. The production variant disables automated test hooks.

## Linux x86-64

Use Linux (Ubuntu 24.04 is the documented baseline), Python 3, GCC/G++ 14,
CMake 3.25+, Ninja and pkg-config. Install development packages for SDL2,
OpenGL/GLEW, libusb, X11/Xext, curl, OpenSSL and FFmpeg, plus glslangValidator
and spirv-cross. Initialize the pinned `sourceport/extern/melee` submodule and
apply the native adaptations with `tools/prepare_native_sources.py`, as described
in [source preparation](docs/build-source-port.md). Keep the default AVX2/FMA
baseline to match the Windows game library.

The [Linux driver](tools/build_linux.py) builds the game library and full host
using the CI branch's fact headers:

```sh
python3 tools/build_linux.py --port-gen port/generated
```

Outputs include `build-linux-game/melee_game.so` and
`build-linux/port/linux/full/melee_source`. Without the header directory, the
driver builds only the smaller offline host, rather than the full Source Port.

## macOS

The experimental full host targets macOS 15+, Intel x86-64 with AVX2/FMA or
Apple silicon with Rosetta. Install Xcode command-line tools and Python 3.
The [builder](port/mac/build_full.py) and [packager](port/mac/package_full.py)
expect SDL2 headers and its x86-64-compatible dylib in `../deps/sdl2`, and
Mac-compatible `glslangValidator` and `spirv-cross` in `../deps/tools/bin`.
Copy the CI branch's two fact headers into
`build-mac-full/gen/port-generated`. Supply an already built, matching Linux
`melee_game.so` without stack-protector imports, its `melee_game.snapexcl` and
its `Sys` directory (including `GC/dsp_coef.bin`) in `build-mac-input`.
The Mac script builds the host and bundle; it does not build these dependencies.

```sh
MELEE_GAME_LIBRARY=build-mac-input/melee_game.so bash port/mac/build-full.sh
```

The unsigned result is `build-mac-full/MeleeUnlocked.app`.

## Your disc and running the result

Supply your **own uncompressed Melee NTSC 1.02 / GALE01 revision 2 ISO** at
runtime. Keep it outside the repository and pass its location with `--iso`;
there is no required disc filename. The native host reads game data and
reconstructs the omitted menu files from it.

All three Source Port recipes above use existing fact headers and need the disc
only at runtime. Without those headers, the older full Linux/macOS setup and
legacy Windows host setup generate them using `tools/extract_dol.py` and
`port/recomp/recomp.py`; that preparation needs the disc's `main.dol` at build
time. **Static Recomp builds always need `main.dol` at build time.** Do not
redistribute the ISO, extracted DOL or translated game-code output.

Standalone, invoke the built `melee_source.exe`, Linux `melee_source`, or Mac
bundle's `Contents/MacOS/MeleeUnlocked` with `--iso "your-disc.iso"` (the
argument is implemented in [the shared entry point](port/app/main.cpp)). Keep
the matching game library, Sys resources and delta files with the executable;
use the packaged layout for distribution. In YouGame, select your disc in the
app and launch the installed native build; the app supplies the disc, profile,
controller, embedding and session arguments. The Windows package's
`yougame-native.json` defines that launch contract.

To contribute, create a branch against
[this repository](https://github.com/yougame-co/melee-unlocked-public) and open a pull request
with the change and relevant checks. YouGame can build that branch as a test
build before accepting it.
