# rs64_relay: the co-op relay server. Part of the game's build (not built by default), or standalone with only the enet6 submodule:
#   cmake -S relay-server -B build-relay && cmake --build build-relay --config Release
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(rs64_relay LANGUAGES C CXX)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    set(RELAY_STANDALONE ON)
    # Portable package: the static C runtime on Windows (no VC++ redistributable); before any target so enet6 matches.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release)
    endif()
endif()
get_filename_component(RS64_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(RELAY_ENET ${RS64_ROOT}/mods/multiplayer-native/lib/enet6)

if (NOT TARGET enet6)
    add_subdirectory(${RELAY_ENET} ${CMAKE_BINARY_DIR}/enet6 EXCLUDE_FROM_ALL)
endif()

add_library(rs64_relay_core STATIC EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/src/relay_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/relay_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/relay_host.cpp)
target_include_directories(rs64_relay_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${RS64_ROOT}/mods/multiplayer-native/src)
target_link_libraries(rs64_relay_core PUBLIC enet6)

if (RELAY_STANDALONE)
    add_executable(rs64_relay ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
else()
    add_executable(rs64_relay EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
endif()

target_link_libraries(rs64_relay PRIVATE rs64_relay_core)

if (UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(rs64_relay PRIVATE Threads::Threads)
endif()

# Linux package: libstdc++ and libgcc linked in, so only glibc comes from the system (CI builds on the oldest hosted Ubuntu).
if (RELAY_STANDALONE AND UNIX AND NOT APPLE)
    target_link_options(rs64_relay PRIVATE -static-libstdc++ -static-libgcc)
endif()

add_executable(relay_core_test EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_core_test.cpp)
target_link_libraries(relay_core_test PRIVATE rs64_relay_core)
target_include_directories(relay_core_test PRIVATE ${RS64_ROOT}/tests)

foreach(t rs64_relay_core rs64_relay relay_core_test)
    set_target_properties(${t} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

    if (MSVC)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endforeach()
