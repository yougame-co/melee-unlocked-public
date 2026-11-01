# Building Melee Unlocked 0.9.89 for YouGame

This source tree corresponds to build 43.1, row `0.9.89-online-test.43.1`.
It is the Windows build on the main listing.
Original project: https://github.com/Hero88go/melee-unlocked. Retain LICENSE,
NOTICE and third-party attribution when redistributing this GPL-3.0-or-later
project. The two pinned submodules are recorded as gitlinks; obtain their public
repositories separately before source preparation. Aurora is not needed for the
native game-library recipe described in docs/build-source-port.md.

## Important prerequisites

This revision's Windows host CMake does not implement `MELEE_SOURCE_ONLY`.
The old 0.9.82 source-only CI recipe does not apply to this tree. Two retained
fact headers in port/generated are insufficient for that Windows host: its
configuration also expects guest_sources.cmake and the other generated units.
Follow README.md and docs/build-source-port.md for preparation from your own
NTSC 1.02 / GALE01 revision 2 disc, including tools/extract_dol.py and
port/recomp/recomp.py where required. Keep all generated game code and disc
inputs outside any published source or package. Do not redistribute them.

This public-source policy also omits game-data containers, precompiled Gecko
payloads and DSP/codehandler binaries. A complete runtime needs resources
obtained or reconstructed lawfully from your own disc. Any VCDIFF files in an
unstaged candidate have not been approved for public distribution. Their
presence blocks publication until reviewed; they are not an approved source
of runtime resources. A source scan passing does not validate reconstruction.

The retained manual CI workflow and tools/ci instructions describe an older
release. They depend on omitted patch inputs and the former source-only CMake
change; do not dispatch them as a build-26 recipe. Use the integrated sources,
not those patches. The commands below describe current source targets, not a
validated disc-free build or a complete packaging recipe.

## Windows x64

Install Python 3, CMake 3.25+, Ninja, Visual Studio 2022 x64 C++ tools and the
Windows SDK/DXC, plus a compatible MinGW-w64 GCC toolchain (GCC 14 or newer).
Use matching AVX2/FMA CPU baselines for the game library and host. Prepare the
pinned decomp sources with the repository's source-preparation tool, as described
in docs/build-source-port.md. Complete the generated-source prerequisites above
before configuring the host. The native game library and host are separate:

```powershell
python tools/prepare_native_sources.py
cmake -S sourceport/game -B build-sourceport-gcc -G Ninja "-DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake" "-DMELEE_MINGW_ROOT=C:/toolchains/mingw64" -DYOUGAME_RANKED_BRANCH=ON -DYOUGAME_AGENT_TEST_HOOK=OFF
cmake --build build-sourceport-gcc --target melee_game --parallel
cmake -S . -B build-review -G "Visual Studio 17 2022" -A x64 -DMELEE_BUILD_EXPERIMENTAL_PORT=ON -DYOUGAME_RANKED_BRANCH=ON -DYOUGAME_AGENT_TEST_HOOK=OFF
cmake --build build-review --config Release --target melee_source --parallel
```

Supply the matching SDKs required by the configured rendering features. See
NOTICE for their separate terms. No Windows compiler or package was validated
as part of the offline public-source staging operation.

## Linux x86-64

The documented baseline is Ubuntu 24.04, GCC/G++ 14, Python 3, CMake 3.25+,
Ninja and pkg-config. Install development packages for SDL2, OpenGL/GLEW,
libusb, X11/Xext, curl, OpenSSL and FFmpeg, plus glslangValidator and spirv-cross.
Prepare the pinned decomp sources first. The full host uses the retained fact
headers; pass them explicitly:

```sh
python3 tools/prepare_native_sources.py
python3 tools/build_linux.py --port-gen port/generated --ranked-branch
```

This driver passes `YOUGAME_RANKED_BRANCH=ON` to both game and host and keeps
automated test hooks off unless requested. Outputs include
build-linux-game/melee_game.so and build-linux/port/linux/full/melee_source.
Without usable fact headers the driver builds the smaller offline host.

## macOS

The experimental host targets macOS 15+, Intel x86-64 with AVX2/FMA or Apple
silicon with Rosetta. Install Xcode command-line tools and Python 3. The scripts
port/mac/build_full.py and port/mac/package_full.py expect SDL2 headers and its
x86-64-compatible dylib in ../deps/sdl2, and glslangValidator and spirv-cross in
../deps/tools/bin. Supply matching fact headers in
build-mac-full/gen/port-generated, and a matching Linux melee_game.so without
stack-protector imports, its melee_game.snapexcl and required runtime resources
in build-mac-input. Build the game library with `YOUGAME_RANKED_BRANCH=ON`.
The Mac script builds the host and bundle, not these dependencies:

```sh
MELEE_GAME_LIBRARY=build-mac-input/melee_game.so bash port/mac/build-full.sh
```

The unsigned result is build-mac-full/MeleeUnlocked.app. No Mac compile or
runtime verification was performed during source staging.

## Running

Supply your own uncompressed Melee NTSC 1.02 ISO using `--iso`, with the matching
game library and lawfully reconstructed runtime resources beside the host.
The public source does not include a disc, DOL, extracted Nintendo assets or a
complete runtime package. The YouGame app supplies its own launch arguments.
Do not infer that build 43.1 is reproducible from this snapshot alone: resource
provenance, missing inputs and platform builds still need verification.
