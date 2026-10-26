# The multiplayer mod's unit tests, added by tests/CMakeLists.txt and run by its rs64_unit_tests target.
get_filename_component(MP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
get_filename_component(RS64_ROOT ${MP_DIR}/../.. ABSOLUTE)

if (NOT TARGET enet6)
    include(${MP_DIR}/net_libs.cmake)
endif()

# Same rounding as the mod: no fused multiply-add in the flight model.
set(MP_NO_FMA ${RS64_ROOT}/src/lockstep/lockstep_core.cpp ${MP_DIR}/src/mp_core.cpp)
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(${MP_NO_FMA} PROPERTIES COMPILE_OPTIONS "/clang:-ffp-contract=off")
elseif (NOT MSVC)
    set_source_files_properties(${MP_NO_FMA} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

add_executable(mp_core_test EXCLUDE_FROM_ALL ${MP_DIR}/tests/mp_core_test.cpp ${MP_DIR}/src/mp_core.cpp ${RS64_ROOT}/src/lockstep/lockstep_core.cpp)
add_executable(net_core_test EXCLUDE_FROM_ALL ${MP_DIR}/tests/net_core_test.cpp ${MP_DIR}/src/net_core.cpp)
# Loopback test for the network link (ENet, simulated latency and loss); also talks to the real router through UPnP.
add_executable(net_link_test EXCLUDE_FROM_ALL ${MP_DIR}/tests/net_link_test.cpp ${MP_DIR}/src/net_core.cpp ${MP_DIR}/src/net_link.cpp)
target_link_libraries(net_link_test PRIVATE enet6 libminiupnpc-static)

foreach(t mp_core_test net_core_test net_link_test)
    set_target_properties(${t} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_include_directories(${t} PRIVATE ${MP_DIR}/src ${RS64_ROOT}/src ${RS64_ROOT}/include ${RS64_ROOT}/tests)

    if (MSVC)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endforeach()
