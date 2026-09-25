#!/usr/bin/env bash
# Package the Linux build into a stripped, self-contained bundle: the exe plus
# its app-level shared libs and a launcher that sets LD_LIBRARY_PATH. Produces a
# .tar.gz that runs on another x86-64 Linux without the -dev packages installed.
#
# NOT bundled (must come from the target host):
#   - glibc core + dynamic loader (libc/libm/libpthread/librt/ld-linux): mixing a
#     bundled glibc with the host loader breaks. The bundle therefore needs the
#     target's glibc >= the build host's (Ubuntu 24.04 = glibc 2.39).
#   - The Vulkan loader + GPU driver ICD: RT64 dlopen()s libvulkan at runtime and
#     the ICD is tied to the host GPU driver. Target needs a working Vulkan setup.
#   - rogue_squadron.z64: copyrighted ROM. Drop it next to run.sh to play.
#
# Usage:
#   tools/package-linux.sh [output.tar.gz] [build-dir]
#   BUILD_DIR / OUT env vars override. Defaults: ~/rs64-build-release, ./rs64-linux.tar.gz
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-${2:-$HOME/rs64-build-release}}"
OUT="${OUT:-${1:-$PWD/rs64-linux.tar.gz}}"
exe="$BUILD_DIR/RogueSquadron64Recomp"

[ -x "$exe" ] || { echo "error: exe not found at $exe (build it first)" >&2; exit 1; }
command -v strip >/dev/null || { echo "error: 'strip' not found (apt install binutils)" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/lib"

cp "$exe" "$stage/RogueSquadron64Recomp"
strip "$stage/RogueSquadron64Recomp"

# Copy every resolved dependency except the glibc core / loader (host-provided).
skip='^(ld-linux|libc|libm|libdl|libpthread|librt|libresolv)\.so'
ldd "$exe" | awk '/=>/ && $3 ~ /^\// {print $3}' | while read -r so; do
    base="$(basename "$so")"
    [[ "$base" =~ $skip ]] && continue
    cp -L "$so" "$stage/lib/$base"
    strip "$stage/lib/$base" 2>/dev/null || true
done

cat > "$stage/run.sh" <<'EOF'
#!/usr/bin/env bash
# Launch RogueSquadron64Recomp with the bundled libraries.
# Needs rogue_squadron.z64 in this directory and a working host Vulkan driver.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$here/lib:${LD_LIBRARY_PATH:-}"
exec "$here/RogueSquadron64Recomp" "$@"
EOF
chmod +x "$stage/run.sh"

tar czf "$OUT" -C "$stage" .
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)); $(ls "$stage/lib" | wc -l) libs bundled"
