#!/usr/bin/env bash
# Sends a file to a remote Linux machine over ssh. A .tar.xz/.tar.gz package is unpacked into <remote-dir> (execute bits intact); anything else (the ROM) is copied into it.
# Usage: tools/linux/deploy-remote.sh <file> <user> <host> <remote-dir>   (file may be a Windows path). ssh asks for the password if no key is set up.
set -euo pipefail
FILE="$1"
USER_AT_HOST="$2@$3"
REMOTE_DIR="$4"
[[ "$FILE" =~ ^[A-Za-z]: ]] && FILE="$(wslpath -a "$FILE")"
[ -f "$FILE" ] || { echo "no such file: $FILE" >&2; exit 1; }
if [[ "$FILE" == *.tar.xz || "$FILE" == *.tar.gz ]]; then
    # tar can't detect the compression on a pipe.
    [[ "$FILE" == *.tar.xz ]] && flag=J || flag=z
    ssh "$USER_AT_HOST" "mkdir -p $REMOTE_DIR && tar x${flag}f - -C $REMOTE_DIR --strip-components=1 && echo 'unpacked into $REMOTE_DIR; run $REMOTE_DIR/run.sh'" < "$FILE"
else
    name="$(basename "$FILE")"
    ssh "$USER_AT_HOST" "mkdir -p $REMOTE_DIR && cat > $REMOTE_DIR/$name && echo 'copied $name into $REMOTE_DIR'" < "$FILE"
fi
