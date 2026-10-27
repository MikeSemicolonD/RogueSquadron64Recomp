#!/usr/bin/env bash
# Package macOS cross-builds (cross-build.sh) as a zip holding RogueSquadron64Recomp.app, README.txt and LICENSE.txt.
# Two build dirs (arm64 then x86_64) make a universal app with lipo; each slice keeps the ad-hoc signature its linker wrote.
# Not stripped: that would break the signature arm64 macOS needs. The build is not notarized, so players clear the quarantine flag (README).
# Usage: tools/release/macos/package.sh <out.zip> <arm64-build-dir> [x86_64-build-dir]
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$(realpath -m "$1")"
shift
BUILDS=("$@")
LIPO="${OSXCROSS:-$HOME/osxcross/target}/bin/lipo"
ARCHS=(arm64 x86_64)

for b in "${BUILDS[@]}"; do
    [ -x "$b/RogueSquadron64Recomp" ] || { echo "error: exe not found in $b (build it first)" >&2; exit 1; }
done

top="$(basename "$OUT" .zip)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
app="$work/$top/RogueSquadron64Recomp.app/Contents"
mkdir -p "$app/MacOS" "$app/Resources"

# merge <dst> <one file per build>: a plain copy for a single build, else a universal file.
merge() {
    local dst="$1"
    shift
    if [ "$#" = 1 ]; then
        cp "$1" "$dst"
    else
        "$LIPO" -create "$@" -output "$dst"
    fi
}

sdls=()
for i in "${!BUILDS[@]}"; do
    sdls+=("$HOME/mac-deps/${ARCHS[$i]}/lib/libSDL2-2.0.0.dylib")
done
merge "$app/MacOS/RogueSquadron64Recomp" "${BUILDS[@]/%//RogueSquadron64Recomp}"
merge "$app/MacOS/libSDL2-2.0.0.dylib" "${sdls[@]}"

# The game finds Resources/mods through SDL_GetBasePath and copies them to Application Support on launch.
if [ -d "${BUILDS[0]}/mods" ]; then
    cp -r "${BUILDS[0]}/mods" "$app/Resources/mods"
    (cd "${BUILDS[0]}" && find mods -name '*.dylib') | while read -r lib; do
        merge "$app/Resources/$lib" "${BUILDS[@]/%//$lib}"
    done
fi

# favicon.ico's 7th entry is the 256x256 PNG; an icns may hold it as-is under type ic08.
ico="$REPO/favicon.ico"
png_size=$(od -An -t u4 -j $((6 + 16 * 6 + 8)) -N 4 "$ico" | tr -d ' ')
png_offset=$(od -An -t u4 -j $((6 + 16 * 6 + 12)) -N 4 "$ico" | tr -d ' ')
be32() { printf '%08x' "$1" | xxd -r -p; }
{
    printf 'icns'
    be32 $((8 + 8 + png_size))
    printf 'ic08'
    be32 $((8 + png_size))
    tail -c +$((png_offset + 1)) "$ico" | head -c "$png_size"
} > "$app/Resources/rs64.icns"

version="$(head -1 "$REPO/VERSION" | tr -d '\r')"
cat > "$app/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>RogueSquadron64Recomp</string>
    <key>CFBundleIdentifier</key><string>com.rs64recomp.app</string>
    <key>CFBundleName</key><string>Rogue Squadron 64</string>
    <key>CFBundleDisplayName</key><string>Rogue Squadron 64 Recompiled</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>CFBundleIconFile</key><string>rs64.icns</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

cp "$REPO/ci/release-readme.txt" "$work/$top/README.txt"
cp "$REPO/LICENSE" "$work/$top/LICENSE.txt"
cp "$REPO/NOTICES.txt" "$work/$top/NOTICES.txt"
cp -r "$REPO/licenses" "$work/$top/licenses"

rm -f "$OUT"
(cd "$work" && zip -qry "$OUT" "$top")
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)); $("$LIPO" -archs "$app/MacOS/RogueSquadron64Recomp")"
