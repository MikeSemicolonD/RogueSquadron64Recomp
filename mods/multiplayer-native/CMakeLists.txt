# Online co-op as a native mod: multiplayer_native.dll / .so, which reaches the game only through the host API table.
# Added by the game's CMake; one folder per config, staged by tools/mods/stage_mods.cmake and by the POST_BUILD below.
include(${CMAKE_CURRENT_SOURCE_DIR}/net_libs.cmake)

set(MP_OUT "${RS64_BUILT_MODS_DIR}/$<CONFIG>/multiplayer-native")
file(GLOB MP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)
# The only base-game files the mod may use (tests/style_check.cpp enforces this for src/).
set(MP_BASE_FILES
    ${CMAKE_SOURCE_DIR}/src/lockstep/lockstep_core.cpp
    ${CMAKE_SOURCE_DIR}/src/lockstep/lockstep_core.h
    ${CMAKE_SOURCE_DIR}/src/util/debug_logs.h
    ${CMAKE_SOURCE_DIR}/include/os_compat.h
    ${CMAKE_SOURCE_DIR}/src/util/rdram_words.h)
# The relay server's protocol (relay-server/src), shared with rs64_relay.
set(MP_RELAY_FILES ${CMAKE_SOURCE_DIR}/relay-server/src/relay_protocol.cpp ${CMAKE_SOURCE_DIR}/relay-server/src/relay_protocol.h)
add_library(multiplayer_native SHARED ${MP_SOURCES} ${MP_BASE_FILES} ${MP_RELAY_FILES})
# Same rounding as the game's copy of lockstep_core: no fused multiply-add in the flight model.
set(MP_NO_FMA ${CMAKE_SOURCE_DIR}/src/lockstep/lockstep_core.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/mp_core.cpp)
if (MSVC)
    set_source_files_properties(${MP_NO_FMA} PROPERTIES COMPILE_OPTIONS "/clang:-ffp-contract=off")
    target_compile_definitions(multiplayer_native PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    set_source_files_properties(${MP_NO_FMA} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
set_target_properties(multiplayer_native PROPERTIES
    PREFIX ""
    OUTPUT_NAME "multiplayer_native"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${MP_OUT}"
    RUNTIME_OUTPUT_DIRECTORY "${MP_OUT}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    # Keeps the PDB (and the import lib above) out of the staged folder.
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
# Android packs it into the APK's native-library directory, which only takes lib*.so; librecomp falls back to that name there (mods.cpp).
if (ANDROID)
    set_target_properties(multiplayer_native PROPERTIES PREFIX "lib")
endif()
target_include_directories(multiplayer_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/relay-server/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib)
target_link_libraries(multiplayer_native PRIVATE enet6 libminiupnpc-static)
# Linux: every symbol resolves inside the library (nothing from the exe), its own calls bind to itself, and only the two mod entry points are exported (no ENet / miniupnpc or libstdc++ template symbols).
if (UNIX AND NOT APPLE)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/exports.map" "{ global: recomp_api_version; rs64_mod_init; local: *; };\n")
    target_link_options(multiplayer_native PRIVATE -Wl,--no-undefined -Wl,-Bsymbolic -Wl,--exclude-libs,ALL "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/exports.map")
    target_link_libraries(multiplayer_native PRIVATE Threads::Threads)
endif()
# macOS: same as Linux, only the two mod entry points exported and no unresolved symbols.
if (APPLE)
    target_link_options(multiplayer_native PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.macos" -Wl,-undefined,error)
endif()
# mod.json and instruction_patches.json (the per-player data relocation, applied by librecomp at load) come from mod.toml through RecompModTool: the in-tree one, or the host one a cross build passes (Android).
set(MP_MODTOOL_OUT "${CMAKE_CURRENT_BINARY_DIR}/modtool")
if (TARGET RecompModTool AND NOT CMAKE_CROSSCOMPILING)
    set(MP_MODTOOL "$<TARGET_FILE:RecompModTool>")
    set(MP_MODTOOL_DEP RecompModTool)
else()
    set(MP_MODTOOL "${RECOMP_MOD_TOOL}")
    set(MP_MODTOOL_DEP "")
endif()
set(MP_MODTOOL_ARGS --as "${MIPS_GCC}")
if (EXISTS "${RS64_GAME_ROM}")
    list(APPEND MP_MODTOOL_ARGS --rom "${RS64_GAME_ROM}")
endif()
add_custom_command(OUTPUT "${MP_MODTOOL_OUT}/mod.json" "${MP_MODTOOL_OUT}/instruction_patches.json"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MP_MODTOOL_OUT}"
    COMMAND "${MP_MODTOOL}" "${CMAKE_CURRENT_SOURCE_DIR}/mod.toml" "${MP_MODTOOL_OUT}" ${MP_MODTOOL_ARGS}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mod.toml" ${MP_MODTOOL_DEP}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "RecompModTool: multiplayer-native mod.json and instruction_patches.json"
    VERBATIM)
add_custom_target(multiplayer_native_manifest DEPENDS "${MP_MODTOOL_OUT}/mod.json" "${MP_MODTOOL_OUT}/instruction_patches.json")
add_dependencies(multiplayer_native multiplayer_native_manifest)

# The mod folder: the library plus its mod.json, menu and instruction_patches.json.
add_custom_command(TARGET multiplayer_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MP_MODTOOL_OUT}/mod.json ${MP_OUT}/mod.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/roguesq_menu.json ${MP_OUT}/roguesq_menu.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MP_MODTOOL_OUT}/instruction_patches.json ${MP_OUT}/instruction_patches.json)
# Built without the exe relinking (an edit to a multiplayer source only): refresh the staged copy and, for Release on Windows, the dist copy too.
if (NOT ANDROID)
    add_custom_command(TARGET multiplayer_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${MP_OUT} $<TARGET_FILE_DIR:RogueSquadron64Recomp>/mods/multiplayer-native)
    if (WIN32)
        add_custom_command(TARGET multiplayer_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy_directory,true> ${MP_OUT} ${RS64_DIST_DIR}/mods/multiplayer-native)
    endif()
endif()
