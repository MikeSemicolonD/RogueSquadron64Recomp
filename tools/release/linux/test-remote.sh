#!/usr/bin/env bash
# Tests the Linux package on remote machines (Raspberry Pis) over ssh: deploy the tarball matching each machine's architecture, run --help, then a headless boot in its desktop session.
# Usage: tools/release/linux/test-remote.sh <release-dir> <rom> [remote-name ...]
# Remotes come from ~/.config/rs64/remotes (kept out of the repo), one per line: <name> <user>@<host> <remote-dir>, e.g.
#   pi-ubuntu  michael@pi-ubuntu.local  ~/rs64
# One ssh connection per remote is shared by every step (ControlMaster), so a password is asked for once; an ssh key avoids it entirely.
# Test machine packages beyond the game's own needs: xdotool (tools/release/linux/drive-menus.sh drives the menus), vulkan-validationlayers
# (run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation), flatpak (to test the Flatpak bundle). Delete what you put on the machine afterwards.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
winpath() { if [[ "$1" =~ ^[A-Za-z]: ]]; then wslpath -a "$1"; else realpath -m "$1"; fi; }
RELEASE_DIR="$(winpath "$1")"
ROM="$(winpath "$2")"
shift 2
CONFIG="${RS64_REMOTES:-$HOME/.config/rs64/remotes}"
LOG_DIR="${RS64_TEST_LOG_DIR:-$REPO/logs/platform-tests/remote-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$LOG_DIR"
[ -f "$CONFIG" ] || { echo "no remotes configured: create $CONFIG with lines '<name> <user>@<host> <remote-dir>'" >&2; exit 2; }

fails=0
fault_pattern='UNKNOWN EXCEPTION|FATAL|undefined symbol|Gtk-CRITICAL|Gtk:ERROR|Bail out|SDL_Init failed|SDL_CreateWindow failed|error while loading shared libraries|Illegal instruction|Segmentation fault'

test_remote() {
    local name="$1" target="$2" dir="$3"
    pass() { echo "PASS $name $1"; }
    fail() { echo "FAIL $name $1${2:+: $2}"; fails=$((fails + 1)); }
    skip() { echo "SKIP $name $1${2:+: $2}"; }
    local sock="$HOME/.ssh/rs64-%r@%h:%p"
    local ssh_opts=(-o ControlMaster=auto -o "ControlPath=$sock" -o ControlPersist=120 -o ConnectTimeout=10)
    r() { ssh "${ssh_opts[@]}" "$target" "$@"; }

    local info
    if ! info="$(r 'dpkg --print-architecture; ldd --version 2>&1 | head -1 | grep -o "[0-9.]*$"; . /etc/os-release; echo "$PRETTY_NAME"; id -u; uname -m')"; then
        fail "connect" "ssh $target"
        return
    fi
    local arch glibc os uid kernel
    { read -r arch; read -r glibc; read -r os; read -r uid; read -r kernel; } <<< "$info"
    echo "INFO $name $os, $arch userland on $kernel, glibc $glibc"
    local vulkan
    vulkan="$(r 'command -v vulkaninfo > /dev/null && vulkaninfo --summary 2>/dev/null | grep -m1 deviceName | sed "s/.*= //"')"
    echo "INFO $name Vulkan device: ${vulkan:-unknown (install vulkan-tools for vulkaninfo)}"

    if [ "$(printf '%s\n2.38\n' "$glibc" | sort -V | head -1)" != "2.38" ]; then
        skip "all" "glibc $glibc is older than the 2.38 the builds need"
        return
    fi
    local pkg
    pkg="$(ls -t "$RELEASE_DIR"/RogueSquadron64Recomp-linux-"$arch"-*.tar.xz 2>/dev/null | head -1)"
    [ -n "$pkg" ] || { fail "package" "no RogueSquadron64Recomp-linux-$arch-*.tar.xz in $RELEASE_DIR"; return; }
    echo "INFO $name deploying $(basename "$pkg") to $dir"

    if r "rm -rf $dir/lib $dir/mods && mkdir -p $dir && tar xJf - -C $dir --strip-components=1" < "$pkg"; then
        pass "deploy"
    else
        fail "deploy"
        return
    fi
    if r "test -f $dir/rogue_squadron.z64 || test -f $dir/rs64.n64.us.1.0.z64"; then
        pass "rom (already there)"
    elif r "cat > $dir/rogue_squadron.z64" < "$ROM"; then
        pass "rom (copied)"
    else
        fail "rom"
    fi
    r "cd $dir && ./run.sh --help" > "$LOG_DIR/$name.help.txt" 2>&1
    grep -q '^Options:' "$LOG_DIR/$name.help.txt" && pass "help" || fail "help" "$(head -1 "$LOG_DIR/$name.help.txt")"

    # The boot needs the logged-in desktop session's display.
    local session
    session="$(r "ls /run/user/$uid 2>/dev/null | grep -m1 '^wayland-[0-9]*\$'; ls /tmp/.X11-unix 2>/dev/null | head -1")"
    if [ -z "$session" ]; then
        skip "boot" "no desktop session (log in on the Pi's screen, or enable auto-login)"
        return
    fi
    local wayland x11
    wayland="$(echo "$session" | grep -m1 '^wayland-' || true)"
    x11="$(echo "$session" | grep -m1 '^X' | sed 's/^X/:/' || true)"
    r "cd $dir && XDG_RUNTIME_DIR=/run/user/$uid ${wayland:+WAYLAND_DISPLAY=$wayland} DISPLAY=${x11:-:0} \
        ROGUESQ_AUDIO_GAIN=0 ROGUESQ_LOG_GAMESTATE=1 timeout ${RS64_REMOTE_BOOT_SECONDS:-240} ./run.sh --fake-controller --auto-start 8000" \
        > "$LOG_DIR/$name.boot.log" 2>&1
    local log="$LOG_DIR/$name.boot.log"
    local device reached
    device="$(grep -a -m1 'Device Name:' "$log" | sed 's/.*Device Name: //')"
    reached="$(grep -ao 'state=menu\.account[a-z_.]*' "$log" | tail -1)"
    if grep -aqE "$fault_pattern" "$log"; then
        fail "boot" "$(grep -aE "$fault_pattern" "$log" | head -1 | cut -c1-100)"
    elif [ -n "$reached" ]; then
        pass "boot (${reached#state=}; GPU ${device:-?})"
    else
        fail "boot" "never reached the account menus (last: $(grep -ao 'state=[a-z_.]*' "$log" | tail -1); GPU ${device:-none})"
    fi
}

while read -r name target dir <&3; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    if [ $# -gt 0 ] && [[ ! " $* " == *" $name "* ]]; then
        continue
    fi
    test_remote "$name" "$target" "$dir"
done 3< "$CONFIG"
echo "logs: $LOG_DIR"
exit $((fails > 0))
