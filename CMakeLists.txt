cmake_minimum_required(VERSION 3.20)

if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")
endif()

# The release version lives in VERSION (also read by Gradle and the packaging scripts); editing it re-runs configure.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RS64_VERSION LIMIT_COUNT 1)
string(STRIP "${RS64_VERSION}" RS64_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
project(RogueSquadron64Recomp VERSION ${RS64_VERSION} LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (WIN32)
    add_compile_definitions(NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

# Turns off MSVC debug iterators (and their bounds checks) across the whole build for faster Debug runs. Both defines are needed to keep the libs link-compatible.
option(ROGUESQ_NO_ITER_DEBUG "Disable MSVC debug iterators (skip bounds checks)" OFF)
if(WIN32 AND ROGUESQ_NO_ITER_DEBUG)
    add_compile_definitions(_HAS_ITERATOR_DEBUGGING=0 _ITERATOR_DEBUG_LEVEL=0)
    message(STATUS "MSVC debug iterators DISABLED (vector bounds checks off)")
endif()


if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Android consumes the code the Windows build generated (N64Recomp, RSPRecomp, state table); it cannot run those tools itself.
if (ANDROID)
    foreach(f RecompiledFuncs/funcs.h src/main/state_table.inl
              build/factor5_ucode/factor5_boot_recompiled.c build/factor5_ucode/musyx_audio_recompiled.c)
        if (NOT EXISTS "${CMAKE_SOURCE_DIR}/${f}")
            message(FATAL_ERROR "Android build needs host-generated ${f}; run the Windows Debug build first.")
        endif()
    endforeach()
endif()

# Android and Linux build SDL2 from source; it must exist before RT64 configures. Android ships libSDL2.so for SDLActivity.
# Linux links it statically: SDL then loads its audio and video backends from the host, so packages don't carry the distro's PulseAudio/codec stack.
if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(FetchContent)
    FetchContent_Declare(SDL2 URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz)
    if (ANDROID)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
    endif()
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL2)
    if (NOT TARGET SDL2::SDL2)
        add_library(SDL2::SDL2 ALIAS SDL2-static)
    endif()
endif()
# RT64's file dialogs (inspector only) go through xdg-desktop-portal over D-Bus instead of linking GTK.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NFD_PORTAL ON CACHE BOOL "" FORCE)
endif()

# ---- RT64 renderer ----
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)
# ImGui's D3D12 backend stores 64-bit GPU descriptor handles in ImTextureID.
if (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_compile_definitions(ImTextureID=unsigned\ long\ long)
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

# ---- N64ModernRuntime (ultramodern + librecomp) ----
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)


# ---- RecompiledFuncs ----
# N64Recomp output; gitignored (derived from the ROM). Regenerate with the regen_funcs target.
set(RECOMP_FUNCS_DIR "${CMAKE_SOURCE_DIR}/RecompiledFuncs")

add_library(RecompiledFuncs STATIC)

if (MSVC)
    # Generated code calls *_recomp wrappers declared only in the runtime, and leaves unused scratch locals and per-block labels.
    target_compile_options(RecompiledFuncs PRIVATE /wd4101 /wd4133
        -Wno-implicit-function-declaration
        -Wno-error=implicit-function-declaration
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-unused-label)
    target_compile_definitions(RecompiledFuncs PRIVATE _CRT_SECURE_NO_WARNINGS)
    # mem_wu.h adds the unsigned-word MEM_WU macro that upstream recomp.h lacks.
    target_compile_options(RecompiledFuncs PRIVATE
        /FI${CMAKE_SOURCE_DIR}/include/mem_wu.h)
    # The recompiled code type-puns RDRAM through MEM_*; strict aliasing miscompiles it at -O2.
    target_compile_options(RecompiledFuncs PRIVATE -fno-strict-aliasing)
    target_compile_definitions(RecompiledFuncs PRIVATE main=rs_main)
else()
    target_compile_definitions(RecompiledFuncs PRIVATE main=rs_main)
    target_compile_options(RecompiledFuncs PRIVATE
        -fno-strict-aliasing
        -Wno-unused-variable
        $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>
        -include "${CMAKE_SOURCE_DIR}/include/mem_wu.h"
    )
endif()

target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

# CONFIGURE_DEPENDS picks up files a regen adds or removes without a manual reconfigure.
file(GLOB RECOMP_C_SOURCES   CONFIGURE_DEPENDS ${RECOMP_FUNCS_DIR}/*.c)
file(GLOB RECOMP_CXX_SOURCES CONFIGURE_DEPENDS ${RECOMP_FUNCS_DIR}/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${RECOMP_C_SOURCES} ${RECOMP_CXX_SOURCES})
# The rogue_squadron.toml hook bodies; built with the recompiled code so they compile exactly as they did inline.
target_sources(RecompiledFuncs PRIVATE ${CMAKE_SOURCE_DIR}/src/main/toml_hooks.c)

# ---- N64Recomp ----
if (CMAKE_HOST_WIN32)
    set(RS64_HOST_EXE_SUFFIX ".exe")
endif()
get_property(RS64_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (RS64_MULTI_CONFIG)
    set(RS64_N64RECOMP_DEFAULT "${CMAKE_BINARY_DIR}/Debug/N64Recomp${RS64_HOST_EXE_SUFFIX}")
else()
    set(RS64_N64RECOMP_DEFAULT "${CMAKE_BINARY_DIR}/N64Recomp${RS64_HOST_EXE_SUFFIX}")
endif()
set(N64RECOMP_EXE "${RS64_N64RECOMP_DEFAULT}" CACHE FILEPATH "N64Recomp executable (built from lib/N64ModernRuntime/N64Recomp via N64RecompCLI target)")

# ---- Regenerate RecompiledFuncs (opt-in) ----
# Not in the default build: needs the decomp ELF (elf_path in rogue_squadron.toml). cmake --build build --config Debug --target regen_funcs
add_custom_target(regen_funcs
    COMMAND ${N64RECOMP_EXE} "${CMAKE_SOURCE_DIR}/rogue_squadron.toml"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Regenerating RecompiledFuncs from rogue_squadron.toml (needs decomp ELF + N64Recomp.exe)"
    VERBATIM
)
# Refreshes syms/ (the names .nrm mods build against; the RogueSquadron64RecompSyms submodule) from the decomp ELF. Run after renames, commit in syms/, bump the submodule. cmake --build build --config Debug --target dump_syms
add_custom_target(dump_syms
    COMMAND ${N64RECOMP_EXE} "${CMAKE_SOURCE_DIR}/rogue_squadron.toml" --dump-context
    COMMAND ${CMAKE_COMMAND} -E copy dump.toml "${CMAKE_SOURCE_DIR}/syms/rogue_squadron.syms.toml"
    COMMAND ${CMAKE_COMMAND} -E copy data_dump.toml "${CMAKE_SOURCE_DIR}/syms/rogue_squadron.datasyms.toml"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Dumping syms/ from the decomp ELF"
    VERBATIM
)
# Build the recompiler first so a stale exe can't silently truncate the output.
if (TARGET N64RecompCLI)
    add_dependencies(regen_funcs N64RecompCLI)
    add_dependencies(dump_syms N64RecompCLI)
endif()

# ---- Patches ELF ----
# Base-game C in patches/src: compiled to MIPS with the code-mod toolchain, recompiled by N64Recomp (patches.toml) and registered with librecomp at boot (register_patches.cpp), as in Zelda64Recomp.
include(${CMAKE_SOURCE_DIR}/tools/mods/code_mod.cmake)
if (NOT EXISTS "${MIPS_GCC}" OR NOT EXISTS "${MIPS_LD}")
    message(FATAL_ERROR "The patches build needs MIPS GCC and ld: set MIPS_TOOLCHAIN_DIR (now ${MIPS_TOOLCHAIN_DIR}) or MIPS_GCC and MIPS_LD.")
endif()
set(PATCHES_DIR ${CMAKE_SOURCE_DIR}/patches)
set(PATCHES_OUT ${CMAKE_SOURCE_DIR}/RecompiledPatches)
set(PATCHES_BUILD ${CMAKE_BINARY_DIR}/patches)
file(GLOB_RECURSE PATCH_C_SOURCES CONFIGURE_DEPENDS ${PATCHES_DIR}/src/*.c)
file(GLOB_RECURSE PATCH_HEADERS CONFIGURE_DEPENDS ${PATCHES_DIR}/src/*.h)
set(PATCH_OBJS "")
foreach(src ${PATCH_C_SOURCES})
    file(RELATIVE_PATH rel "${PATCHES_DIR}/src" "${src}")
    string(REPLACE "/" "_" obj_name "${rel}")
    set(obj "${PATCHES_BUILD}/${obj_name}.o")
    add_custom_command(OUTPUT "${obj}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PATCHES_BUILD}"
        COMMAND "${MIPS_GCC}" ${RS64_MIPS_CFLAGS} -c "${src}" -o "${obj}"
        DEPENDS "${src}" ${PATCH_HEADERS} "${RS64_MOD_SDK_DIR}/rs64_api.h"
        VERBATIM)
    list(APPEND PATCH_OBJS "${obj}")
endforeach()
# Game functions stay undefined so N64Recomp resolves them by name through patches.toml's syms; --noinhibit-exec keeps the ELF despite ld's out-of-range jal errors for them.
add_custom_command(OUTPUT ${PATCHES_DIR}/patches.elf
    COMMAND "${MIPS_LD}" -nostdlib -T "${PATCHES_DIR}/patches.ld" -T "${PATCHES_DIR}/syms.ld" -Map "${PATCHES_BUILD}/patches.map"
        --unresolved-symbols=ignore-all --emit-relocs --noinhibit-exec -e 0 ${PATCH_OBJS} -o "${PATCHES_DIR}/patches.elf"
    DEPENDS ${PATCH_OBJS} "${PATCHES_DIR}/patches.ld" "${PATCHES_DIR}/syms.ld"
    VERBATIM)
set(PATCHES_RECOMP_DEPS "")
if (TARGET N64RecompCLI AND N64RECOMP_EXE STREQUAL RS64_N64RECOMP_DEFAULT)
    set(PATCHES_RECOMP_DEPS N64RecompCLI)
endif()
add_custom_command(OUTPUT ${PATCHES_OUT}/patches.c ${PATCHES_OUT}/recomp_overlays.inl ${PATCHES_OUT}/funcs.h ${PATCHES_DIR}/patches.bin
    COMMAND "${N64RECOMP_EXE}" "${CMAKE_SOURCE_DIR}/patches.toml"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS ${PATCHES_DIR}/patches.elf ${CMAKE_SOURCE_DIR}/patches.toml ${CMAKE_SOURCE_DIR}/syms/rogue_squadron.syms.toml ${CMAKE_SOURCE_DIR}/syms/rogue_squadron.datasyms.toml ${PATCHES_RECOMP_DEPS} "${N64RECOMP_EXE}"
    COMMENT "Recompiling patches.elf"
    VERBATIM)
add_custom_command(OUTPUT ${PATCHES_BUILD}/patches_bin.c ${PATCHES_BUILD}/patches_bin.h
    COMMAND file_to_c ${PATCHES_DIR}/patches.bin rs64_patches_bin ${PATCHES_BUILD}/patches_bin.c ${PATCHES_BUILD}/patches_bin.h
    DEPENDS ${PATCHES_DIR}/patches.bin file_to_c
    VERBATIM)
# OBJECT, not STATIC: objects link like the game's own sources, so nothing depends on archive link order.
add_library(PatchesLib OBJECT ${PATCHES_OUT}/patches.c ${PATCHES_BUILD}/patches_bin.c)
target_compile_options(PatchesLib PRIVATE $<TARGET_PROPERTY:RecompiledFuncs,COMPILE_OPTIONS>)
target_compile_definitions(PatchesLib PRIVATE $<TARGET_PROPERTY:RecompiledFuncs,COMPILE_DEFINITIONS>)
target_include_directories(PatchesLib PRIVATE $<TARGET_PROPERTY:RecompiledFuncs,INCLUDE_DIRECTORIES>)

# ---- Main executable ----
# Graphics are HLE through RT64; only the audio RSP ucodes (shared boot + MusyX synth, from rsp/*.toml) are recompiled.
set(FACTOR5_BOOT_C  ${CMAKE_SOURCE_DIR}/build/factor5_ucode/factor5_boot_recompiled.c)
set(MUSYX_AUDIO_C   ${CMAKE_SOURCE_DIR}/build/factor5_ucode/musyx_audio_recompiled.c)
# RSPRecomp output is fixed up in place before it compiles (boot hand-off, MusyX task-complete return); unfixed audio ucode spins forever. Android reuses the host-fixed files.
if (ANDROID)
    foreach(ucode_c ${FACTOR5_BOOT_C} ${MUSYX_AUDIO_C})
        file(READ ${ucode_c} ucode_text)
        string(FIND "${ucode_text}" "/* fixup:" ucode_fixed)
        if (ucode_fixed EQUAL -1)
            message(FATAL_ERROR "${ucode_c} is unfixed; run the Windows Debug build first.")
        endif()
    endforeach()
else()
    add_executable(fixup_factor5_ucode ${CMAKE_SOURCE_DIR}/tools/fixup_factor5_ucode.cpp)
    set_target_properties(fixup_factor5_ucode PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/ucode_fixup.stamp
        COMMAND fixup_factor5_ucode ${FACTOR5_BOOT_C} ${MUSYX_AUDIO_C}
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/ucode_fixup.stamp
        DEPENDS fixup_factor5_ucode ${FACTOR5_BOOT_C} ${MUSYX_AUDIO_C}
        COMMENT "Fixing up the RSPRecomp ucode output")
    add_custom_target(ucode_fixup DEPENDS ${CMAKE_BINARY_DIR}/ucode_fixup.stamp)
endif()
# arm64 maps the RSP vector ops through sse2neon (rsp_vu.hpp), so -msse4.1 is x86-only.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(RSP_UCODE_ARCH_FLAGS "-msse4.1")
endif()
set_source_files_properties(${FACTOR5_BOOT_C} ${MUSYX_AUDIO_C} PROPERTIES
    LANGUAGE CXX
    COMPILE_OPTIONS "${RSP_UCODE_ARCH_FLAGS};-Wno-unused-variable;$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-missing-braces>;$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-shift-op-parentheses>;$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-bitwise-op-parentheses>")

# Generate the game-state table header from the canonical descriptor (state_model.toml).
set(STATE_TABLE_INL ${CMAKE_SOURCE_DIR}/src/main/state_table.inl)
if (ANDROID)
    add_custom_target(gen_state_table)
else()
    add_executable(gen_state_table_tool ${CMAKE_SOURCE_DIR}/tools/state/gen_state_table.cpp)
    set_target_properties(gen_state_table_tool PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(gen_state_table_tool PRIVATE tomlplusplus::tomlplusplus)
    add_custom_command(
        OUTPUT ${STATE_TABLE_INL}
        COMMAND gen_state_table_tool ${CMAKE_SOURCE_DIR}/tools/state/state_model.toml ${STATE_TABLE_INL}
        DEPENDS gen_state_table_tool ${CMAKE_SOURCE_DIR}/tools/state/state_model.toml
        COMMENT "Generating state_table.inl from state_model.toml")
    add_custom_target(gen_state_table DEPENDS ${STATE_TABLE_INL})
endif()

set(RS64_MAIN_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/game_state.cpp
    ${CMAKE_SOURCE_DIR}/src/main/nav_sequencer.cpp
    ${CMAKE_SOURCE_DIR}/src/main/input_bindings.cpp
    ${CMAKE_SOURCE_DIR}/src/main/rumble.cpp
    ${CMAKE_SOURCE_DIR}/src/main/touch_input.cpp
    ${CMAKE_SOURCE_DIR}/src/main/touch_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/main/touch_config.cpp
    ${CMAKE_SOURCE_DIR}/src/main/video_config.cpp
    ${CMAKE_SOURCE_DIR}/src/main/menu_config.cpp
    ${CMAKE_SOURCE_DIR}/src/main/rt64_render_context.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_patches.cpp
    ${CMAKE_SOURCE_DIR}/src/main/upstream_compat.cpp
    ${CMAKE_SOURCE_DIR}/src/main/hook_helpers.cpp
    ${CMAKE_SOURCE_DIR}/src/main/lockstep_core.cpp
    ${CMAKE_SOURCE_DIR}/src/main/lockstep.cpp
    ${CMAKE_SOURCE_DIR}/src/main/renderdoc_capture.cpp
    ${CMAKE_SOURCE_DIR}/src/main/host_api.cpp
    ${CMAKE_SOURCE_DIR}/src/main/host_api_game.cpp
    ${CMAKE_SOURCE_DIR}/src/main/builtin_hooks.cpp
    ${FACTOR5_BOOT_C}
    ${MUSYX_AUDIO_C}
    $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/main/app.rc>
)
# Lockstep simulation math must round identically everywhere: no fused multiply-add.
if (MSVC)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/main/lockstep_core.cpp PROPERTIES COMPILE_OPTIONS "/clang:-ffp-contract=off")
else()
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/main/lockstep_core.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
# Android: the game is libmain.so, loaded by SDL's SDLActivity (which calls SDL_main).
if (ANDROID)
    add_library(RogueSquadron64Recomp SHARED ${RS64_MAIN_SOURCES} ${CMAKE_SOURCE_DIR}/src/main/android_host.cpp)
    set_target_properties(RogueSquadron64Recomp PROPERTIES OUTPUT_NAME main)
    target_link_libraries(RogueSquadron64Recomp PRIVATE android log)
    target_link_options(RogueSquadron64Recomp PRIVATE -Wl,-z,max-page-size=16384)
else()
    add_executable(RogueSquadron64Recomp ${RS64_MAIN_SOURCES})
endif()
add_dependencies(RogueSquadron64Recomp gen_state_table)
if (NOT ANDROID)
    add_dependencies(RogueSquadron64Recomp ucode_fixup)
endif()
if (MSVC)
    target_compile_definitions(RogueSquadron64Recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_compile_definitions(RogueSquadron64Recomp PRIVATE RS64_VERSION_STRING="${RS64_VERSION}")
# The shader combos warmed at startup without an ubershader are compiled into the exe.
set(RS64_SHADER_COMBOS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tools/d3d11/shader_combos.txt")
file(READ "${RS64_SHADER_COMBOS_FILE}" RS64_SHADER_COMBOS)
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/rs64_shader_combos.inl" CONTENT "R\"RS64(@RS64_SHADER_COMBOS@)RS64\"\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RS64_SHADER_COMBOS_FILE}")
target_include_directories(RogueSquadron64Recomp PRIVATE "${CMAKE_BINARY_DIR}/generated")
if (ANDROID)
    target_compile_definitions(RogueSquadron64Recomp PRIVATE SSE2NEON_SUPPRESS_WARNINGS)
    target_compile_options(RogueSquadron64Recomp PRIVATE -Wno-deprecated-literal-operator)
endif()

# Linux executables carry no icon resource: favicon.ico is embedded and set as the window icon at startup.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/favicon_ico.c ${CMAKE_BINARY_DIR}/favicon_ico.h
        COMMAND file_to_c ${CMAKE_SOURCE_DIR}/favicon.ico rs64_favicon_ico ${CMAKE_BINARY_DIR}/favicon_ico.c ${CMAKE_BINARY_DIR}/favicon_ico.h
        DEPENDS ${CMAKE_SOURCE_DIR}/favicon.ico file_to_c
        VERBATIM)
    target_sources(RogueSquadron64Recomp PRIVATE ${CMAKE_BINARY_DIR}/favicon_ico.c)
    target_include_directories(RogueSquadron64Recomp PRIVATE ${CMAKE_BINARY_DIR})
endif()

target_include_directories(RogueSquadron64Recomp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${RECOMP_FUNCS_DIR}
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
)

if (NOT MSVC)
    target_compile_options(RogueSquadron64Recomp PRIVATE
        -fno-strict-aliasing
        -fms-extensions
    )
endif()

if (MSVC)
    target_link_options(RogueSquadron64Recomp PRIVATE /OPT:NOICF)
    # The src/main overrides redefine RecompiledFuncs/runtime symbols; the first definition wins. Masks accidental duplicates too.
    target_link_options(RogueSquadron64Recomp PRIVATE /FORCE:MULTIPLE)
elseif (APPLE)
else()
    # GNU ld equivalent of /FORCE:MULTIPLE.
    target_link_options(RogueSquadron64Recomp PRIVATE -Wl,--allow-multiple-definition)
endif()

# POSIX guest-fault guard test (Windows uses SEH instead): cmake --build <dir> --target fault_guard_test
if (NOT WIN32 AND NOT ANDROID)
    add_executable(fault_guard_test EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/fault_guard_test.cpp)
    target_link_libraries(fault_guard_test PRIVATE librecomp)
    target_include_directories(fault_guard_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include)
endif()

# Touch engine + menu layer unit tests: cmake --build build --config Debug --target touch_input_test
add_executable(touch_input_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/touch_input_test.cpp
    ${CMAKE_SOURCE_DIR}/src/main/touch_input.cpp
    ${CMAKE_SOURCE_DIR}/src/main/touch_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/main/touch_config.cpp
)
set_target_properties(touch_input_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(touch_input_test PRIVATE ${CMAKE_SOURCE_DIR}/src/main ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib)

# Not part of the default build: cmake --build build --config Debug --target video_config_test
add_executable(video_config_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/video_config_test.cpp
    ${CMAKE_SOURCE_DIR}/src/main/video_config.cpp
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/common/rt64_user_configuration.cpp
)
set_target_properties(video_config_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(video_config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
)
if(WIN32)
    # Matches the game, which gets it from plume; without it validate() turns D3D11 into Automatic.
    target_compile_definitions(video_config_test PRIVATE PLUME_D3D11_ENABLED)
endif()

# Standalone unit test for the nav-sequencer target parser.
add_executable(nav_target_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/nav_target_test.cpp
    ${CMAKE_SOURCE_DIR}/src/main/nav_sequencer.cpp
    ${CMAKE_SOURCE_DIR}/src/main/lockstep_core.cpp
    ${CMAKE_SOURCE_DIR}/src/main/game_state.cpp
    ${CMAKE_SOURCE_DIR}/src/main/touch_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/main/host_api.cpp
)
set_target_properties(nav_target_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(nav_target_test PRIVATE ${CMAKE_SOURCE_DIR}/src/main ${CMAKE_SOURCE_DIR}/include)
add_dependencies(nav_target_test gen_state_table)

# Standalone unit test for the lockstep record/replay core.
add_executable(lockstep_core_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/lockstep_core_test.cpp
    ${CMAKE_SOURCE_DIR}/src/main/lockstep_core.cpp
)
set_target_properties(lockstep_core_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(lockstep_core_test PRIVATE ${CMAKE_SOURCE_DIR}/src/main)



# Standalone unit test for the mod host API registry.
add_executable(host_api_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/host_api_test.cpp
    ${CMAKE_SOURCE_DIR}/src/main/host_api.cpp
)
set_target_properties(host_api_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(host_api_test PRIVATE ${CMAKE_SOURCE_DIR}/src/main ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(host_api_test PRIVATE RS64_TOML_PATH="${CMAKE_SOURCE_DIR}/rogue_squadron.toml" RS64_TOML_HOOKS_PATH="${CMAKE_SOURCE_DIR}/src/main/toml_hooks.c")

# Standalone unit test for the tow cable head interpolation alias.
add_executable(cable_alias_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/cable_alias_test.cpp
)
set_target_properties(cable_alias_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Standalone unit test for the crosshair texrect matcher.
add_executable(crosshair_match_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/crosshair_match_test.cpp
)
set_target_properties(crosshair_match_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Standalone unit test for the dynamic point-light helpers.
add_executable(rs64_lights_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/rs64_lights_test.cpp
)
set_target_properties(rs64_lights_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# AGENTS.md style rules for src/main, against tools/style/baseline.txt.
add_executable(style_check EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/style_check.cpp
)
set_target_properties(style_check PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# One command for the unit tests: cmake --build build --config Release --target rs64_unit_tests
add_custom_target(rs64_unit_tests
    COMMAND $<TARGET_FILE:lockstep_core_test>
    COMMAND $<TARGET_FILE:mp_core_test>
    COMMAND $<TARGET_FILE:net_core_test>
    COMMAND $<TARGET_FILE:net_link_test>
    COMMAND $<TARGET_FILE:nav_target_test>
    COMMAND $<TARGET_FILE:host_api_test>
    COMMAND $<TARGET_FILE:cable_alias_test>
    COMMAND $<TARGET_FILE:crosshair_match_test>
    COMMAND $<TARGET_FILE:rs64_lights_test>
    COMMAND $<TARGET_FILE:style_check> ${CMAKE_SOURCE_DIR}
    DEPENDS lockstep_core_test mp_core_test net_core_test net_link_test nav_target_test host_api_test cable_alias_test crosshair_match_test rs64_lights_test style_check
    COMMENT "Running the unit tests")

# Standalone unit test for the mode-change blanking gate.
add_executable(transition_gate_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/transition_gate_test.cpp
)
set_target_properties(transition_gate_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

if (MSVC)
    foreach(t touch_input_test video_config_test nav_target_test lockstep_core_test host_api_test transition_gate_test cable_alias_test crosshair_match_test rs64_lights_test style_check)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endforeach()
endif()

# ---- SDL2 (Windows: fetch; Linux/Mac: find_package) ----
if (WIN32)
    include(FetchContent)
    if (DEFINED ENV{SDL2_VERSION})
        set(SDL2_VERSION $ENV{SDL2_VERSION})
    else()
        set(SDL2_VERSION "2.30.3")
    endif()
    FetchContent_Declare(sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(RS64_WIN_ARCH x86)
        # 4GB of address space on a 64-bit OS instead of 2GB; RDRAM alone takes 512MB.
        target_link_options(RogueSquadron64Recomp PRIVATE /LARGEADDRESSAWARE)
    else()
        set(RS64_WIN_ARCH x64)
    endif()
    target_include_directories(RogueSquadron64Recomp PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(RogueSquadron64Recomp PRIVATE ${sdl2_SOURCE_DIR}/lib/${RS64_WIN_ARCH})
    add_custom_command(TARGET RogueSquadron64Recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/${RS64_WIN_ARCH}/SDL2.dll"
            "${RT64_DXC_BIN_DIR}/dxil.dll"
            "${RT64_DXC_BIN_DIR}/dxcompiler.dll"
            $<TARGET_FILE_DIR:RogueSquadron64Recomp>)
    set_target_properties(RogueSquadron64Recomp PROPERTIES
        LINK_FLAGS_DEBUG          "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE        "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
    target_link_libraries(RogueSquadron64Recomp PRIVATE SDL2 Winmm.lib)
    # SDL_MAIN_HANDLED with our own main(); SDL2main would demand an SDL_main we don't define.
    target_link_options(RogueSquadron64Recomp PRIVATE /NODEFAULTLIB:SDL2main.lib)
elseif (ANDROID)
    find_package(Threads REQUIRED)
    target_link_libraries(RogueSquadron64Recomp PRIVATE SDL2::SDL2 Threads::Threads)
elseif (APPLE)
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_include_directories(RogueSquadron64Recomp PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(RogueSquadron64Recomp PRIVATE ${CMAKE_DL_LIBS} Threads::Threads SDL2::SDL2)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(RogueSquadron64Recomp PRIVATE SDL2::SDL2 ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# Input-bindings resolver test against an SDL virtual joystick: cmake --build build --config Debug --target input_bindings_test
if (NOT ANDROID)
    add_executable(input_bindings_test EXCLUDE_FROM_ALL
        ${CMAKE_SOURCE_DIR}/tests/input_bindings_test.cpp
        ${CMAKE_SOURCE_DIR}/src/main/input_bindings.cpp
    )
    set_target_properties(input_bindings_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_include_directories(input_bindings_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib)
    if (WIN32)
        target_include_directories(input_bindings_test PRIVATE ${sdl2_SOURCE_DIR}/include)
        target_link_directories(input_bindings_test PRIVATE ${sdl2_SOURCE_DIR}/lib/${RS64_WIN_ARCH})
        target_link_libraries(input_bindings_test PRIVATE SDL2)
        target_link_options(input_bindings_test PRIVATE /NODEFAULTLIB:SDL2main.lib)
        add_custom_command(TARGET input_bindings_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sdl2_SOURCE_DIR}/lib/${RS64_WIN_ARCH}/SDL2.dll" $<TARGET_FILE_DIR:input_bindings_test>)
    else()
        target_include_directories(input_bindings_test PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(input_bindings_test PRIVATE SDL2::SDL2)
    endif()
endif()

if (WIN32)
    set(RS64_DIST_DIR "${CMAKE_BINARY_DIR}/dist")
endif()
# Native mods the game's CMake builds, one folder per config; stage_mods.cmake stages a listed mod from here before mods/.
set(RS64_BUILT_MODS_DIR "${CMAKE_BINARY_DIR}/built_mods" CACHE PATH "Where native mods' finished folders go (Android's Gradle build points it at its own build dir to bundle them)")
add_subdirectory(${CMAKE_SOURCE_DIR}/mods/multiplayer-native ${CMAKE_BINARY_DIR}/multiplayer-native EXCLUDE_FROM_ALL)
add_dependencies(RogueSquadron64Recomp multiplayer_native)
# The steam-relay mod builds only against a local Steamworks SDK (headers only; never committed). Without one, mods/platforms.json's steam-relay entry is skipped with a warning.
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/lib/steamworks/sdk" CACHE PATH "Steamworks SDK folder (holds public/steam/steam_api_flat.h)")
if (NOT ANDROID AND EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api_flat.h")
    add_subdirectory(${CMAKE_SOURCE_DIR}/mods/steam-relay ${CMAKE_BINARY_DIR}/steam-relay EXCLUDE_FROM_ALL)
    add_dependencies(RogueSquadron64Recomp steam_relay)
elseif (NOT ANDROID)
    message(STATUS "steam-relay mod not built: no Steamworks SDK at ${STEAMWORKS_SDK_DIR}")
endif()

# Stage the desktop mods listed in mods/platforms.json next to the exe. The user's own mod folders there are left alone.
# Native mods built by this CMake (multiplayer-native) are staged from build/built_mods/<Config>; a relink restages here, a rebuild of the library alone restages from its own POST_BUILD.
if (NOT ANDROID)
add_custom_command(TARGET RogueSquadron64Recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/mods -DDST=$<TARGET_FILE_DIR:RogueSquadron64Recomp>/mods -DPLATFORM=desktop
        -DBUILT=${RS64_BUILT_MODS_DIR}/$<CONFIG>
        -P "${CMAKE_SOURCE_DIR}/tools/mods/stage_mods.cmake")
endif()

target_include_directories(RogueSquadron64Recomp PRIVATE ${PATCHES_BUILD})
target_link_libraries(RogueSquadron64Recomp PRIVATE
    PatchesLib
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
)

set_property(TARGET RogueSquadron64Recomp PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# ---- Release packaging ----
# Submodule build tools and .libs land next to the exe; Release copies only what ships into build/dist.
if (WIN32)
    add_custom_command(TARGET RogueSquadron64Recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,make_directory,true> "${RS64_DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy_if_different,true>
            "$<TARGET_FILE:RogueSquadron64Recomp>"
            "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/SDL2.dll"
            "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/dxil.dll"
            "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/dxcompiler.dll"
            "${RS64_DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/mods -DDST=$<IF:$<CONFIG:Release>,${RS64_DIST_DIR}/mods,> -DPLATFORM=desktop
            -DBUILT=${RS64_BUILT_MODS_DIR}/$<CONFIG>
            -P "${CMAKE_SOURCE_DIR}/tools/mods/stage_mods.cmake"
        COMMENT "Assembling clean Release folder in ${RS64_DIST_DIR} (Release only)"
        VERBATIM)
endif()

# Opt-level bisect: targets listed here compile /Od in RelWithDebInfo.
set(RS64_OD_TARGETS "" CACHE STRING "Targets compiled /Od in RelWithDebInfo (opt-level bisect)")
foreach(t IN LISTS RS64_OD_TARGETS)
    target_compile_options(${t} PRIVATE $<$<CONFIG:RelWithDebInfo>:/Od /Ob0>)
endforeach()
set(RS64_OD_SOURCES "" CACHE STRING "RecompiledFuncs sources (basenames) compiled /Od in RelWithDebInfo")
foreach(s IN LISTS RS64_OD_SOURCES)
    set_source_files_properties("${CMAKE_SOURCE_DIR}/RecompiledFuncs/${s}" TARGET_DIRECTORY RecompiledFuncs
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:RelWithDebInfo>:/Od;/Ob0>")
endforeach()
