# Build for the atst-crosshair code mod, separate from the game build:
#
#   cmake -S mods/atst-crosshair -B mods/atst-crosshair/build
#   cmake --build mods/atst-crosshair/build
#
# Produces mods/atst_crosshair.nrm; the next game build copies it beside the exe.
cmake_minimum_required(VERSION 3.20)
project(atst_crosshair_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(atst_crosshair src/atst_crosshair.c)
