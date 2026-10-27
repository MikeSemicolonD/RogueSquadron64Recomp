#!/usr/bin/env bash
# Package the Linux build into a stripped, self-contained bundle: the exe plus
# its app-level shared libs and a launcher that sets LD_LIBRARY_PATH. Produces a
# .tar.xz (or .tar.gz, by the output name) that runs on another x86-64 Linux without the -dev packages installed.
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
#   tools/release/package-linux.sh [output.tar.xz] [build-dir]
#   BUILD_DIR / OUT env vars override. Defaults: ~/rs64-build-release, ./rs64-linux.tar.xz
#   LDD / STRIP override ldd and strip for a cross build (tools/release/linux/cross-build.sh).
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-${2:-$HOME/rs64-build-release}}"
OUT="${OUT:-${1:-$PWD/rs64-linux.tar.xz}}"
exe="$BUILD_DIR/RogueSquadron64Recomp"

[ -x "$exe" ] || { echo "error: exe not found at $exe (build it first)" >&2; exit 1; }
STRIP="${STRIP:-strip}"
command -v "$STRIP" >/dev/null || { echo "error: '$STRIP' not found (apt install binutils)" >&2; exit 1; }

# Everything goes under one folder named after the tarball, so extracting it doesn't scatter files.
top="$(basename "$OUT")"
top="${top%.tar.xz}"
top="${top%.tar.gz}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
stage="$work/$top"
mkdir -p "$stage/lib"

cp "$exe" "$stage/RogueSquadron64Recomp"
"$STRIP" "$stage/RogueSquadron64Recomp"

# Copy every resolved dependency except what the host must provide: the glibc core / loader, and the libraries GPU drivers load into the
# process themselves (a bundled older copy shadows the host's and the drivers fail, e.g. Mesa needing a newer libwayland-client).
skip='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libstdc\+\+|libgcc_s|libdrm|libgbm|libexpat|libz|libffi|libudev|libasound|libfontconfig|libfreetype|libharfbuzz)\.so'
skip_family='^(libwayland-|libxcb|libX|libGL|libEGL)'
${LDD:-ldd} "$exe" | awk '/=>/ && $3 ~ /^\// {print $3}' | while read -r so; do
    base="$(basename "$so")"
    [[ "$base" =~ $skip || "$base" =~ $skip_family ]] && continue
    cp -L "$so" "$stage/lib/$base"
    "$STRIP" "$stage/lib/$base" 2>/dev/null || true
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

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# favicon.ico holds one PNG; its directory entry gives the size (byte 14) and offset (byte 18).
ico="$repo/favicon.ico"
png_size=$(od -An -t u4 -j 14 -N 4 "$ico" | tr -d ' ')
png_offset=$(od -An -t u4 -j 18 -N 4 "$ico" | tr -d ' ')
tail -c +$((png_offset + 1)) "$ico" | head -c "$png_size" > "$stage/rs64recomp.png"

case "$(readelf -h "$exe" | sed -n 's/^ *Machine: *//p')" in
    AArch64) arch=arm64 ;;
    ARM) arch=armhf ;;
    *80386*) arch=i386 ;;
    *) arch=x64 ;;
esac

cat > "$stage/install-desktop.sh" <<'EOF'
#!/usr/bin/env bash
# Adds a menu entry and icon for this folder's run.sh (rs64recomp.desktop; the game's window class matches it).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
mkdir -p "$data/applications" "$data/icons/hicolor/256x256/apps"
cp "$here/rs64recomp.png" "$data/icons/hicolor/256x256/apps/rs64recomp.png"
cat > "$data/applications/rs64recomp.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Rogue Squadron 64 Recompiled (@ARCH@)
Exec="$here/run.sh"
Path=$here
Icon=rs64recomp
StartupWMClass=rs64recomp
Categories=Game;
Terminal=false
DESKTOP
echo "installed $data/applications/rs64recomp.desktop"
EOF
sed -i "s/@ARCH@/$arch/g" "$stage/install-desktop.sh"
chmod +x "$stage/install-desktop.sh"
cp "$repo/ci/release-readme.txt" "$stage/README.txt"
cp "$repo/LICENSE" "$stage/LICENSE.txt"
cp "$repo/NOTICES.txt" "$stage/NOTICES.txt"
cp -r "$repo/licenses" "$stage/licenses"
if [ -d "$BUILD_DIR/mods" ]; then
  cp -r "$BUILD_DIR/mods" "$stage/mods"
  find "$stage/mods" -name '*.so' -exec "$STRIP" --strip-unneeded {} +
fi

case "$OUT" in
    *.tar.xz) tar -I 'xz -9e -T0' -cf "$OUT" -C "$work" "$top" ;;
    *) tar czf "$OUT" -C "$work" "$top" ;;
esac
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)); $(ls "$stage/lib" | wc -l) libs bundled"
