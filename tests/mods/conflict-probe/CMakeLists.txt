# Test-only code mods for the base-patch conflict checks: patch must be refused, force loads with a warning, hook (a toml-hooked function) and hook-patched (a patches-ELF function) load and fire. Not shipped.
#
#   cmake -S tests/mods/conflict-probe -B tests/mods/conflict-probe/build
#   cmake --build tests/mods/conflict-probe/build
#
# Copy one tests/mods/conflict-probe/*.nrm into build/Debug/mods/ and boot.
cmake_minimum_required(VERSION 3.20)
project(conflict_probe NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/mods/code_mod.cmake")
add_subdirectory(patch)
add_subdirectory(hook)
add_subdirectory(force)
add_subdirectory(hook-patched)
