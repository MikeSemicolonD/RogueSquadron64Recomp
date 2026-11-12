# Builds rs64tint.js / rs64tint.wasm: Tint's SPIR-V reader and WGSL writer only, for the browser build. Run through make-tint-wasm.ps1 (offline; never part of the game's build).
# Adds Tint's own directories instead of Dawn's top level, which under Emscripten always builds Dawn itself.

cmake_minimum_required(VERSION 3.22)
project(rs64tint C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT DAWN_DIR)
    message(FATAL_ERROR "Pass -DDAWN_DIR=<Dawn checkout with third_party/abseil-cpp, spirv-headers/src and spirv-tools/src>")
endif()

# Backslashes from a Windows shell break CMake's try_compile projects.
file(TO_CMAKE_PATH "${DAWN_DIR}" DAWN_DIR)

# What Dawn's top-level CMakeLists.txt would set for Tint, including its common_compile_options().
list(APPEND CMAKE_MODULE_PATH ${DAWN_DIR}/src/cmake)
include(DawnCompilerExtraFlags)
set(TINT_ROOT_SOURCE_DIR ${DAWN_DIR})
set(Dawn_BINARY_DIR ${CMAKE_BINARY_DIR})
set(DAWN_BUILD_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
set(DAWN_INCLUDE_DIR ${DAWN_DIR}/include)
set(DAWN_THIRD_PARTY_DIR ${DAWN_DIR}/third_party)
set(TINT_SPIRV_HEADERS_DIR ${DAWN_DIR}/third_party/spirv-headers/src)
set(TINT_SPIRV_TOOLS_DIR ${DAWN_DIR}/third_party/spirv-tools/src)
set(DAWN_WERROR OFF)
set(TINT_TARGET_SUFFIX "")

set(TINT_BUILD_SPV_READER ON)
set(TINT_BUILD_WGSL_READER ON)
set(TINT_BUILD_WGSL_WRITER ON)

foreach(off TINT_BUILD_GLSL_WRITER TINT_BUILD_GLSL_VALIDATOR TINT_BUILD_HLSL_WRITER TINT_BUILD_MSL_WRITER TINT_BUILD_SPV_WRITER TINT_BUILD_NULL_WRITER
            TINT_BUILD_IR_BINARY TINT_BUILD_FUZZERS TINT_BUILD_FUZZER_VULKAN_SUPPORT TINT_BUILD_BENCHMARKS TINT_BUILD_TESTS TINT_BUILD_CMD_TOOLS
            TINT_BUILD_MESA TINT_ENABLE_IR_DUMPING TINT_ENABLE_IR_VALIDATION_ASSERTS TINT_ENABLE_BREAK_IN_DEBUGGER TINT_ENABLE_INSTALL)
    set(${off} OFF)
endforeach()

set(BUILD_TESTING OFF)
set(ABSL_PROPAGATE_CXX_STD ON)
add_subdirectory(${DAWN_DIR}/third_party/abseil-cpp abseil EXCLUDE_FROM_ALL)

set(SPIRV_HEADERS_SKIP_EXAMPLES ON)
set(SPIRV_HEADERS_SKIP_INSTALL ON)
add_subdirectory(${TINT_SPIRV_HEADERS_DIR} spirv-headers EXCLUDE_FROM_ALL)

set(SPIRV_SKIP_TESTS ON)
set(SPIRV_SKIP_EXECUTABLES ON)
set(SKIP_SPIRV_TOOLS_INSTALL ON)
set(SPIRV_WERROR OFF)
add_subdirectory(${TINT_SPIRV_TOOLS_DIR} spirv-tools EXCLUDE_FROM_ALL)

# Tint includes Dawn's src/utils headers, which require this define, and links dawn_shared_utils (both from Dawn's src/utils build).
add_compile_definitions(DAWN_HAVE_UTILS_BUILD_CONFIG)
add_library(dawn_shared_utils STATIC ${DAWN_DIR}/src/utils/assert.cc ${DAWN_DIR}/src/utils/log.cc)
target_include_directories(dawn_shared_utils PUBLIC ${DAWN_DIR})
add_subdirectory(${DAWN_DIR}/src/tint tint EXCLUDE_FROM_ALL)

add_executable(rs64tint ${CMAKE_CURRENT_SOURCE_DIR}/tint_module.cpp)
target_include_directories(rs64tint PRIVATE ${DAWN_DIR} ${DAWN_DIR}/include ${DAWN_BUILD_GEN_DIR})
target_compile_definitions(rs64tint PRIVATE DAWN_HAVE_UTILS_BUILD_CONFIG NDEBUG TINT_BUILD_SPV_READER=1 TINT_BUILD_WGSL_READER=1 TINT_BUILD_WGSL_WRITER=1)
target_link_libraries(rs64tint PRIVATE tint_api tint_lang_spirv_reader tint_lang_wgsl_writer)
set_target_properties(rs64tint PROPERTIES SUFFIX ".js")
target_link_options(rs64tint PRIVATE -O3 -sMODULARIZE -sEXPORT_NAME=rs64Tint -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH -sSTACK_SIZE=8MB
    -sEXPORTED_FUNCTIONS=_rs64_tint_convert,_rs64_tint_free,_malloc,_free -sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU32)
