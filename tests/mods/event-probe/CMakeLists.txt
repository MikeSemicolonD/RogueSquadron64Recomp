# Test-only code mod for the rs64_on_* base events. Not shipped.
#
#   cmake -S tests/mods/event-probe -B tests/mods/event-probe/build
#   cmake --build tests/mods/event-probe/build
#
# Copy tests/mods/event_probe.nrm into build/Debug/mods/, boot with ROGUESQ_WATCH_ADDRS=80C00000,80C00004,80C00008,80C0000C,80C00010,80C00014,80C00018,80C0001C,80C00020.
cmake_minimum_required(VERSION 3.20)
project(event_probe_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/mods/code_mod.cmake")
add_code_mod(event_probe src/event_probe.c)
