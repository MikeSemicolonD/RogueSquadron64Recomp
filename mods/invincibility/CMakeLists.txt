# Build for the invincibility code mod, separate from the game build:
#
#   cmake -S mods/invincibility -B mods/invincibility/build
#   cmake --build mods/invincibility/build
#
# Produces mods/invincibility.nrm; the next game build copies it beside the exe.
cmake_minimum_required(VERSION 3.20)
project(invincibility_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(invincibility src/invincibility.c)
