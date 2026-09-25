# Build for the any-craft code mod, separate from the game build:
#
#   cmake -S mods/any-craft -B mods/any-craft/build
#   cmake --build mods/any-craft/build
#
# Produces mods/any_craft.nrm; the next game build copies it beside the exe.
cmake_minimum_required(VERSION 3.20)
project(any_craft_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(any_craft src/any_craft.c)
