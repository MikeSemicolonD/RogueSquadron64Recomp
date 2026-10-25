#!/usr/bin/env bash
# Checks one Linux package (RogueSquadron64Recomp-linux-<arch>-*.tar.xz, *.AppImage or *.flatpak) and prints PASS/FAIL/SKIP lines; exits 1 on any FAIL.
# Usage: tools/release/linux/test-package.sh <package> [--boot <rom>] [--replay <rom> <save> <recording.rec>]
#   always: arch of every binary, glibc floor, no host-only libraries bundled, launcher bits, no ROM/saves inside, --help (foreign arches via qemu)
#   --boot: 90 s headless boot with the fake controller (host arch only); must reach the account menus with no fault
#   --replay: tarball only, host arch only; lockstep replay with the given save seeded, compared against <recording>.hash
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
winpath() { if [[ "$1" =~ ^[A-Za-z]: ]]; then wslpath -a "$1"; else realpath -m "$1"; fi; }
PKG="$(winpath "$1")"
shift
BOOT_ROM=""
REPLAY_ROM=""
REPLAY_SAVE=""
REPLAY_REC=""
while [ $# -gt 0 ]; do
    case "$1" in
        --boot) BOOT_ROM="$(winpath "$2")"; shift 2 ;;
        --replay) REPLAY_ROM="$(winpath "$2")"; REPLAY_SAVE="$(winpath "$3")"; REPLAY_REC="$(winpath "$4")"; shift 4 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -f "$PKG" ] || { echo "FAIL package: no such file $PKG"; exit 1; }

fails=0
name="$(basename "$PKG")"
pass() { echo "PASS $name $1"; }
fail() { echo "FAIL $name $1${2:+: $2}"; fails=$((fails + 1)); }
skip() { echo "SKIP $name $1${2:+: $2}"; }

case "$name" in
    *x64*|*x86_64*) arch=x64;   magic="x86-64";      qemu="" ;;
    *arm64*|*aarch64*) arch=arm64; magic="ARM aarch64"; qemu="qemu-aarch64 -L /" ;;
    *armhf*) arch=armhf; magic="ARM, EABI5";  qemu="qemu-arm -L /" ;;
    *i386*|*i686*) arch=i386; magic="Intel 80386"; qemu="qemu-i386 -L /" ;;
    *) echo "FAIL $name: can't tell the architecture from the name"; exit 1 ;;
esac
eval "$(grep -E '^skip(_family)?=' "$REPO/tools/release/package-linux.sh")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fault_pattern='UNKNOWN EXCEPTION|FATAL|undefined symbol|Gtk-CRITICAL|Gtk:ERROR|Bail out|SDL_Init failed|SDL_CreateWindow failed|error while loading shared libraries'

# Checks shared by the tarball and the AppImage, on an unpacked tree: <exe> <lib dir>.
check_tree() {
    local exe="$1" libdir="$2"
    file "$exe" | grep -q "$magic" && pass "exe-arch" || fail "exe-arch" "$(file -b "$exe" | cut -c1-60)"
    local wrong
    wrong="$(for f in "$libdir"/*.so*; do [ -e "$f" ] && ! file -L "$f" | grep -q "$magic" && basename "$f"; done)"
    [ -z "$wrong" ] && pass "lib-arch" || fail "lib-arch" "$wrong"
    local glibc
    glibc="$(readelf --dyn-syms --wide "$exe" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
    [ "$(printf '%s\n2.38\n' "$glibc" | sort -V | tail -1)" = "2.38" ] && pass "glibc<=2.38 ($glibc)" || fail "glibc<=2.38" "needs $glibc"
    local host_libs
    host_libs="$(for f in "$libdir"/*; do b="$(basename "$f")"; [[ "$b" =~ $skip || "$b" =~ $skip_family ]] && echo "$b"; done)"
    [ -z "$host_libs" ] && pass "no-host-libs ($(ls "$libdir" | wc -l) bundled)" || fail "no-host-libs" "$(echo $host_libs)"
}

check_no_rom() {
    local found
    found="$(find "$1" \( -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -name saves \) | head -3)"
    [ -z "$found" ] && pass "no-rom-or-saves" || fail "no-rom-or-saves" "$found"
}

# Boot until the account menus (or a fault). Args: log file, then the command.
boot_check() {
    local log="$1"; shift
    ROGUESQ_AUDIO_GAIN=0 ROGUESQ_LOG_GAMESTATE=1 ROGUESQ_HIDE_WINDOW=1 timeout 90 "$@" --fake-controller --auto-start 8000 > "$log" 2>&1
    local reached
    reached="$(grep -ao 'state=menu\.account[a-z_.]*' "$log" | tail -1)"
    if grep -aqE "$fault_pattern" "$log"; then
        fail "boot" "$(grep -aE "$fault_pattern" "$log" | head -1 | cut -c1-100)"
    elif [ -n "$reached" ]; then
        pass "boot (${reached#state=})"
    else
        fail "boot" "never reached the account menus (last: $(grep -ao 'state=[a-z_.]*' "$log" | tail -1))"
    fi
}

case "$name" in
    *.tar.xz|*.tar.gz)
        tree="$work/tree"
        mkdir -p "$tree"
        tar xf "$PKG" -C "$tree" --strip-components=1
        check_tree "$tree/RogueSquadron64Recomp" "$tree/lib"
        [ -x "$tree/run.sh" ] && [ -x "$tree/RogueSquadron64Recomp" ] && [ -x "$tree/install-desktop.sh" ] && pass "executable-bits" || fail "executable-bits"
        file "$tree/rs64recomp.png" | grep -q "256 x 256" && pass "icon" || fail "icon"
        [ -n "$(ls -A "$tree/mods" 2>/dev/null)" ] && pass "mods ($(ls "$tree/mods" | wc -l))" || fail "mods" "none bundled"
        check_no_rom "$tree"
        (cd "$tree" && LD_LIBRARY_PATH="$tree/lib" $qemu ./RogueSquadron64Recomp --help > "$work/help.txt" 2>&1)
        grep -q "^Options:" "$work/help.txt" && pass "help" || fail "help" "$(head -1 "$work/help.txt")"
        if [ -n "$BOOT_ROM" ]; then
            if [ -n "$qemu" ]; then
                skip "boot" "foreign arch"
            else
                cp "$BOOT_ROM" "$tree/rogue_squadron.z64"
                pushd "$tree" > /dev/null; boot_check "$work/boot.log" ./run.sh; popd > /dev/null
                rm -f "$tree/rogue_squadron.z64" "$tree"/rs64.*.z64
                rm -rf "$tree/saves"
            fi
        fi
        if [ -n "$REPLAY_REC" ]; then
            if [ -n "$qemu" ]; then
                skip "replay" "foreign arch"
            else
                level="$(grep -m1 '^level=' "$REPLAY_REC" | cut -d= -f2)"
                craft="$(grep -m1 '^craft=' "$REPLAY_REC" | cut -d= -f2)"
                cp "$REPLAY_ROM" "$tree/rogue_squadron.z64"
                mkdir -p "$tree/saves"
                cp "$REPLAY_SAVE" "$tree/saves/"
                (cd "$tree" && ROGUESQ_BOOT_TARGET="level:$level,$craft" ROGUESQ_FAKE_CONTROLLER=1 ROGUESQ_AUDIO_GAIN=0 ROGUESQ_SPEED=4 \
                    ROGUESQ_NAV_STEP_BUDGET=4800 ROGUESQ_HIDE_WINDOW=1 ROGUESQ_LS_REPLAY="$REPLAY_REC" ROGUESQ_LS_HASH_OUT="$work/replay.hash" \
                    timeout 300 ./run.sh > "$work/replay.log" 2>&1)
                result="$(python3 "$REPO/tools/debug/recordings/compare_hashes.py" "$REPLAY_REC.hash" "$work/replay.hash" 2>&1 | tail -1)"
                [[ "$result" == IDENTICAL* ]] && pass "replay ($result)" || fail "replay" "$result"
            fi
        fi
        ;;
    *.AppImage)
        cp "$PKG" "$work/app.AppImage"
        chmod +x "$work/app.AppImage"
        (cd "$work" && $qemu ./app.AppImage --appimage-extract > /dev/null 2>&1)
        root="$work/squashfs-root"
        if [ ! -d "$root" ]; then
            fail "extract"
        else
            check_tree "$root/usr/bin/RogueSquadron64Recomp" "$root/usr/lib"
            [ -x "$root/AppRun" ] && pass "executable-bits" || fail "executable-bits"
            grep -q '^StartupWMClass=rs64recomp' "$root/rs64recomp.desktop" && file "$root/rs64recomp.png" | grep -q "256 x 256" && pass "desktop+icon" || fail "desktop+icon"
            check_no_rom "$root"
        fi
        (cd "$work" && APPIMAGE_EXTRACT_AND_RUN=1 RS64_DATA_DIR="$work/data" $qemu ./app.AppImage --help > "$work/help.txt" 2>&1)
        grep -q "^Options:" "$work/help.txt" && pass "help" || fail "help" "$(head -1 "$work/help.txt")"
        [ -n "$(ls -A "$work/data/mods" 2>/dev/null)" ] && pass "data-dir-mods" || fail "data-dir-mods" "AppRun didn't stage the mods"
        if [ -n "$BOOT_ROM" ]; then
            if [ -n "$qemu" ]; then
                skip "boot" "foreign arch"
            else
                mkdir -p "$work/bootdata"
                cp "$BOOT_ROM" "$work/bootdata/rogue_squadron.z64"
                pushd "$work" > /dev/null; APPIMAGE_EXTRACT_AND_RUN=1 RS64_DATA_DIR="$work/bootdata" boot_check "$work/boot.log" ./app.AppImage; popd > /dev/null
            fi
        fi
        ;;
    *.flatpak)
        app_id=io.github.MikeSemicolonD.RogueSquadron64Recomp
        if [ "$arch" != x64 ]; then
            skip "install+help" "foreign arch"
        elif ! command -v flatpak > /dev/null; then
            skip "install+help" "flatpak not installed"
        else
            flatpak uninstall --user -y --noninteractive "$app_id" > /dev/null 2>&1
            flatpak install --user -y --noninteractive "$PKG" > "$work/install.log" 2>&1 && pass "install" || fail "install" "$(tail -1 "$work/install.log")"
            flatpak run "$app_id" --help > "$work/help.txt" 2>&1
            grep -q "^Options:" "$work/help.txt" && pass "help" || fail "help" "$(head -1 "$work/help.txt")"
            perms="$(flatpak info --show-permissions "$app_id" 2>/dev/null)"
            echo "$perms" | grep -q 'x11' && echo "$perms" | grep -q 'pulseaudio' && pass "permissions" || fail "permissions" "$(echo $perms | cut -c1-80)"
            if [ -n "$BOOT_ROM" ]; then
                data="$HOME/.var/app/$app_id/data"
                mkdir -p "$data"
                [ -e "$data/rogue_squadron.z64" ] || cp "$BOOT_ROM" "$data/rogue_squadron.z64"
                boot_check "$work/boot.log" flatpak run --env=ROGUESQ_AUDIO_GAIN=0 --env=ROGUESQ_LOG_GAMESTATE=1 --env=ROGUESQ_HIDE_WINDOW=1 "$app_id"
            fi
        fi
        ;;
    *) echo "FAIL $name: unknown package type"; exit 1 ;;
esac

if [ -n "${RS64_TEST_LOG_DIR:-}" ]; then
    mkdir -p "$RS64_TEST_LOG_DIR"
    for l in boot replay; do [ -f "$work/$l.log" ] && cp "$work/$l.log" "$RS64_TEST_LOG_DIR/$name.$l.log"; done
fi
exit $((fails > 0))
