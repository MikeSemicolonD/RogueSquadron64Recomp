#!/usr/bin/env bash
# Tests setup from scratch on a remote Linux machine: wipes ~/rs64-fresh there, clones the repo from GitHub into it and runs tools/setup/setup.sh.
# apt packages stay installed between runs; everything else (repo, decomp, tools, build) is rebuilt. The ROM is kept at ~/rogue_squadron.z64 on the remote.
# Usage: tools/setup/test-fresh-clone.sh <user@host> <rom> [branch]   (the log goes to logs/setup-tests/)
set -uo pipefail

TARGET="$1"
ROM="$2"
BRANCH="${3:-main}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG="$REPO/logs/setup-tests/$(echo "$TARGET" | tr '@' '_')-$(date +%Y%m%d-%H%M%S).log"

mkdir -p "$(dirname "$LOG")"

if ! ssh "$TARGET" 'test -f ~/rogue_squadron.z64'; then
    echo "copying the ROM to $TARGET"
    ssh "$TARGET" 'cat > ~/rogue_squadron.z64' < "$ROM" || exit 1
fi

start=$SECONDS

ssh "$TARGET" "set -e
rm -rf ~/rs64-fresh
mkdir ~/rs64-fresh
cd ~/rs64-fresh
git clone -b '$BRANCH' https://github.com/MikeSemicolonD/RogueSquadron64Recomp.git
cd RogueSquadron64Recomp
bash tools/setup/setup.sh ~/rogue_squadron.z64
test -x build/Release/RogueSquadron64Recomp
build/Release/RogueSquadron64Recomp --help > /dev/null" 2>&1 | tee "$LOG"

rc=${PIPESTATUS[0]}

echo "exit $rc after $(( (SECONDS - start) / 60 )) min; log: $LOG" | tee -a "$LOG"
exit "$rc"
