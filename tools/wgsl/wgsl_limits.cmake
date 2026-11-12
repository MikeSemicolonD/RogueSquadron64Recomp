# Prints each converted shader that exceeds a WebGPU default per-stage limit or uses a read-write storage format core WebGPU doesn't allow.
# cmake -DWGSL_DIR=<dir> -P wgsl_limits.cmake

file(GLOB shaders ${WGSL_DIR}/*.wgsl)
list(LENGTH shaders total)
set(over 0)

foreach(path ${shaders})
    file(READ ${path} text)
    get_filename_component(name ${path} NAME_WE)
    string(REGEX MATCHALL "var<storage" storage "${text}")
    string(REGEX MATCHALL ": sampler(_comparison)?;" samplers "${text}")
    string(REGEX MATCHALL ": texture_(1d|2d|3d|cube|multisampled|depth)" textures "${text}")
    string(REGEX MATCHALL ": texture_storage_" storage_textures "${text}")
    string(REGEX MATCHALL "texture_storage_[a-z0-9_]+<(rgba8unorm|rgba8uint|rgba16float|rgba16uint|rgba32float|rgba32uint|rg32float), read_write>" rw "${text}")
    list(LENGTH storage n_storage)
    list(LENGTH samplers n_samplers)
    list(LENGTH textures n_textures)
    list(LENGTH storage_textures n_storage_textures)
    list(LENGTH rw n_rw)
    set(problems)

    if (n_storage GREATER 8)
        list(APPEND problems "${n_storage} storage buffers")
    endif()

    if (n_samplers GREATER 16)
        list(APPEND problems "${n_samplers} samplers")
    endif()

    if (n_textures GREATER 16)
        list(APPEND problems "${n_textures} sampled textures")
    endif()

    if (n_storage_textures GREATER 4)
        list(APPEND problems "${n_storage_textures} storage textures")
    endif()

    if (n_rw GREATER 0)
        list(APPEND problems "read-write storage needing texture-formats-tier2")
    endif()

    if (problems)
        string(REPLACE ";" ", " problems "${problems}")
        message(STATUS "${name}: ${problems}")
        math(EXPR over "${over} + 1")
    endif()
endforeach()

message(STATUS "WGSL: ${total} shaders, ${over} over WebGPU default limits")
