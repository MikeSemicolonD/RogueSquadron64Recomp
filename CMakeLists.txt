cmake_minimum_required(VERSION 3.20)

if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")
endif()

project(RogueSquadron64Recomp LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (WIN32)
    add_compile_definitions(NOMINMAX _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

# Disable MSVC's debug iterator bounds checks. Currently there's a
# `vector subscript out of range` assert deep in RT64 when Factor 5's
# ucode generates non-standard RDP byte sequences. The assert fires in
# Debug builds even with our CRT_REPORT hook returning 1 because
# _STL_VERIFY calls abort() unconditionally after the report. With
# bounds checks off, the vector returns garbage and execution continues.
# Trade-off: we lose bounds-check safety nets across the whole build.
# Enable via -DROGUESQ_NO_ITER_DEBUG=ON. Both defines are required to
# avoid linker incompatibility with already-compiled debug libs.
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

# ---- RT64 renderer ----
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)
# Enable D3D12 debug layer to get readable error messages from DirectX
# instead of cryptic crashes deep in D3D12GetInterface. RT64's plume layer
# checks for D3D12_DEBUG_LAYER_ENABLED at compile time. Set via
# -DROGUESQ_DX12_DEBUG=ON when configuring CMake to enable.
option(ROGUESQ_DX12_DEBUG "Enable D3D12 debug layer in RT64" OFF)
if(ROGUESQ_DX12_DEBUG)
    # plume defines the BREAK_ON_* companion macros only under !NDEBUG, so the enable
    # flag must be Debug-only — setting it in Release breaks the plume D3D12 build.
    add_compile_definitions($<$<CONFIG:Debug>:D3D12_DEBUG_LAYER_ENABLED>)
    message(STATUS "D3D12 debug layer ENABLED (Debug config only)")
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

# ---- N64ModernRuntime (ultramodern + librecomp) ----
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# ---- RecompiledFuncs ----
# N64Recomp output. Gitignored (derivative of the copyrighted ROM; generated
# locally, never committed). Regenerate with the `regen_funcs` target.
set(RECOMP_FUNCS_DIR "${CMAKE_SOURCE_DIR}/RecompiledFuncs")

add_library(RecompiledFuncs STATIC)

if (MSVC)
    target_compile_options(RecompiledFuncs PRIVATE /wd4101 /wd4133)
    # ClangCL: the recompiler emits calls to libultra *_recomp wrappers whose
    # declarations live in the runtime, not funcs.h. Treat implicit decls as a
    # warning instead of an error (plain MSVC cl ignores this flag).
    target_compile_options(RecompiledFuncs PRIVATE
        -Wno-implicit-function-declaration
        -Wno-error=implicit-function-declaration
        # clang-cl: the recompiler emits scratch locals (hi/lo/result/c1cs) and
        # goto labels that many generated functions never use. /wd4101 only
        # covers MSVC C4101; clang needs -Wno-unused-variable for unread locals,
        # -Wno-unused-but-set-variable for ones only assigned (hi/lo/result), and
        # -Wno-unused-label for the L_ labels emitted per basic block.
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-unused-label)
    # Generated probes call getenv/atoi. Opt out of the secure-CRT deprecation
    # specifically (keeps -Wdeprecated-declarations live for real deprecations).
    target_compile_definitions(RecompiledFuncs PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Force-include our MEM_WU macro header (fills the unsigned-word gap in
    # upstream recomp.h without modifying it). See include/rsp_dpc_macros.h.
    target_compile_options(RecompiledFuncs PRIVATE
        /FI${CMAKE_SOURCE_DIR}/include/rsp_dpc_macros.h)
    # clang-cl enables TBAA at -O2; the recompiled code type-puns RDRAM through
    # MEM_* constantly, so strict aliasing miscompiles it in Release (garbage
    # framebuffer pointers in the built DL). Match the non-MSVC branch.
    target_compile_options(RecompiledFuncs PRIVATE -fno-strict-aliasing)
    # Rename game's 'main' to avoid MSVC treating it as the entry point
    target_compile_definitions(RecompiledFuncs PRIVATE main=rs_main)
else()
    target_compile_definitions(RecompiledFuncs PRIVATE main=rs_main)
    target_compile_options(RecompiledFuncs PRIVATE
        -fno-strict-aliasing
        -Wno-unused-variable
        -Wno-implicit-function-declaration
        # Match the MSVC branch's force-include: supplies MEM_WU (unsigned-word
        # RDRAM access) that upstream recomp.h omits. GCC/Clang syntax is
        # `-include <file>` (two tokens), not clang-cl's glued /FI.
        -include "${CMAKE_SOURCE_DIR}/include/rsp_dpc_macros.h"
    )
endif()

target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

# CONFIGURE_DEPENDS: re-glob on build so a regen_funcs run that adds/removes a
# file (e.g. lookup.cpp) is picked up without a manual reconfigure.
file(GLOB RECOMP_C_SOURCES   CONFIGURE_DEPENDS ${RECOMP_FUNCS_DIR}/*.c)
file(GLOB RECOMP_CXX_SOURCES CONFIGURE_DEPENDS ${RECOMP_FUNCS_DIR}/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${RECOMP_C_SOURCES} ${RECOMP_CXX_SOURCES})

# ---- Patches (Zelda-style override layer) ----
# Hand-written C in patches/*.c is cross-compiled to MIPS by the decompals
# mips64-elf-gcc toolchain, then run back through N64Recomp to produce
# RecompiledPatches/patches.c. Symbols defined there override the same names
# in RecompiledFuncs because PatchesLib gets linked first. Pattern adapted
# from Zelda64Recompiled — see docs/audit-changes.md Tier 4 + patches/README.md.

set(PATCHES_DIR             "${CMAKE_SOURCE_DIR}/patches")
set(RECOMPILED_PATCHES_DIR  "${CMAKE_SOURCE_DIR}/RecompiledPatches")

# Toolchain locations. Override on the cmake command line if installed elsewhere:
#   cmake -DMIPS_TOOLCHAIN_DIR=C:/mips-toolchain ...
set(MIPS_TOOLCHAIN_DIR "E:/mips-toolchain" CACHE PATH "Root of mips64-elf toolchain (decompals/n64-tools).")
set(MIPS_GCC           "${MIPS_TOOLCHAIN_DIR}/bin/mips64-elf-gcc.exe")
set(MIPS_LD            "${MIPS_TOOLCHAIN_DIR}/bin/mips64-elf-ld.exe")
set(N64RECOMP_EXE      "${CMAKE_BINARY_DIR}/Debug/N64Recomp.exe" CACHE FILEPATH "N64Recomp executable (built from lib/N64ModernRuntime/N64Recomp via N64RecompCLI target)")
find_program(PATCHES_MAKE_EXE NAMES mingw32-make make)

if (EXISTS "${MIPS_GCC}" AND EXISTS "${N64RECOMP_EXE}" AND PATCHES_MAKE_EXE)
    file(GLOB PATCH_C_SOURCES "${PATCHES_DIR}/*.c")

    # Step 1: cross-compile *.c → patches.elf (MIPS)
    add_custom_command(OUTPUT "${PATCHES_DIR}/patches.elf"
        COMMAND ${PATCHES_MAKE_EXE} CC=${MIPS_GCC} LD=${MIPS_LD}
        WORKING_DIRECTORY "${PATCHES_DIR}"
        DEPENDS ${PATCH_C_SOURCES}
                "${PATCHES_DIR}/Makefile"
                "${PATCHES_DIR}/patches.ld"
                "${PATCHES_DIR}/syms.ld"
        COMMENT "Cross-compiling patches.elf (MIPS)"
        VERBATIM
    )

    # Step 2: N64Recomp the elf → RecompiledPatches/patches.c
    add_custom_command(OUTPUT "${RECOMPILED_PATCHES_DIR}/patches.c"
                              "${RECOMPILED_PATCHES_DIR}/funcs.h"
                              "${RECOMPILED_PATCHES_DIR}/recomp_overlays.inl"
        COMMAND ${N64RECOMP_EXE} "${CMAKE_SOURCE_DIR}/patches.toml"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS "${PATCHES_DIR}/patches.elf" "${CMAKE_SOURCE_DIR}/patches.toml"
        COMMENT "Recompiling patches.elf → RecompiledPatches/patches.c"
        VERBATIM
    )

    # Step 3: build PatchesLib that wraps the recompiled output.
    # OBJECT (not STATIC): CMake splices its .obj directly onto the exe link
    # line, so the patch symbols are strong command-line-object definitions.
    # A .lib-vs-.lib pair is the documented failure mode -- /FORCE:MULTIPLE does
    # NOT reliably let a static-lib override win an address-of reference (the
    # func_map's &getNpcCurrentHealth bound to the RecompiledFuncs body instead,
    # so the indirect LOOKUP_FUNC call reached the unguarded recompiled version
    # and AV'd). An object beats an archive member regardless of link order.
    add_library(PatchesLib OBJECT "${RECOMPILED_PATCHES_DIR}/patches.c")
    set_source_files_properties("${RECOMPILED_PATCHES_DIR}/patches.c"
        PROPERTIES GENERATED TRUE)

    target_include_directories(PatchesLib PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${RECOMPILED_PATCHES_DIR}
        ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
        ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
        ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    )
    if (MSVC)
        target_compile_options(PatchesLib PRIVATE /wd4101 /wd4133
            -Wno-implicit-function-declaration
            -Wno-error=implicit-function-declaration
            # clang-cl: same generated-code noise as RecompiledFuncs. /wd4101 is
            # MSVC-only; clang needs these three for the scratch locals, the
            # set-but-unused hi/lo/result, and the per-block L_ labels.
            -Wno-unused-variable
            -Wno-unused-but-set-variable
            -Wno-unused-label)
        target_compile_definitions(PatchesLib PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(PatchesLib PRIVATE
            -fno-strict-aliasing -Wno-unused-variable -Wno-implicit-function-declaration)
    endif()
    target_compile_definitions(PatchesLib PRIVATE main=rs_main)

    set(PATCHES_BUILD_OK TRUE)
else()
    set(PATCHES_BUILD_OK FALSE)
    message(STATUS "Patches build skipped:")
    if (NOT EXISTS "${MIPS_GCC}")
        message(STATUS "  - mips64-elf-gcc not at ${MIPS_GCC}")
    endif()
    if (NOT EXISTS "${N64RECOMP_EXE}")
        message(STATUS "  - N64Recomp.exe not at ${N64RECOMP_EXE}")
    endif()
    if (NOT PATCHES_MAKE_EXE)
        message(STATUS "  - mingw32-make / make not on PATH")
    endif()
    message(STATUS "  See patches/README.md for setup; rebuild without patches for now.")
endif()

# ---- Regenerate RecompiledFuncs (opt-in) ----
# Runs N64Recomp on the in-repo rogue_squadron.toml, regenerating RecompiledFuncs/
# from the decomp ELF (elf_path in the toml -> ../rogue_squadron64/build). Not in
# the default build: regen is heavy and needs the ELF. Run explicitly, then re-run
# cmake configure (RecompiledFuncs are globbed at configure time):
#   cmake --build build --config Debug --target regen_funcs
add_custom_target(regen_funcs
    COMMAND ${N64RECOMP_EXE} "${CMAKE_SOURCE_DIR}/rogue_squadron.toml"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Regenerating RecompiledFuncs from rogue_squadron.toml (needs decomp ELF + N64Recomp.exe)"
    VERBATIM
)
# Build the recompiler first so a stale exe can't silently truncate the output.
if (TARGET N64RecompCLI)
    add_dependencies(regen_funcs N64RecompCLI)
endif()

# ---- Main executable ----
# Hybrid graphics pipeline (2026-05-09):
#   - HLE side: RT64's Factor 5 GBI module (lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp)
#     handles standard F3D-derived opcodes (TEXRECT, FILLRECT, sync ops, etc.)
#     when the HLE path is invoked.
#   - LLE side: factor5_ucode_recompiled.c (auto-translated by RSPRecomp from the
#     game's RSP graphics ucode) handles vertex transform / clip / project.
#     Triangles get emitted as raw RDP bytes via DPC_END writes; src/rsp/
#     dpc_bridge.cpp forwards them into RT64 via submit_rdp_range.
#
set(FACTOR5_UCODE_C ${CMAKE_SOURCE_DIR}/build/factor5_ucode/factor5_ucode_recompiled.c)
set(FACTOR5_BOOT_C  ${CMAKE_SOURCE_DIR}/build/factor5_ucode/factor5_boot_recompiled.c)
# MusyX audio synthesis ucode (RSPRecomp'd from ROM 0x9ABE0 via musyx_rsp.toml).
# Replaces the no-audio musyx_stub when ROGUESQ_AUDIO_UCODE=1. See
# project_audio_state_2026_05_24.md.
set(MUSYX_AUDIO_C   ${CMAKE_SOURCE_DIR}/build/factor5_ucode/musyx_audio_recompiled.c)
# Post-process: RSPRecomp emits a goto to a non-existent label for one
# never-taken branch near end-of-IMEM (perspective-divide reciprocal block).
# tools/fixup_factor5_ucode.py rewrites those gotos to clean exits. The
# fixup runs as part of CMake config so it survives any future regen.
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND AND EXISTS ${FACTOR5_UCODE_C})
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/fixup_factor5_ucode.py ${FACTOR5_UCODE_C}
        RESULT_VARIABLE FACTOR5_FIXUP_RESULT
    )
endif()
# Same fixup, applied to factor5_boot_recompiled.c. The boot ucode hands off
# to the main ucode via `jr $7=0x1080`, which RSPRecomp can't model — without
# this fixup, every task triggers a 4-line printf register dump. The runner
# already treats UnhandledJumpTarget from boot as expected, so the printf is
# pure spam (and at ~3 tasks/sec it can starve the console pump).
if (Python3_Interpreter_FOUND AND EXISTS ${FACTOR5_BOOT_C})
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/fixup_factor5_ucode.py ${FACTOR5_BOOT_C}
        RESULT_VARIABLE FACTOR5_BOOT_FIXUP_RESULT
    )
endif()
# Same unhandled-jump-goto fixup for the MusyX audio ucode.
if (Python3_Interpreter_FOUND AND EXISTS ${MUSYX_AUDIO_C})
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/fixup_factor5_ucode.py ${MUSYX_AUDIO_C}
        RESULT_VARIABLE MUSYX_AUDIO_FIXUP_RESULT
    )
endif()
# Force-include rsp_dpc_macros.h (RSP DPC MMIO bridge macros). clang-cl wants
# the glued /FI form; GCC/Clang want `-include <file>` as two tokens.
if (MSVC)
    set(RSP_UCODE_FORCE_INCLUDE "/FI${CMAKE_SOURCE_DIR}/include/rsp_dpc_macros.h")
else()
    set(RSP_UCODE_FORCE_INCLUDE "-include;${CMAKE_SOURCE_DIR}/include/rsp_dpc_macros.h")
endif()
set_source_files_properties(${FACTOR5_UCODE_C} ${FACTOR5_BOOT_C} ${MUSYX_AUDIO_C} PROPERTIES
    LANGUAGE CXX
    COMPILE_OPTIONS "-msse4.1;${RSP_UCODE_FORCE_INCLUDE}")

# Generate the game-state table header from the canonical descriptor (state_model.toml).
set(STATE_TABLE_INL ${CMAKE_SOURCE_DIR}/src/main/state_table.inl)
if (Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${STATE_TABLE_INL}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/state/gen_state_table.py
                ${CMAKE_SOURCE_DIR}/state_model.toml ${STATE_TABLE_INL}
        DEPENDS ${CMAKE_SOURCE_DIR}/state_model.toml
                ${CMAKE_SOURCE_DIR}/tools/state/gen_state_table.py
                ${CMAKE_SOURCE_DIR}/tools/state/state_model.py
        COMMENT "Generating state_table.inl from state_model.toml")
    add_custom_target(gen_state_table DEPENDS ${STATE_TABLE_INL})
endif()

add_executable(RogueSquadron64Recomp
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/game_state.cpp
    ${CMAKE_SOURCE_DIR}/src/main/nav_sequencer.cpp
    ${CMAKE_SOURCE_DIR}/src/main/input_bindings.cpp
    ${CMAKE_SOURCE_DIR}/src/main/video_config.cpp
    ${CMAKE_SOURCE_DIR}/src/main/menu_config.cpp
    ${CMAKE_SOURCE_DIR}/src/main/rt64_render_context.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/upstream_compat.cpp
    ${CMAKE_SOURCE_DIR}/src/main/hook_helpers.cpp
    ${CMAKE_SOURCE_DIR}/src/rsp/aspMain.cpp
    ${CMAKE_SOURCE_DIR}/src/rsp/dpc_bridge.cpp
    ${FACTOR5_BOOT_C}
    ${FACTOR5_UCODE_C}
    ${MUSYX_AUDIO_C}
    $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/main/app.rc>
)
if (TARGET gen_state_table)
    add_dependencies(RogueSquadron64Recomp gen_state_table)
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
    # /FORCE:MULTIPLE lets PatchesLib's overrides win against the same-named
    # auto-generated functions in RecompiledFuncs. lld-link processes libs in
    # command-line order and takes the first definition; for our symbols
    # PatchesLib comes first in the link line. Without this flag the linker
    # treats the second definition as a fatal error. The risk: any genuinely
    # accidental duplicate elsewhere is masked. If that becomes a problem we
    # can switch to /FORCE:MULTIPLE only on the patched-symbol set via a
    # per-symbol .def or by carving out the patched .obj from RecompiledFuncs.
    target_link_options(RogueSquadron64Recomp PRIVATE /FORCE:MULTIPLE)
else()
    # GNU ld equivalent of /FORCE:MULTIPLE. src/main overrides (osVi*_recomp in
    # upstream_compat.cpp, submit_rdp_range in rt64_render_context.cpp, and the
    # PatchesLib symbols) intentionally redefine symbols that also live in the
    # librecomp/ultramodern archives. Command-line objects come first, so the
    # override wins; without this ld treats the duplicate as fatal.
    target_link_options(RogueSquadron64Recomp PRIVATE -Wl,--allow-multiple-definition)
endif()

# Standalone unit test for the friendly video-config translation layer.
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

# Standalone unit test for the nav-sequencer target parser.
add_executable(nav_target_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/nav_target_test.cpp
    ${CMAKE_SOURCE_DIR}/src/main/nav_sequencer.cpp
    ${CMAKE_SOURCE_DIR}/src/main/game_state.cpp
)
set_target_properties(nav_target_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(nav_target_test PRIVATE ${CMAKE_SOURCE_DIR}/src/main)
if (TARGET gen_state_table)
    add_dependencies(nav_target_test gen_state_table)
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
    target_include_directories(RogueSquadron64Recomp PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(RogueSquadron64Recomp PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)
    add_custom_command(TARGET RogueSquadron64Recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:RogueSquadron64Recomp>)
    set_target_properties(RogueSquadron64Recomp PROPERTIES
        LINK_FLAGS_DEBUG          "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE        "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
    target_link_libraries(RogueSquadron64Recomp PRIVATE SDL2 Winmm.lib)
    # We define SDL_MAIN_HANDLED and provide our own main() under /SUBSYSTEM:CONSOLE, so
    # SDL2main (pulled in transitively) must not be linked: its SDL_windows_main.obj
    # references SDL_main, which we intentionally do not provide.
    target_link_options(RogueSquadron64Recomp PRIVATE /NODEFAULTLIB:SDL2main.lib)
elseif (APPLE)
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_include_directories(RogueSquadron64Recomp PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(RogueSquadron64Recomp PRIVATE ${CMAKE_DL_LIBS} Threads::Threads SDL2::SDL2)
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_include_directories(RogueSquadron64Recomp PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(RogueSquadron64Recomp PRIVATE SDL2::SDL2 ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# Ship the user mods tree next to the exe. Each subfolder of mods/ is a mod;
# menu_config.cpp scans them for a roguesq_menu.json. copy_directory refreshes them
# without deleting the user's own mod folders. Native mods build separately with
# their own mods/<name>/CMakeLists.txt, and whatever DLL that left beside mod.json
# is copied along; the game build never compiles them. The example mods (native-demo,
# menu-example) are stripped from the staged tree, as are code-mod source folders (any
# mods/<name>/ with a mod.toml; their .nrm is built into mods/ and staged on its own).
file(GLOB RS64_CODE_MOD_TOMLS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/mods/*/mod.toml")
set(RS64_CODE_MOD_DIRS "")
foreach(toml ${RS64_CODE_MOD_TOMLS})
    get_filename_component(dir "${toml}" DIRECTORY)
    get_filename_component(name "${dir}" NAME)
    list(APPEND RS64_CODE_MOD_DIRS "${name}")
endforeach()
list(TRANSFORM RS64_CODE_MOD_DIRS PREPEND "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/mods/" OUTPUT_VARIABLE RS64_STAGED_CODE_MOD_DIRS)
add_custom_command(TARGET RogueSquadron64Recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/mods"
        $<TARGET_FILE_DIR:RogueSquadron64Recomp>/mods
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/mods/native-demo"
        "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/mods/menu-example"
        ${RS64_STAGED_CODE_MOD_DIRS})

# PatchesLib is an OBJECT library: its objects go straight onto the exe link
# line ahead of RecompiledFuncs (the .lib), so the patch definitions win any
# duplicated symbol via /FORCE:MULTIPLE (object beats archive member).
if (PATCHES_BUILD_OK)
    target_link_libraries(RogueSquadron64Recomp PRIVATE PatchesLib)
endif()

target_link_libraries(RogueSquadron64Recomp PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
)

set_property(TARGET RogueSquadron64Recomp PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# ---- Release packaging ----
# CMAKE_RUNTIME_OUTPUT_DIRECTORY points every executable target at build/<Config>,
# so the submodule build tools (file_to_c, re-spirv-cli, texture_hasher/packer,
# zstd) and the intermediate .libs all land next to the game exe. None are runtime
# dependencies. For Release, assemble a clean folder holding only what ships.
if (WIN32)
    set(RS64_DIST_DIR "${CMAKE_BINARY_DIR}/dist")
    list(TRANSFORM RS64_CODE_MOD_DIRS PREPEND "${RS64_DIST_DIR}/mods/" OUTPUT_VARIABLE RS64_DIST_CODE_MOD_DIRS)
    add_custom_command(TARGET RogueSquadron64Recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,make_directory,true> "${RS64_DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy_if_different,true>
            "$<TARGET_FILE:RogueSquadron64Recomp>"
            "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/SDL2.dll"
            "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/dxil.dll"
            "$<TARGET_FILE_DIR:RogueSquadron64Recomp>/dxcompiler.dll"
            "${RS64_DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,copy_directory,true>
            "${CMAKE_SOURCE_DIR}/mods"
            "${RS64_DIST_DIR}/mods"
        COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Release>,rm,true> -rf ${RS64_DIST_CODE_MOD_DIRS}
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
