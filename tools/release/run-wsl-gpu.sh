#!/usr/bin/env bash
# Launch the Linux build under WSL with GPU-accelerated Vulkan via Mesa Dozen
# (dzn): Vulkan -> D3D12 -> the real GPU through /dev/dxg. Without this the WSL
# Vulkan loader falls back to llvmpipe (software) and the game hitches badly.
#
# Prereqs (one-time, see project_linux_wsl_build_run_2026_09_21 memory):
#   - Game built to $RS64_BUILD_DIR (default ~/rs64-build) -- see the
#     "Build (Linux/WSL)" VSCode task.
#   - Mesa dzn built+installed to $DZN_PREFIX (default ~/mesa-dzn).
#   - rogue_squadron.z64 present next to the exe (symlinked below if missing).
#
# Any extra args are forwarded to the game, e.g.:
#   tools/release/run-wsl-gpu.sh --fake-controller --auto-start 8000
set -euo pipefail

RS64_BUILD_DIR="${RS64_BUILD_DIR:-$HOME/rs64-build}"
DZN_PREFIX="${DZN_PREFIX:-$HOME/mesa-dzn}"
ROM_SRC="${ROM_SRC:-/mnt/e/Projects/RogueSquadron64Recomp/build/Debug/rogue_squadron.z64}"

exe="$RS64_BUILD_DIR/RogueSquadron64Recomp"
icd="$DZN_PREFIX/share/vulkan/icd.d/dzn_icd.x86_64.json"

[ -x "$exe" ] || { echo "error: exe not found at $exe (build it first)" >&2; exit 1; }
[ -f "$icd" ] || { echo "error: dzn ICD not found at $icd (build Mesa dzn first)" >&2; exit 1; }
[ -f "$RS64_BUILD_DIR/rogue_squadron.z64" ] || ln -sf "$ROM_SRC" "$RS64_BUILD_DIR/rogue_squadron.z64"

# dzn ICD only (so the loader picks the GPU, not llvmpipe); WSL D3D12 libs on
# the search path for libd3d12.so; the dzn driver .so alongside it.
export VK_ICD_FILENAMES="$icd"
export LD_LIBRARY_PATH="/usr/lib/wsl/lib:$DZN_PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

# Route SDL audio to WSLg's PulseServer (Windows speakers). If it crackles under
# WSL scheduling jitter, deepen the game's audio queue: --set ROGUESQ_AUDIO_LATENCY_MS=120
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pulseaudio}"

cd "$RS64_BUILD_DIR"
exec ./RogueSquadron64Recomp "$@"
