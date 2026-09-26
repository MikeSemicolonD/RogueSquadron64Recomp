# cmake -DDIR=<staged mods dir> -P clean_staged_nrm.cmake
# Removes previously staged .nrm files so a renamed or deleted code mod can't linger beside the exe (two copies of one mod conflict and block game start).
if (NOT DIR OR NOT IS_DIRECTORY "${DIR}")
    return()
endif()
file(GLOB staged_nrm "${DIR}/*.nrm")
if (staged_nrm)
    file(REMOVE ${staged_nrm})
endif()
