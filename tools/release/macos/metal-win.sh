#!/usr/bin/env bash
# Runs Apple's Metal Developer Tools for Windows for RT64_METAL / RT64_METALLIB: through Windows on WSL, through Wine elsewhere (or with METAL_USE_WINE=1).
# Usage: metal-win.sh metal|metallib <args...>
set -euo pipefail
TOOLS="${METAL_TOOLS_DIR:-/mnt/c/Program Files/Metal Developer Tools/bin}"
tool="$1"
shift
target=()
[ "$tool" = metal ] && target=(-target "air64-apple-macos${MACOS_MIN:-11.0}")

if [ -e /proc/sys/fs/binfmt_misc/WSLInterop ] && [ "${METAL_USE_WINE:-0}" != 1 ]; then

    # The tools can't open \\wsl.localhost paths, so inputs and the -o output go through a Windows temp folder.
    wintmp="$(wslpath "$(cmd.exe /c echo %TEMP% 2>/dev/null | tr -d '\r')")"
    tmp="$(mktemp -d -p "$wintmp" rs64metal.XXXXXX)"
    trap 'rm -rf "$tmp"' EXIT
    args=("${target[@]}")
    out=""
    next_is_out=0
    for a in "$@"; do
        if [ "$next_is_out" = 1 ]; then
            out="$a"
            args+=("$(wslpath -w "$tmp")\\$(basename "$a")")
            next_is_out=0
        elif [ "$a" = -o ]; then
            args+=(-o)
            next_is_out=1
        elif [ -f "$a" ]; then
            cp "$a" "$tmp/"
            args+=("$(wslpath -w "$tmp")\\$(basename "$a")")
        else
            args+=("$a")
        fi
    done
    "$TOOLS/$tool.exe" "${args[@]}"
    [ -z "$out" ] || cp "$tmp/$(basename "$out")" "$out"
    exit 0
fi

# Wine maps / to Z:. Debian/Ubuntu's wine launcher can pick a foreign-arch (e.g. armhf) wineserver, so the 64-bit binaries are called directly when present.
WINE=/usr/lib/wine/wine64
[ -x "$WINE" ] || WINE="$(command -v wine64 || command -v wine)"
[ ! -x /usr/lib/wine/wineserver64 ] || export WINESERVER=/usr/lib/wine/wineserver64
export WINEPREFIX="${METAL_WINEPREFIX:-$HOME/.wine-metal}" WINEDEBUG=-all DISPLAY=
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    mkdir -p "$(dirname "$WINEPREFIX")"
    flock "$WINEPREFIX.lock" sh -c '[ -f "$WINEPREFIX/system.reg" ] || "$1" wineboot -i' sh "$WINE" >/dev/null 2>&1
fi
args=("${target[@]}")
for a in "$@"; do
    case "$a" in
        /*) args+=("Z:${a//\//\\}") ;;
        *) args+=("$a") ;;
    esac
done
# Wine lacks the replace-on-rename mode LLVM tries first; it falls back, so the "Rename failed" lines are noise.
err="$(mktemp)"
trap 'rm -f "$err"' EXIT
rc=0
"$WINE" "$TOOLS/$tool.exe" "${args[@]}" 2>"$err" || rc=$?
grep -v '^Rename failed: ' "$err" >&2 || true
exit "$rc"
