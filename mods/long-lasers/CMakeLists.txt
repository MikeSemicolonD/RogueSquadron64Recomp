# Long Lasers: a code-less .nrm of instruction patches (mod.toml).
#
#   cmake -S mods/long-lasers -B mods/long-lasers/build
#   cmake --build mods/long-lasers/build
cmake_minimum_required(VERSION 3.20)
project(long_lasers_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(long_lasers)
