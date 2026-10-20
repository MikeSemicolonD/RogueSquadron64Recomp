#!/usr/bin/env bash
# Wraps a tools/package-linux.sh tarball in an AppImage.
# Usage: tools/linux/make-appimage.sh <bundle.tar.xz> <x86_64|aarch64> <out.AppImage>
# The AppImage runs from ~/.local/share/RogueSquadron64Recomp (RS64_DATA_DIR overrides): saves, configs and mods live there, and a
# rogue_squadron.z64 next to the AppImage file is linked in on first launch. Vulkan comes from the host, as with the tarball.
set -euo pipefail
TARBALL="$1"
ARCH="$2"
OUT="$3"
TOOL_VERSION=1.9.1
RUNTIME_VERSION=20251108
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/rs64-appimage"
mkdir -p "$CACHE"

host_arch="$(uname -m)"
tool="$CACHE/appimagetool-$TOOL_VERSION-$host_arch.AppImage"
runtime="$CACHE/runtime-$RUNTIME_VERSION-$ARCH"
[ -f "$tool" ] || curl -fsSL -o "$tool" "https://github.com/AppImage/appimagetool/releases/download/$TOOL_VERSION/appimagetool-$host_arch.AppImage"
[ -f "$runtime" ] || curl -fsSL -o "$runtime" "https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-$ARCH"
chmod +x "$tool"

appdir="$(mktemp -d)/RogueSquadron64Recomp.AppDir"
trap 'rm -rf "$(dirname "$appdir")"' EXIT
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" "$appdir/usr/share/rs64"
bundle="$(mktemp -d)"
tar xf "$TARBALL" -C "$bundle" --strip-components=1
mv "$bundle/RogueSquadron64Recomp" "$appdir/usr/bin/"
cp -a "$bundle/lib/." "$appdir/usr/lib/"
[ -d "$bundle/mods" ] && cp -a "$bundle/mods" "$appdir/usr/share/rs64/mods"
cp "$bundle/README.txt" "$appdir/usr/share/rs64/"
cp "$bundle/rs64recomp.png" "$appdir/rs64recomp.png"
mkdir -p "$appdir/usr/share/icons/hicolor/256x256/apps"
cp "$bundle/rs64recomp.png" "$appdir/usr/share/icons/hicolor/256x256/apps/"
rm -rf "$bundle"

cat > "$appdir/AppRun" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
data="${RS64_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/RogueSquadron64Recomp}"
mkdir -p "$data/mods"
# Refresh the shipped mods; mods the user added and per-mod settings (mod_config/) are left alone.
[ -d "$here/usr/share/rs64/mods" ] && cp -a "$here/usr/share/rs64/mods/." "$data/mods/"
rom_beside="$(dirname "${APPIMAGE:-$here}")/rogue_squadron.z64"
[ -e "$data/rogue_squadron.z64" ] || [ ! -f "$rom_beside" ] || ln -s "$rom_beside" "$data/rogue_squadron.z64"
cd "$data"
export LD_LIBRARY_PATH="$here/usr/lib:${LD_LIBRARY_PATH:-}"
exec "$here/usr/bin/RogueSquadron64Recomp" "$@"
EOF
chmod +x "$appdir/AppRun"

cat > "$appdir/rs64recomp.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Rogue Squadron 64 Recompiled
Exec=RogueSquadron64Recomp
Icon=rs64recomp
StartupWMClass=rs64recomp
Categories=Game;
Terminal=false
EOF

ARCH="$ARCH" APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream --runtime-file "$runtime" "$appdir" "$OUT"
echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
