# Build for the larger-object-pool code mod, separate from the game build:
#
#   cmake -S mods/larger-object-pool -B mods/larger-object-pool/build
#   cmake --build mods/larger-object-pool/build
#
# Produces mods/larger_object_pool.nrm; the next game build copies it beside the exe.
cmake_minimum_required(VERSION 3.20)
project(larger_object_pool_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mods/code_mod.cmake")
add_code_mod(larger_object_pool src/larger_object_pool.c)
