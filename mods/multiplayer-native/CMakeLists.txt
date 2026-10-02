# Online co-op as a native mod: the game's multiplayer sources built into multiplayer_native.dll / .so, which reach the game only through the host API table.
# Added by the game's CMake (needs its enet6 and libminiupnpc-static); one folder per config, staged by tools/mods/stage_mods.cmake and by the POST_BUILD below.
set(MP_OUT "${RS64_BUILT_MODS_DIR}/$<CONFIG>/multiplayer-native")
add_library(multiplayer_native SHARED ${CMAKE_CURRENT_SOURCE_DIR}/mp_mod.cpp ${RS64_MP_SOURCES} ${CMAKE_SOURCE_DIR}/src/main/lockstep_core.cpp)
# Same rounding as the game's copy: no fused multiply-add in the flight model.
if (MSVC)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/main/lockstep_core.cpp PROPERTIES COMPILE_OPTIONS "/clang:-ffp-contract=off")
    target_compile_definitions(multiplayer_native PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/main/lockstep_core.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
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
target_include_directories(multiplayer_native PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/main
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
# The mod folder: the library plus its mod.json and the menu (one source, mods/multiplayer/roguesq_menu.json).
add_custom_command(TARGET multiplayer_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/mod.json ${MP_OUT}/mod.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/mods/multiplayer/roguesq_menu.json ${MP_OUT}/roguesq_menu.json)
# Built without the exe relinking (an edit to a multiplayer source only): refresh the staged copy and, for Release on Windows, the dist copy too.
if (NOT RS64_MULTIPLAYER)
    add_custom_command(TARGET multiplayer_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${MP_OUT} $<TARGET_FILE_DIR:RogueSquadron64Recomp>/mods/multiplayer-native)
    if (WIN32)
        add_custom_command(TARGET multiplayer_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy_directory,true> ${MP_OUT} ${RS64_DIST_DIR}/mods/multiplayer-native)
    endif()
endif()
