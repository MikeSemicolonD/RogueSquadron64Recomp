# Generate the game-state table header from the canonical descriptor (state_model.toml). Needs tomlplusplus::tomlplusplus.
get_filename_component(STATE_TABLE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
set(STATE_TABLE_INL ${STATE_TABLE_ROOT}/src/game/state_table.inl)
if (ANDROID)
    add_custom_target(gen_state_table)
else()
    add_executable(gen_state_table_tool ${STATE_TABLE_ROOT}/tools/state/gen_state_table.cpp)
    set_target_properties(gen_state_table_tool PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(gen_state_table_tool PRIVATE tomlplusplus::tomlplusplus)
    add_custom_command(
        OUTPUT ${STATE_TABLE_INL}
        COMMAND gen_state_table_tool ${STATE_TABLE_ROOT}/tools/state/state_model.toml ${STATE_TABLE_INL}
        DEPENDS gen_state_table_tool ${STATE_TABLE_ROOT}/tools/state/state_model.toml
        COMMENT "Generating state_table.inl from state_model.toml")
    add_custom_target(gen_state_table DEPENDS ${STATE_TABLE_INL})
endif()
