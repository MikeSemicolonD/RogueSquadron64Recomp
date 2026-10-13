# Test-only code mod for menu files inside a .nrm, config: behaviors and .nrm @export actions. Not shipped.
#
#   cmake -S tests/mods/menu-probe -B tests/mods/menu-probe/build
#   cmake --build tests/mods/menu-probe/build
#
# Copy tests/mods/menu_probe.nrm into build/Debug/mods/; the main menu shows MENU PROBE.
cmake_minimum_required(VERSION 3.20)
project(menu_probe_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/mods/code_mod.cmake")
add_code_mod(menu_probe src/menu_probe.c)
