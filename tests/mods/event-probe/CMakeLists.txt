# Test-only code mod for the rs64_on_* base events. Not shipped.
#
#   cmake -S tests/mods/event-probe -B tests/mods/event-probe/build
#   cmake --build tests/mods/event-probe/build
#
# Copy tests/mods/event_probe.nrm into build/Debug/mods/, boot with ROGUESQ_WATCH_ADDRS=80A02000,80A02004,80A02008,80A0200C,80A02010,80A02014,80A02018,80A0201C,80A02020.
cmake_minimum_required(VERSION 3.20)
project(event_probe_mod NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/mods/code_mod.cmake")
add_code_mod(event_probe src/event_probe.c)
