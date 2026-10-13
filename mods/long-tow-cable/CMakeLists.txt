# Long Tow Cable: a code-less .nrm of instruction patches (mod.toml).
#
#   cmake -S mods/long-tow-cable -B mods/long-tow-cable/build
#   cmake --build mods/long-tow-cable/build
cmake_minimum_required(VERSION 3.20)
project(long_tow_cable_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(long_tow_cable)
