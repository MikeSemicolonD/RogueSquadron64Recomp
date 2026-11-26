# Extended Laser Range: a code-less .nrm of instruction patches (mod.toml).
#
#   cmake -S mods/extended-laser-range -B mods/extended-laser-range/build
#   cmake --build mods/extended-laser-range/build
cmake_minimum_required(VERSION 3.20)
project(extended_laser_range_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(extended_laser_range)
