# Standalone build for the native-demo example mod's DLL. Independent of the main
# game build so the game never churns on it:
#
#   cmake -S mods/native-demo -B mods/native-demo/build
#   cmake --build mods/native-demo/build
#
# Produces native_demo.dll next to mod.json (referenced by roguesq_menu.json's
# @exports). To use the mod, copy this whole folder into the game's mods/ directory.
cmake_minimum_required(VERSION 3.20)
project(native_demo_mod LANGUAGES CXX)

add_library(native_demo SHARED native_demo.cpp)
# The trailing $<0:> keeps multi-config generators (Visual Studio) from appending a
# per-config subfolder, so the DLL lands directly beside mod.json.
set_target_properties(native_demo PROPERTIES
    PREFIX ""
    OUTPUT_NAME "native_demo"
    CXX_STANDARD 17
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/$<0:>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/$<0:>")

target_include_directories(native_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

# Static CRT so the DLL is self-contained (LoadLibraryEx uses a restricted search
# that won't find the CRT DLLs otherwise).
if (MSVC)
    set_property(TARGET native_demo PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
