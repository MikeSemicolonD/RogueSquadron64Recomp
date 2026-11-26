#!/usr/bin/env bash
# Sends a file to a remote Linux machine over ssh. A .tar.xz/.tar.gz package is unpacked into <remote-dir> (execute bits intact); a .flatpak is installed
# for the user (saves, settings and mods are kept); anything else (the ROM) is copied into <remote-dir>. Given a folder, the newest .flatpak in it built for
# the remote machine's CPU is installed.
# Usage: tools/release/linux/deploy-remote.sh <file|folder> <user> <host> [remote-dir]   (Windows paths work). ssh asks for the password if no key is set up.
set -euo pipefail
FILE="$1"
USER_AT_HOST="$2@$3"
REMOTE_DIR="${4:-rs64}"
[[ "$FILE" =~ ^[A-Za-z]: ]] && FILE="$(wslpath -a "$FILE")"

# Under WSL, Windows' ssh resolves the LAN's host names and uses the Windows keys; it can't share a connection, so without a key
# the password is asked once per step. Elsewhere one shared connection asks once.
if command -v ssh.exe >/dev/null; then
    SSH=(ssh.exe)
else
    SSH=(ssh -o ControlMaster=auto -o "ControlPath=/tmp/rs64-deploy-%r@%h:%p" -o ControlPersist=60)
fi

if [ -d "$FILE" ]; then
    arch="$("${SSH[@]}" "$USER_AT_HOST" uname -m)"
    FILE="$(find "$FILE" -name "*-$arch.flatpak" -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2-)"
    [ -n "$FILE" ] || { echo "no *-$arch.flatpak under $1 for this $arch machine" >&2; exit 1; }
    echo "installing $FILE"
fi

[ -f "$FILE" ] || { echo "no such file: $FILE" >&2; exit 1; }

if [[ "$FILE" == *.tar.xz || "$FILE" == *.tar.gz ]]; then
    # tar can't detect the compression on a pipe.
    [[ "$FILE" == *.tar.xz ]] && flag=J || flag=z
    "${SSH[@]}" "$USER_AT_HOST" "mkdir -p $REMOTE_DIR && tar x${flag}f - -C $REMOTE_DIR --strip-components=1 && echo 'unpacked into $REMOTE_DIR; run $REMOTE_DIR/run.sh'" < "$FILE"
elif [[ "$FILE" == *.flatpak ]]; then
    "${SSH[@]}" "$USER_AT_HOST" 'f=$(mktemp --suffix=.flatpak) && cat > "$f" && flatpak install --user --reinstall -y "$f"; s=$?; rm -f "$f"; exit $s' < "$FILE"
    echo "installed $(basename "$FILE"); start it from the app menu or with: flatpak run io.github.MikeSemicolonD.RogueSquadron64Recomp"
else
    name="$(basename "$FILE")"
    "${SSH[@]}" "$USER_AT_HOST" "mkdir -p $REMOTE_DIR && cat > $REMOTE_DIR/$name && echo 'copied $name into $REMOTE_DIR'" < "$FILE"
fi
