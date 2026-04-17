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
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

# ---- N64ModernRuntime (ultramodern + librecomp) ----
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# ---- RecompiledFuncs ----
# Path to the N64Recomp output — adjust if you move the files
set(RECOMP_FUNCS_DIR "${CMAKE_SOURCE_DIR}/../N64Recomp/RecompiledFuncs")

add_library(RecompiledFuncs STATIC)

if (MSVC)
    target_compile_options(RecompiledFuncs PRIVATE /wd4101 /wd4133)
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

# ---- Main executable ----
add_executable(RogueSquadron64Recomp
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/rt64_render_context.cpp
    ${CMAKE_SOURCE_DIR}/src/rsp/aspMain.cpp
)

target_include_directories(RogueSquadron64Recomp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
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

target_link_libraries(RogueSquadron64Recomp PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
)

set_property(TARGET RogueSquadron64Recomp PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
