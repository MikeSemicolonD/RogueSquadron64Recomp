# Build for the any-craft mod's DLL, separate from the game build:
#
#   cmake -S mods/any-craft -B mods/any-craft/build
#   cmake --build mods/any-craft/build --config Release
#
# Produces any_craft.dll next to mod.json; the next game build copies the mod folder beside the exe.
cmake_minimum_required(VERSION 3.20)
project(any_craft_mod LANGUAGES CXX)

add_library(any_craft SHARED any_craft.cpp)
# The trailing $<0:> keeps multi-config generators from appending a per-config subfolder.
set_target_properties(any_craft PROPERTIES
    PREFIX ""
    OUTPUT_NAME "any_craft"
    CXX_STANDARD 17
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/$<0:>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/$<0:>")

# Static CRT so the DLL is self-contained (LoadLibraryEx uses a restricted search).
if (MSVC)
    set_property(TARGET any_craft PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
