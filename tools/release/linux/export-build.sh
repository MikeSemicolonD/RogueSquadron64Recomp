#!/usr/bin/env bash
# Packages a Linux build tree (x86-64 or arm64) into <dest>/rs64-linux-<arch>.tar.xz. A tarball keeps the execute bits that a copy through a Windows folder loses.
# Usage: tools/release/linux/export-build.sh <build-dir> <dest-dir>   (dest may be a Windows path, e.g. C:\Users\me\Desktop)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="${1/#\~/$HOME}"
DEST="$2"
[[ "$DEST" =~ ^[A-Za-z]: ]] && DEST="$(wslpath -a "$DEST")"
exe="$BUILD/RogueSquadron64Recomp"
[ -x "$exe" ] || { echo "no build at $exe" >&2; exit 1; }
mkdir -p "$DEST"
if file "$exe" | grep -q aarch64; then
    arch=arm64
    export LDD="qemu-aarch64 -L / /lib/ld-linux-aarch64.so.1 --list"
    export STRIP=aarch64-linux-gnu-strip
else
    arch=x64
fi
bash "$REPO/tools/release/package-linux.sh" "$DEST/rs64-linux-$arch.tar.xz" "$BUILD"
