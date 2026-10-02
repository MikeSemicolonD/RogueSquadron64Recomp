# Build for the long-tow-cable code mod, separate from the game build:
#
#   cmake -S mods/long-tow-cable -B mods/long-tow-cable/build
#   cmake --build mods/long-tow-cable/build
#
# Produces mods/long_tow_cable.nrm; the next game build copies it beside the exe.
cmake_minimum_required(VERSION 3.20)
project(long_tow_cable_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(long_tow_cable src/long_tow_cable.s)
