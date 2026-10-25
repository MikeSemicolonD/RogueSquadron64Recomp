#!/usr/bin/env bash
# Wraps a tools/release/package-linux.sh tarball in a single-file Flatpak bundle on the org.freedesktop.Platform runtime.
# Usage: tools/release/linux/make-flatpak.sh <bundle.tar.xz> <x86_64|aarch64> <out.flatpak>
# Needs flatpak and flatpak-builder (apt); the runtime and SDK are installed per-user from Flathub on first use. aarch64 on an x86-64 host needs qemu-user-binfmt.
# Inside the sandbox the game runs from ~/.var/app/<app id>/data: saves, configs and mods live there, and rogue_squadron.z64 goes there before the first launch.
set -euo pipefail
TARBALL="$(realpath "$1")"
ARCH="$2"
OUT="$(realpath -m "$3")"
APP_ID=io.github.MikeSemicolonD.RogueSquadron64Recomp
RUNTIME_VERSION=26.08

command -v flatpak-builder >/dev/null || { echo "flatpak-builder not found: sudo apt install flatpak flatpak-builder" >&2; exit 1; }
flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak --user install -y --noninteractive --arch="$ARCH" flathub "org.freedesktop.Platform//$RUNTIME_VERSION" "org.freedesktop.Sdk//$RUNTIME_VERSION"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
ext="${TARBALL##*.tar.}"
cp "$TARBALL" "$work/bundle.tar.$ext"

cat > "$work/rs64recomp.sh" <<EOF
#!/bin/sh
set -e
cd "\$XDG_DATA_HOME"
mkdir -p mods
# Refresh the shipped mods; mods the user added and mod_config/ are left alone.
cp -a /app/lib/rs64/mods/. mods/
if [ ! -e rogue_squadron.z64 ] && [ ! -e rs64.n64.us.1.0.z64 ]; then
    echo "Copy rogue_squadron.z64 into \$XDG_DATA_HOME (~/.var/app/$APP_ID/data) and start the game again." >&2
fi
# Wayland matches the window to $APP_ID.desktop for its icon.
export SDL_VIDEO_WAYLAND_WMCLASS=$APP_ID SDL_VIDEO_X11_WMCLASS=$APP_ID
export LD_LIBRARY_PATH=/app/lib/rs64/lib
exec /app/lib/rs64/RogueSquadron64Recomp "\$@"
EOF

cat > "$work/$APP_ID.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Rogue Squadron 64 Recompiled
Exec=rs64recomp
Icon=$APP_ID
StartupWMClass=$APP_ID
Categories=Game;
Terminal=false
EOF

cat > "$work/$APP_ID.yml" <<EOF
app-id: $APP_ID
runtime: org.freedesktop.Platform
runtime-version: '$RUNTIME_VERSION'
sdk: org.freedesktop.Sdk
command: rs64recomp
finish-args:
  # SDL2 tries X11 first; on Wayland alone it loads libdecor's GTK plugin, which crashes on the game's render-thread window.
  - --socket=x11
  - --socket=wayland
  - --share=ipc
  - --device=all
  - --socket=pulseaudio
  - --share=network
modules:
  - name: rs64recomp
    buildsystem: simple
    build-commands:
      - mkdir -p /app/lib/rs64
      - cp -a RogueSquadron64Recomp lib mods README.txt /app/lib/rs64/
      # The runtime's own copies are newer and are what its GPU drivers expect.
      - for f in /app/lib/rs64/lib/*; do if [ -e "/usr/lib/\${FLATPAK_ARCH}-linux-gnu/\$(basename "\$f")" ]; then rm "\$f"; fi; done
      - install -Dm755 rs64recomp.sh /app/bin/rs64recomp
      - install -Dm644 rs64recomp.png /app/share/icons/hicolor/256x256/apps/$APP_ID.png
      - install -Dm644 $APP_ID.desktop /app/share/applications/$APP_ID.desktop
    sources:
      - type: archive
        path: bundle.tar.$ext
      - type: file
        path: rs64recomp.sh
      - type: file
        path: $APP_ID.desktop
EOF

flatpak-builder --user --arch="$ARCH" --force-clean --disable-rofiles-fuse --repo="$work/repo" --state-dir="$work/state" "$work/build" "$work/$APP_ID.yml"
flatpak build-bundle --arch="$ARCH" --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo "$work/repo" "$OUT" "$APP_ID"
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)); install with: flatpak install --user $(basename "$OUT")"
