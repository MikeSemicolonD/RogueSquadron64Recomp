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
    add_compile_definitions(D3D12_DEBUG_LAYER_ENABLED)
    message(STATUS "D3D12 debug layer ENABLED")
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

# ---- N64ModernRuntime (ultramodern + librecomp) ----
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# ---- RecompiledFuncs ----
# Path to the N64Recomp output — adjust if you move the files
set(RECOMP_FUNCS_DIR "${CMAKE_SOURCE_DIR}/../N64Recomp/RecompiledFuncs")

add_library(RecompiledFuncs STATIC)

if (MSVC)
    target_compile_options(RecompiledFuncs PRIVATE /wd4101 /wd4133)
    # ClangCL: the recompiler emits calls to libultra *_recomp wrappers whose
    # declarations live in the runtime, not funcs.h. Treat implicit decls as a
    # warning instead of an error (plain MSVC cl ignores this flag).
    target_compile_options(RecompiledFuncs PRIVATE
        -Wno-implicit-function-declaration
        -Wno-error=implicit-function-declaration)
    # Force-include our MEM_WU macro header (fills the unsigned-word gap in
    # upstream recomp.h without modifying it). See include/rsp_dpc_macros.h.
    target_compile_options(RecompiledFuncs PRIVATE
        /FI${CMAKE_SOURCE_DIR}/include/rsp_dpc_macros.h)
    # Rename game's 'main' to avoid MSVC treating it as the entry point
    target_compile_definitions(RecompiledFuncs PRIVATE main=rs_main)
else()
    target_compile_definitions(RecompiledFuncs PRIVATE main=rs_main)
    target_compile_options(RecompiledFuncs PRIVATE
        -fno-strict-aliasing
        -Wno-unused-variable
        -Wno-implicit-function-declaration
    )
endif()

target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

file(GLOB RECOMP_C_SOURCES   ${RECOMP_FUNCS_DIR}/*.c)
file(GLOB RECOMP_CXX_SOURCES ${RECOMP_FUNCS_DIR}/*.cpp)
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

    # Step 3: build PatchesLib that wraps the recompiled output
    add_library(PatchesLib STATIC "${RECOMPILED_PATCHES_DIR}/patches.c")
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
            -Wno-error=implicit-function-declaration)
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
# A pure HLE replacement for the LLE side is feasible (op_01..op_04 plus
# triangle assembly are tractable handlers, contrary to factor5-gbi.md's
# original "GBI-level plan is closed" conclusion) but is multi-day work.
# Keeping LLE for now lets us ship visuals while migrating ops one at a time.
set(FACTOR5_UCODE_C ${CMAKE_SOURCE_DIR}/build/factor5_ucode/factor5_ucode_recompiled.c)
set(FACTOR5_BOOT_C  ${CMAKE_SOURCE_DIR}/build/factor5_ucode/factor5_boot_recompiled.c)
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
set_source_files_properties(${FACTOR5_UCODE_C} ${FACTOR5_BOOT_C} PROPERTIES
    LANGUAGE CXX
    COMPILE_OPTIONS "-msse4.1;/FI${CMAKE_SOURCE_DIR}/include/rsp_dpc_macros.h")

add_executable(RogueSquadron64Recomp
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/rt64_render_context.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/upstream_compat.cpp
    ${CMAKE_SOURCE_DIR}/src/rsp/aspMain.cpp
    ${CMAKE_SOURCE_DIR}/src/rsp/dpc_bridge.cpp
    ${FACTOR5_BOOT_C}
    ${FACTOR5_UCODE_C}
    $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/main/app.rc>
)

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

# PatchesLib must come BEFORE RecompiledFuncs so the linker takes its
# overrides for any duplicated symbol names (Zelda's pattern).
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
