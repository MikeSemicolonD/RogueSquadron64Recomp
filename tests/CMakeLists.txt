# Unit tests. Part of the game's build, or standalone with no ROM, recompiled code or MIPS toolchain (submodules only):
#   cmake -S tests -B build-tests && cmake --build build-tests --config Release --target rs64_unit_tests
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(rs64_unit_tests LANGUAGES C CXX)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
get_filename_component(RS64_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

if (NOT TARGET tomlplusplus::tomlplusplus)
    set(TOML_ENABLE_FORMATTERS OFF)
    add_subdirectory(${RS64_ROOT}/lib/N64ModernRuntime/N64Recomp/lib/tomlplusplus ${CMAKE_BINARY_DIR}/tomlplusplus EXCLUDE_FROM_ALL)
endif()

if (NOT TARGET gen_state_table)
    include(${RS64_ROOT}/tools/state/gen_state_table.cmake)
endif()

add_subdirectory(${RS64_ROOT}/mods/multiplayer-native/tests ${CMAKE_BINARY_DIR}/multiplayer-native-tests)

if (NOT TARGET relay_core_test)
    add_subdirectory(${RS64_ROOT}/relay-server ${CMAKE_BINARY_DIR}/relay-server)
endif()

# Same rounding as the game: no fused multiply-add in the lockstep math.
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(${RS64_ROOT}/src/lockstep/lockstep_core.cpp PROPERTIES COMPILE_OPTIONS "/clang:-ffp-contract=off")
elseif (NOT MSVC)
    set_source_files_properties(${RS64_ROOT}/src/lockstep/lockstep_core.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

# Standalone unit test for the nav-sequencer target parser.
add_executable(nav_target_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/nav_target_test.cpp
    ${RS64_ROOT}/src/game/nav_sequencer.cpp
    ${RS64_ROOT}/src/lockstep/lockstep_core.cpp
    ${RS64_ROOT}/src/game/game_state.cpp
    ${RS64_ROOT}/src/input/touch_menu.cpp
    ${RS64_ROOT}/src/host_api/host_api.cpp
)
set_target_properties(nav_target_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(nav_target_test PRIVATE ${RS64_ROOT}/src ${RS64_ROOT}/include)
add_dependencies(nav_target_test gen_state_table)

# Standalone unit test for the lockstep record/replay core.
add_executable(lockstep_core_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/lockstep_core_test.cpp
    ${RS64_ROOT}/src/lockstep/lockstep_core.cpp
)
set_target_properties(lockstep_core_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(lockstep_core_test PRIVATE ${RS64_ROOT}/src)

# Standalone unit test for the mod host API registry.
add_executable(host_api_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/host_api_test.cpp
    ${RS64_ROOT}/src/host_api/host_api.cpp
)
set_target_properties(host_api_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(host_api_test PRIVATE ${RS64_ROOT}/src ${RS64_ROOT}/include)
target_compile_definitions(host_api_test PRIVATE RS64_TOML_PATH="${RS64_ROOT}/rogue_squadron.toml" RS64_TOML_HOOKS_PATH="${RS64_ROOT}/src/game/toml_hooks.c")

# Standalone unit test for the tow cable head interpolation alias.
add_executable(cable_alias_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/cable_alias_test.cpp
)
set_target_properties(cable_alias_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Standalone unit test for the mid-mission camera shot links.
add_executable(camera_shots_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/camera_shots_test.cpp
)
set_target_properties(camera_shots_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(camera_shots_test PRIVATE ${RS64_ROOT}/src)
add_executable(mod_levels_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/mod_levels_test.cpp
)
set_target_properties(mod_levels_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(mod_levels_test PRIVATE ${RS64_ROOT}/src ${RS64_ROOT}/lib/rt64/src/contrib)
add_executable(level_rules_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/level_rules_test.cpp
)
set_target_properties(level_rules_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(level_rules_test PRIVATE ${RS64_ROOT}/src ${RS64_ROOT}/lib/rt64/src/contrib)

# Standalone unit test for menu button index placement.
add_executable(menu_layout_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/menu_layout_test.cpp
)
set_target_properties(menu_layout_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(menu_layout_test PRIVATE ${RS64_ROOT}/src)

# Standalone unit test for language mods (parsing, code page, glyph packs).
add_executable(lang_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/lang_test.cpp
)
# Standalone unit test for the MODS page's list (window, carry, restart notice, row text).
add_executable(mods_page_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/mods_page_test.cpp
)
set_target_properties(mods_page_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(mods_page_test PRIVATE ${RS64_ROOT}/src)

set_target_properties(lang_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(lang_test PRIVATE ${RS64_ROOT}/src ${RS64_ROOT}/lib/rt64/src/contrib)

if(MSVC)
    target_compile_options(lang_test PRIVATE /utf-8)
endif()

# Standalone unit test for the crosshair texrect matcher.
add_executable(crosshair_match_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/crosshair_match_test.cpp
)
set_target_properties(crosshair_match_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Standalone unit test for the dynamic point-light helpers.
add_executable(rs64_lights_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/rs64_lights_test.cpp
)
set_target_properties(rs64_lights_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Standalone unit test for the mode-change blanking gate.
add_executable(transition_gate_test EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/transition_gate_test.cpp
)
set_target_properties(transition_gate_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# AGENTS.md style rules for src/, against tools/style/baseline.txt.
add_executable(style_check EXCLUDE_FROM_ALL
    ${RS64_ROOT}/tests/style_check.cpp
)
set_target_properties(style_check PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

set(RS64_BASE_TESTS lockstep_core_test nav_target_test host_api_test cable_alias_test camera_shots_test mod_levels_test level_rules_test crosshair_match_test rs64_lights_test menu_layout_test mods_page_test lang_test)
set(RS64_TESTS ${RS64_BASE_TESTS} mp_core_test net_core_test net_link_test relay_core_test relay_client_test)

if (MSVC)
    foreach(t ${RS64_BASE_TESTS} transition_gate_test style_check)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endforeach()
endif()

# One command for the unit tests: cmake --build build --config Release --target rs64_unit_tests
set(RS64_TEST_COMMANDS)
foreach(t ${RS64_TESTS})
    list(APPEND RS64_TEST_COMMANDS COMMAND $<TARGET_FILE:${t}>)
endforeach()

add_custom_target(rs64_unit_tests
    ${RS64_TEST_COMMANDS}
    COMMAND $<TARGET_FILE:style_check> ${RS64_ROOT}
    DEPENDS ${RS64_TESTS} style_check
    COMMENT "Running the unit tests")
