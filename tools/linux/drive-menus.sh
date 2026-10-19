#!/usr/bin/env bash
# Runs on a Linux test machine: starts the game in its desktop session and drives the menus with xdotool until the game-state log
# reports <target-state> (e.g. menu.account.enter_name), then waits and stops it. Screenshots land in <work-dir>/screenshots.
# Usage: drive-menus.sh <game-dir> <target-state> [extra game args...]   (needs xdotool; work dir: RS64_DRIVE_DIR, default /tmp/rs64-drive)
# RS64_DRIVE_THEN="Return Down ..." presses those xdotool keys, 2 s apart, after the target state (screens the classifier doesn't name).
set -uo pipefail
GAME_DIR="$1"
TARGET="$2"
shift 2
WORK="${RS64_DRIVE_DIR:-/tmp/rs64-drive}"
mkdir -p "$WORK"
cd "$WORK"
export DISPLAY="${DISPLAY:-:0}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
"$GAME_DIR/run.sh" --set ROGUESQ_AUDIO_GAIN=0 --set ROGUESQ_LOG_GAMESTATE=1 --set ROGUESQ_SCREENSHOT_EVERY=3 --set ROGUESQ_SCREENSHOT_MAX=60 "$@" > "$WORK/game.log" 2>&1 &
game=$!

press() {
    local w
    w=$(xdotool search --name "Rogue Squadron 64" | head -1)
    [ -n "$w" ] && xdotool windowactivate --sync "$w" key --delay 120 "$1" 2>/dev/null
}

state() { grep -a "gamestate\] ->" "$WORK/game.log" | tail -1 | awk '{print $NF}'; }

# Return is A (confirm) and Escape is Start in the default bindings.
for i in $(seq 1 120); do
    sleep 2
    s=$(state)
    echo "t=$((i * 2))s state=$s"
    [ "$s" = "$TARGET" ] && break
    case "$s" in
        menu.main) press Return ;;
        *) press Escape ;;
    esac
done

for k in ${RS64_DRIVE_THEN:-}; do
    sleep 2
    press "$k"
    echo "pressed $k"
done

sleep 12
echo "final=$(state)"
kill "$game" 2>/dev/null
pkill -x RogueSquadron64
