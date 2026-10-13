# Shared build for .nrm code mods. A mod's CMakeLists.txt sets the project and calls:
#
#   add_code_mod(<nrm_name> [<source.c>...])
#
# Compiles with MIPS GCC, links with <mod>/mod.ld if the mod has one (else the SDK's syms/include/mod.ld), packs with RecompModTool per <mod>/mod.toml (whose
# mod_filename must equal <nrm_name>), and copies <nrm_name>.nrm into mods/.
if (CMAKE_HOST_WIN32)
    set(RS64_HOST_EXE_SUFFIX ".exe")
    set(RS64_MOD_TOOL_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../../build/Debug/RecompModTool.exe")
else()
    set(RS64_MOD_TOOL_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../../build/RecompModTool")
endif()
set(MIPS_TOOLCHAIN_DIR "E:/mips-toolchain" CACHE PATH "MIPS GCC toolchain root")
set(MIPS_GCC "${MIPS_TOOLCHAIN_DIR}/bin/mips64-elf-gcc${RS64_HOST_EXE_SUFFIX}" CACHE FILEPATH "MIPS C compiler")
set(MIPS_LD  "${MIPS_TOOLCHAIN_DIR}/bin/mips64-elf-ld${RS64_HOST_EXE_SUFFIX}" CACHE FILEPATH "MIPS linker")
set(RECOMP_MOD_TOOL "${RS64_MOD_TOOL_DEFAULT}" CACHE FILEPATH "RecompModTool executable")
set(RS64_MOD_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../syms/include")
set(RS64_GAME_ROM "${CMAKE_CURRENT_LIST_DIR}/../../build/Debug/rogue_squadron.z64" CACHE FILEPATH "The game ROM (big-endian .z64), if present: instruction patches are checked against it at build time")
# Also used by the base game's patches ELF (CMakeLists.txt). Sibling calls stay off: a tail j into an import or game function fails LiveRecomp. No stack protector: distro MIPS GCCs (Ubuntu mips-linux-gnu) enable it by default and its guard symbol doesn't exist in the game.
set(RS64_MIPS_CFLAGS
    -mips2 -mabi=32 -O2 -G0 -mno-abicalls -fomit-frame-pointer -fno-builtin -fno-optimize-sibling-calls -fno-stack-protector
    -nostdinc -I "${RS64_MOD_SDK_DIR}" -Wall -Wextra -Wno-unused-parameter)

function(add_code_mod NRM_NAME)
    set(MOD_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(MOD_ELF "${MOD_DIR}/build/mod.elf")
    set(MOD_NRM "${MOD_DIR}/../${NRM_NAME}.nrm")

    set(objs "")
    foreach(src ${ARGN})
        get_filename_component(stem "${src}" NAME_WE)
        set(obj "${MOD_DIR}/build/${stem}.o")
        add_custom_command(OUTPUT "${obj}"
            COMMAND "${MIPS_GCC}" ${RS64_MIPS_CFLAGS} -c "${MOD_DIR}/${src}" -o "${obj}"
            DEPENDS "${MOD_DIR}/${src}")
        list(APPEND objs "${obj}")
    endforeach()

    # No sources: a mod of only [[patches.instruction]] entries (mod.toml has no elf_path).
    set(MOD_ELF_DEP "")
    if (objs)
        if (EXISTS "${MOD_DIR}/mod.ld")
            set(MOD_LD "${MOD_DIR}/mod.ld")
        else()
            set(MOD_LD "${RS64_MOD_SDK_DIR}/mod.ld")
        endif()
        # Game functions stay undefined so RecompModTool resolves them by name; GNU ld resolves them to 0 and flags every jal as out of range, so --noinhibit-exec keeps the output (the emitted relocs are what RecompModTool reads).
        add_custom_command(OUTPUT "${MOD_ELF}"
            COMMAND "${MIPS_LD}"
                -nostdlib -T "${MOD_LD}" -Map "${MOD_DIR}/build/mod.map"
                --unresolved-symbols=ignore-all --emit-relocs --noinhibit-exec -e 0
                ${objs} -o "${MOD_ELF}"
            DEPENDS ${objs} "${MOD_LD}")
        set(MOD_ELF_DEP "${MOD_ELF}")
    endif()

    # --as assembles the toml's instruction patches; --rom (when the game's ROM is there) checks their expect against the game at build time.
    set(MOD_TOOL_ARGS --as "${MIPS_GCC}")
    if (EXISTS "${RS64_GAME_ROM}")
        list(APPEND MOD_TOOL_ARGS --rom "${RS64_GAME_ROM}")
    endif()

    # RecompModTool also writes loose mod.json/bin files, so it outputs to build/ and only the .nrm is copied.
    # A menu file packed through mod.toml additional_files repacks the .nrm when it changes.
    file(GLOB MOD_MENU "${MOD_DIR}/roguesq_menu.json")
    add_custom_command(OUTPUT "${MOD_NRM}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MOD_DIR}/build"
        COMMAND "${RECOMP_MOD_TOOL}" "${MOD_DIR}/mod.toml" "${MOD_DIR}/build" ${MOD_TOOL_ARGS}
        COMMAND ${CMAKE_COMMAND} -E copy "${MOD_DIR}/build/${NRM_NAME}.nrm" "${MOD_NRM}"
        DEPENDS ${MOD_ELF_DEP} "${MOD_DIR}/mod.toml" ${MOD_MENU}
        WORKING_DIRECTORY "${MOD_DIR}")

    add_custom_target(${NRM_NAME} ALL DEPENDS "${MOD_NRM}")
endfunction()
