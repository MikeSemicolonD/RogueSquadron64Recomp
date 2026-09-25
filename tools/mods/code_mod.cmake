# Shared build for .nrm code mods. A mod's CMakeLists.txt sets the project and calls:
#
#   add_code_mod(<nrm_name> <source.c>...)
#
# Compiles with MIPS GCC, links with <mod>/mod.ld, packs with RecompModTool per <mod>/mod.toml (whose
# mod_filename must equal <nrm_name>), and copies <nrm_name>.nrm into mods/.
set(MIPS_TOOLCHAIN_DIR "E:/mips-toolchain" CACHE PATH "MIPS GCC toolchain root")
set(RECOMP_MOD_TOOL "${CMAKE_CURRENT_LIST_DIR}/../../build/Debug/RecompModTool.exe" CACHE FILEPATH "RecompModTool executable")

function(add_code_mod NRM_NAME)
    set(MOD_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(MOD_ELF "${MOD_DIR}/build/mod.elf")
    set(MOD_NRM "${MOD_DIR}/../${NRM_NAME}.nrm")

    set(objs "")
    foreach(src ${ARGN})
        get_filename_component(stem "${src}" NAME_WE)
        set(obj "${MOD_DIR}/build/${stem}.o")
        add_custom_command(OUTPUT "${obj}"
            COMMAND "${MIPS_TOOLCHAIN_DIR}/bin/mips64-elf-gcc.exe"
                -mips2 -mabi=32 -O2 -G0 -mno-abicalls -fomit-frame-pointer -fno-builtin
                -nostdinc -Wall -Wextra -Wno-unused-parameter
                -c "${MOD_DIR}/${src}" -o "${obj}"
            DEPENDS "${MOD_DIR}/${src}")
        list(APPEND objs "${obj}")
    endforeach()

    # Game functions stay undefined so RecompModTool resolves them by name; GNU ld resolves them to 0 and flags every jal as out of range, so --noinhibit-exec keeps the output (the emitted relocs are what RecompModTool reads).
    add_custom_command(OUTPUT "${MOD_ELF}"
        COMMAND "${MIPS_TOOLCHAIN_DIR}/bin/mips64-elf-ld.exe"
            -nostdlib -T "${MOD_DIR}/mod.ld" -Map "${MOD_DIR}/build/mod.map"
            --unresolved-symbols=ignore-all --emit-relocs --noinhibit-exec -e 0
            ${objs} -o "${MOD_ELF}"
        DEPENDS ${objs} "${MOD_DIR}/mod.ld")

    # RecompModTool also writes loose mod.json/bin files, so it outputs to build/ and only the .nrm is copied.
    add_custom_command(OUTPUT "${MOD_NRM}"
        COMMAND "${RECOMP_MOD_TOOL}" "${MOD_DIR}/mod.toml" "${MOD_DIR}/build"
        COMMAND ${CMAKE_COMMAND} -E copy "${MOD_DIR}/build/${NRM_NAME}.nrm" "${MOD_NRM}"
        DEPENDS "${MOD_ELF}" "${MOD_DIR}/mod.toml"
        WORKING_DIRECTORY "${MOD_DIR}")

    add_custom_target(${NRM_NAME} ALL DEPENDS "${MOD_NRM}")
endfunction()
