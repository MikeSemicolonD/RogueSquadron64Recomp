#!/usr/bin/env bash
# Builds the host-side RT64 tools (file_to_c, spirv_cross_msl) that a macOS cross-build runs on the Linux host.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${1:-$HOME/mac-host-tools}"
TOOLS="$REPO/lib/rt64/src/tools"

mkdir -p "$OUT"
cmake -S "$TOOLS/file_to_c" -B "$OUT/file_to_c" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/file_to_c"

# spirv_cross_msl's CMakeLists forces its binary dir into the source tree; build it from a copy instead.
rm -rf "$OUT/spirv_cross_msl_src"
mkdir -p "$OUT/spirv_cross_msl_src/tools" "$OUT/spirv_cross_msl_src/contrib"
cp -r "$TOOLS/spirv_cross_msl" "$OUT/spirv_cross_msl_src/tools/"
ln -sfn "$REPO/lib/rt64/src/contrib/spirv-cross" "$OUT/spirv_cross_msl_src/contrib/spirv-cross"
cmake -S "$OUT/spirv_cross_msl_src/tools/spirv_cross_msl" -B "$OUT/spirv_cross_msl" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF
cmake --build "$OUT/spirv_cross_msl"

echo "file_to_c:       $(find "$OUT/file_to_c" -name file_to_c -type f -perm -u+x | head -1)"
echo "spirv_cross_msl: $(find "$OUT/spirv_cross_msl_src" "$OUT/spirv_cross_msl" -name spirv_cross_msl -type f -perm -u+x | head -1)"
