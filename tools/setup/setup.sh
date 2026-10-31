#!/usr/bin/env bash
# Builds the game from a clone of this repo: installs the packages (apt), fetches the submodules and the decomp, imports the ROM,
# generates the recompiled code and builds Release. Re-run it after a git pull; finished steps are redone or skipped as needed.
#
# Usage: bash tools/setup/setup.sh [path/to/rom]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DECOMP="$(dirname "$REPO")/rogue_squadron64"
PACKAGES=(build-essential cmake ninja-build git python3 libsdl2-dev libvulkan-dev libgtk-3-dev gcc-mips-linux-gnu binutils-mips-linux-gnu)
ROM_SHA1=ed42eed1ee2db646ff7ef94ba8c5421d164a4f0d

step() { printf '\n== %s\n' "$*"; }
fail() { printf '\nSetup stopped: %s\n' "$*" >&2; exit 1; }

cd "$REPO"

step "Checking packages"

if command -v dpkg-query > /dev/null; then
    missing=()
    arch="$(dpkg --print-architecture)"

    # Multiarch systems list one line per installed architecture.
    for p in "${PACKAGES[@]}"; do
        dpkg-query -W -f='${Status} ${Architecture}\n' "$p" 2> /dev/null | grep -Eqx "install ok installed ($arch|all)" || missing+=("$p")
    done

    if [ ${#missing[@]} -gt 0 ]; then
        echo "Installing ${missing[*]} (needs your password)"
        sudo apt-get update && sudo apt-get install -y "${missing[@]}" || fail "could not install ${missing[*]}"
    fi
else
    for c in cmake ninja git python3 c++; do
        command -v "$c" > /dev/null || fail "$c is not installed"
    done

    command -v mips-linux-gnu-gcc > /dev/null || command -v mips64-elf-gcc > /dev/null || fail "no MIPS GCC (mips-linux-gnu-gcc or mips64-elf-gcc) is installed"
    echo "Not an apt system: the SDL2, Vulkan and GTK 3 development packages must be installed too."
fi

python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' || fail "Python 3.11 or newer is needed"

step "Fetching submodules"
git submodule update --init --recursive || fail "a submodule could not be fetched; try again later, or report it on GitHub"

step "Fetching the decomp"

if [ ! -d "$DECOMP/.git" ]; then
    git clone https://github.com/MikeSemicolonD/rogue_squadron64.git "$DECOMP" || fail "could not clone the decomp into $DECOMP"
fi

step "Importing the ROM"

if [ ! -f rogue_squadron.z64 ] || [ "$(sha1sum < "$DECOMP/rogue_squadron.z64" 2> /dev/null | cut -d' ' -f1)" != "$ROM_SHA1" ]; then
    rom="${1:-}"

    if [ -z "$rom" ]; then
        read -r -p "Path to your Star Wars: Rogue Squadron ROM (USA 1.0; .z64, .v64 or .n64): " rom
    fi

    python3 -I tools/setup/import_rom.py "$rom" || fail "the ROM was not imported"
fi

step "Building the game ELF"
python3 tools/re/make_elf_from_syms.py || fail "make_elf_from_syms.py failed"

step "Building N64Recomp, RSPRecomp and RecompModTool"
cmake -S lib/N64ModernRuntime/N64Recomp -B build-tools -DCMAKE_BUILD_TYPE=Release || fail "configuring the tools failed"
cmake --build build-tools --target N64RecompCLI RSPRecomp RecompModTool -j"$(nproc)" || fail "building the tools failed"

step "Recompiling the game code"
build-tools/N64Recomp rogue_squadron.toml || fail "N64Recomp failed"

# The build fixes these up in place, so they are only generated once.
if [ ! -f build/factor5_ucode/musyx_audio_recompiled.c ]; then
    build-tools/RSPRecomp rsp/factor5_boot_rsp.toml || fail "RSPRecomp failed on the boot microcode"
    build-tools/RSPRecomp rsp/musyx_rsp.toml || fail "RSPRecomp failed on the audio microcode"
fi

step "Building the code mods"
cmake -DPLATFORM=desktop -DRECOMP_MOD_TOOL="$REPO/build-tools/RecompModTool" -P tools/mods/build_code_mods.cmake || fail "building the code mods failed"

step "Building the game (Release)"

if [ -f build/CMakeCache.txt ] && ! grep -qx 'CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config' build/CMakeCache.txt; then
    echo "build/ was configured with another generator; reconfiguring it with Ninja Multi-Config"
    rm -rf build/CMakeCache.txt build/CMakeFiles
fi

cmake -B build -G "Ninja Multi-Config" -DN64RECOMP_EXE="$REPO/build-tools/N64Recomp" || fail "configuring the game failed"
cmake --build build --config Release --target RogueSquadron64Recomp || fail "building the game failed"
cp rogue_squadron.z64 build/Release/

step "Done"
echo "Play with:  cd \"$REPO/build/Release\" && ./RogueSquadron64Recomp"
