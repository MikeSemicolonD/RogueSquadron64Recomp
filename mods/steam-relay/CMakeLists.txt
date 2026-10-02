# The steam-relay native mod: multiplayer's "mp.transport" service over Steam (app id 480). Compiles against a local Steamworks SDK's headers (STEAMWORKS_SDK_DIR) and links nothing from it; the player supplies the runtime library.
# Added by the game's CMake only when the SDK is present; one folder per config, staged by tools/mods/stage_mods.cmake and by the POST_BUILD below.
set(SR_OUT "${RS64_BUILT_MODS_DIR}/$<CONFIG>/steam-relay")
add_library(steam_relay SHARED ${CMAKE_CURRENT_SOURCE_DIR}/steam_mod.cpp)
set_target_properties(steam_relay PROPERTIES
    PREFIX ""
    OUTPUT_NAME "steam_relay"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${SR_OUT}"
    RUNTIME_OUTPUT_DIRECTORY "${SR_OUT}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
target_include_directories(steam_relay PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${STEAMWORKS_SDK_DIR}/public)
# The pinned SDK version (the first "vX.YY" line of the SDK's Readme.txt), named in the mod's log when a player's library is missing or from another SDK.
file(STRINGS "${STEAMWORKS_SDK_DIR}/Readme.txt" SR_SDK_LINE REGEX "^v[0-9]+\\.[0-9]+" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+\\.[0-9]+" SR_SDK_VERSION "${SR_SDK_LINE}")
if (NOT SR_SDK_VERSION)
    set(SR_SDK_VERSION "unknown")
endif()
target_compile_definitions(steam_relay PRIVATE RS64_STEAM_SDK_VERSION="${SR_SDK_VERSION}")
if (MSVC)
    target_compile_definitions(steam_relay PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if (UNIX AND NOT APPLE)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/exports.map" "{ global: recomp_api_version; rs64_mod_init; local: *; };\n")
    target_link_options(steam_relay PRIVATE -Wl,--no-undefined -Wl,-Bsymbolic "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/exports.map")
    target_link_libraries(steam_relay PRIVATE ${CMAKE_DL_LIBS})
endif()
if (APPLE)
    target_link_options(steam_relay PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.macos" -Wl,-undefined,error)
endif()
add_custom_command(TARGET steam_relay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/mod.json ${SR_OUT}/mod.json)
# Built without the exe relinking: refresh the staged copy (the player's steam_api library already there is left alone) and, for Release on Windows, the dist copy too.
add_custom_command(TARGET steam_relay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SR_OUT} $<TARGET_FILE_DIR:RogueSquadron64Recomp>/mods/steam-relay)
if (WIN32)
    add_custom_command(TARGET steam_relay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy_directory,true> ${SR_OUT} ${RS64_DIST_DIR}/mods/steam-relay)
endif()
