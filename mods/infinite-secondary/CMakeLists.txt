# Build for the infinite-secondary code mod, separate from the game build:
#
#   cmake -S mods/infinite-secondary -B mods/infinite-secondary/build
#   cmake --build mods/infinite-secondary/build
#
# Produces mods/infinite_secondary.nrm; the next game build copies it beside the exe.
cmake_minimum_required(VERSION 3.20)
project(infinite_secondary_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(infinite_secondary src/infinite_secondary.c)
