# RELAY_WEBTRANSPORT: the relay's listener for browser players, on libwtf over MsQuic's OpenSSL build (Schannel QUIC needs Windows 11).
# Included by CMakeLists.txt when the option is on; fetches both pinned dependencies at configure time.
include(FetchContent)

set(RELAY_MSQUIC_VERSION 2.6.4)

if (WIN32)
    # Microsoft's signed NuGet package: msquic.dll with OpenSSL built in, plus its import library and headers.
    set(msquic_pkg ${CMAKE_BINARY_DIR}/_deps/msquic-openssl-${RELAY_MSQUIC_VERSION})
    if (NOT EXISTS ${msquic_pkg}/build/native/include/msquic.h)
        file(DOWNLOAD
            https://api.nuget.org/v3-flatcontainer/microsoft.native.quic.msquic.openssl/${RELAY_MSQUIC_VERSION}/microsoft.native.quic.msquic.openssl.${RELAY_MSQUIC_VERSION}.nupkg
            ${msquic_pkg}.zip
            EXPECTED_HASH SHA256=370baf73845ac516a7acb53394ebf889ca947f7826a0e74f8f8c332b3627adb5)
        file(ARCHIVE_EXTRACT INPUT ${msquic_pkg}.zip DESTINATION ${msquic_pkg})
    endif()

    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(msquic_arch x64)
    else()
        set(msquic_arch x86)
    endif()

    set(RELAY_MSQUIC_DLL ${msquic_pkg}/build/native/bin/${msquic_arch}/msquic.dll)

    # libwtf finds MsQuic with find_package(msquic CONFIG): a config file naming the package's files.
    file(WRITE ${CMAKE_BINARY_DIR}/_deps/msquic-config/msquic-config.cmake
        "if (NOT TARGET msquic)\n"
        "  add_library(msquic SHARED IMPORTED)\n"
        "  set_target_properties(msquic PROPERTIES IMPORTED_LOCATION \"${RELAY_MSQUIC_DLL}\" IMPORTED_IMPLIB \"${msquic_pkg}/build/native/lib/${msquic_arch}/msquic.lib\" INTERFACE_INCLUDE_DIRECTORIES \"${msquic_pkg}/build/native/include\")\n"
        "endif()\n")
    set(msquic_DIR ${CMAKE_BINARY_DIR}/_deps/msquic-config CACHE PATH "" FORCE)
else()
    # shortcut: Linux and macOS use an installed libmsquic (OpenSSL build) with its CMake config; fetch one like Windows when release CI builds this.
    find_package(msquic CONFIG REQUIRED)
endif()

set(WTF_USE_EXTERNAL_MSQUIC ON CACHE BOOL "" FORCE)
set(WTF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WTF_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)

# libwtf's public header imports from a DLL on Windows, so it builds shared there; its submodules (MsQuic, googletest) aren't needed.
set(relay_shared_before ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS ${WIN32})
FetchContent_Declare(libwtf
    GIT_REPOSITORY https://github.com/andrewmd5/libwtf.git
    GIT_TAG 447610b0326d5014b028fe28dbe3c86bea2d688a
    GIT_SUBMODULES "")
FetchContent_MakeAvailable(libwtf)
set(BUILD_SHARED_LIBS ${relay_shared_before})

target_sources(rs64_relay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/relay_webtransport.cpp)
target_compile_definitions(rs64_relay PRIVATE RS64_RELAY_WEBTRANSPORT)
target_link_libraries(rs64_relay PRIVATE wtf)

if (WIN32)
    target_link_libraries(rs64_relay PRIVATE crypt32 ncrypt)
    add_custom_command(TARGET rs64_relay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RELAY_MSQUIC_DLL} $<TARGET_FILE:wtf> $<TARGET_FILE_DIR:rs64_relay>
        VERBATIM)
endif()
