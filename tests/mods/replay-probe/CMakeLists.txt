# Test-only code mod: empty hooks on functions whose rogue_squadron.toml patches LiveRecomp must replay. A replay with it staged must hash the same as without. Not shipped.
#
#   cmake -S tests/mods/replay-probe -B tests/mods/replay-probe/build
#   cmake --build tests/mods/replay-probe/build
cmake_minimum_required(VERSION 3.20)
project(replay_probe_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/mods/code_mod.cmake")
add_code_mod(replay_probe src/replay_probe.c)
