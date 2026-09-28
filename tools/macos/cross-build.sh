#!/usr/bin/env bash
# Compile-only macOS cross-build from Linux/WSL via osxcross. Metal shaders are not compiled (no xcrun), so the result links but cannot render.
# Usage: tools/macos/cross-build.sh [arm64|x86_64] [Debug|Release]
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:-arm64}"
CONFIG="${2:-Debug}"
OSXCROSS="${OSXCROSS:-$HOME/osxcross/target}"
TRIPLE="$(ls "$OSXCROSS/bin" | grep -E "^${ARCH}-apple-darwin[0-9.]+-cmake$" | head -1 | sed 's/-cmake$//')"
[ -n "$TRIPLE" ] || { echo "no osxcross toolchain for $ARCH in $OSXCROSS/bin" >&2; exit 1; }
export PATH="$OSXCROSS/bin:$PATH"

HOST_TOOLS="${HOST_TOOLS:-$HOME/mac-host-tools}"
FILE_TO_C="$HOST_TOOLS/file_to_c/file_to_c"
SPIRV_CROSS_MSL="$HOST_TOOLS/spirv_cross_msl_src/tools/spirv_cross_msl/build/bin/spirv_cross_msl"
[ -x "$FILE_TO_C" ] && [ -x "$SPIRV_CROSS_MSL" ] || bash "$REPO/tools/macos/build-host-tools.sh" "$HOST_TOOLS"

# Ubuntu's clang ships no Darwin builtins (___isPlatformVersionAtLeast etc.); link osxcross's build_compiler_rt.sh output directly.
CLANG_RT="${CLANG_RT:-$HOME/osxcross/build/compiler-rt/compiler-rt/build/lib/darwin/libclang_rt.osx.a}"
[ -f "$CLANG_RT" ] || { echo "missing $CLANG_RT; run osxcross/build_compiler_rt.sh" >&2; exit 1; }
RT_LINK=(-DCMAKE_EXE_LINKER_FLAGS="$CLANG_RT" -DCMAKE_SHARED_LINKER_FLAGS="$CLANG_RT" -DCMAKE_MODULE_LINKER_FLAGS="$CLANG_RT")

DEPS="$HOME/mac-deps/$ARCH"
SDL_VER=2.32.10
if [ ! -f "$DEPS/lib/cmake/SDL2/SDL2Config.cmake" ]; then
    mkdir -p "$HOME/mac-deps/src"
    cd "$HOME/mac-deps/src"
    [ -d "SDL2-$SDL_VER" ] || curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" | tar xz
    "$TRIPLE-cmake" -S "SDL2-$SDL_VER" -B "sdl-build-$ARCH" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF "${RT_LINK[@]}"
    cmake --build "sdl-build-$ARCH"
    cmake --install "sdl-build-$ARCH"
fi

SUFFIX=""
[ "$CONFIG" = "Debug" ] || SUFFIX="-$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
BUILD="${BUILD_DIR:-$HOME/rs64-build-macos-$ARCH$SUFFIX}"
"$TRIPLE-cmake" -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DCMAKE_PREFIX_PATH="$DEPS" -DCMAKE_FIND_ROOT_PATH="$DEPS" "${RT_LINK[@]}" \
    -DRT64_HOST_FILE_TO_C="$FILE_TO_C" \
    -DRT64_HOST_SPIRV_CROSS_MSL="$SPIRV_CROSS_MSL"
cmake --build "$BUILD" --target RogueSquadron64Recomp -- -k 0
