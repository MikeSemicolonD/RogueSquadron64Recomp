#!/usr/bin/env bash
# Cross-builds the Linux game for another architecture from x86-64 Ubuntu 24.04 / WSL, then packages a tarball, an AppImage and (arm64) a Flatpak.
# Usage: tools/release/linux/cross-build.sh <arm64|armhf|i386> [Debug|Release] [out-dir]
# Needs the host's x64 N64Recomp and RecompModTool (N64RECOMP_EXE / RECOMP_MOD_TOOL, default ~/rs64-build) and the packages in docs/building-all-platforms.md.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
ARCH="${1:?usage: cross-build.sh <arm64|armhf|i386> [Debug|Release] [out-dir]}"
CONFIG="${2:-Release}"
OUT_DIR="${3:-$REPO/build/release}"
N64RECOMP_EXE="${N64RECOMP_EXE:-$HOME/rs64-build/N64Recomp}"
RECOMP_MOD_TOOL="${RECOMP_MOD_TOOL:-$HOME/rs64-build/RecompModTool}"

case "$ARCH" in
    arm64) triple=aarch64-linux-gnu;   libdir=aarch64-linux-gnu;   qemu=qemu-aarch64; loader=/lib/ld-linux-aarch64.so.1; appimage_arch=aarch64; flatpak_arch=aarch64 ;;
    armhf) triple=arm-linux-gnueabihf; libdir=arm-linux-gnueabihf; qemu=qemu-arm;     loader=/lib/ld-linux-armhf.so.3;   appimage_arch=armhf;   flatpak_arch= ;;
    i386)  triple=i686-linux-gnu;      libdir=i386-linux-gnu;      qemu=qemu-i386;    loader=/lib/ld-linux.so.2;         appimage_arch=i686;    flatpak_arch= ;;
    *) echo "unknown arch '$ARCH' (arm64, armhf or i386)" >&2; exit 1 ;;
esac
BUILD="${BUILD_DIR:-$HOME/rs64-build-$ARCH-$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')}"

missing=()
for tool in "$triple-g++" "$qemu" mips-linux-gnu-gcc cmake ninja; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
for lib in libSDL2.so libgtk-3.so libvulkan.so; do
    [ -e "/usr/lib/$libdir/$lib" ] || missing+=("/usr/lib/$libdir/$lib")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "missing: ${missing[*]} (one-time setup: docs/building-all-platforms.md)" >&2
    exit 1
fi
[ -x "$N64RECOMP_EXE" ] && [ -x "$RECOMP_MOD_TOOL" ] || { echo "need x64 N64Recomp and RecompModTool (build the WSL Debug tree, or set N64RECOMP_EXE / RECOMP_MOD_TOOL)" >&2; exit 1; }

# RT64 embeds its shaders with file_to_c, which has to run on the build host.
HOST_TOOLS="${HOST_TOOLS:-$HOME/rs64-host-tools}"
FILE_TO_C="$HOST_TOOLS/file_to_c/file_to_c"
if [ ! -x "$FILE_TO_C" ]; then
    cmake -S "$REPO/lib/rt64/src/tools/file_to_c" -B "$HOST_TOOLS/file_to_c" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$HOST_TOOLS/file_to_c"
fi

cmake -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/tools/release/linux/$triple.cmake" \
    -DRT64_HOST_FILE_TO_C="$FILE_TO_C" \
    -DN64RECOMP_EXE="$N64RECOMP_EXE" -DRECOMP_MOD_TOOL="$RECOMP_MOD_TOOL" \
    -DMIPS_GCC=/usr/bin/mips-linux-gnu-gcc -DMIPS_LD=/usr/bin/mips-linux-gnu-ld
cmake --build "$BUILD" --target RogueSquadron64Recomp -j"$(nproc)"

ver="${RS64_VERSION:-v$(head -n1 "$REPO/VERSION" | tr -d '[:space:]')}"
mkdir -p "$OUT_DIR"
tarball="$OUT_DIR/RogueSquadron64Recomp-linux-$ARCH-$ver.tar.xz"
LDD="$qemu -L / $loader --list" STRIP="$triple-strip" bash "$REPO/tools/release/package-linux.sh" "$tarball" "$BUILD"
bash "$REPO/tools/release/linux/make-appimage.sh" "$tarball" "$appimage_arch" "$OUT_DIR/RogueSquadron64Recomp-$ver-$appimage_arch.AppImage"
# flatpak-builder runs target binaries inside its sandbox, where only a binfmt handler registered with the F (fix binary) flag can reach qemu.
binfmt="/proc/sys/fs/binfmt_misc/qemu-$flatpak_arch"
if [ -n "$flatpak_arch" ] && command -v flatpak-builder >/dev/null && ! grep -q '^flags:.*F' "$binfmt" 2>/dev/null; then
    echo "skipping the $flatpak_arch Flatpak: $binfmt lacks the F flag (install qemu-user-static)" >&2
elif [ -n "$flatpak_arch" ] && command -v flatpak-builder >/dev/null; then
    bash "$REPO/tools/release/linux/make-flatpak.sh" "$tarball" "$flatpak_arch" "$OUT_DIR/RogueSquadron64Recomp-$ver-$flatpak_arch.flatpak"
fi
