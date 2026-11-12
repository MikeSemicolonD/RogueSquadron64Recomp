# WGSL for the browser build: spirv_to_wgsl converts RT64's SPIR-V with Tint from a Dawn release package (RS64_DAWN_DIR).
# Build: cmake -S . -B build -DRS64_DAWN_DIR=<Dawn package>, then cmake --build build --config Release --target rs64_web_shaders

set(RS64_RESPIRV_DIR ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/re-spirv)
set(RS64_DAWN_COMMIT 37f80f91a029d37019995d4bfa9141fa73fe1cc6)
file(GLOB RS64_TINT_LIBS ${RS64_DAWN_DIR}/lib/tint_*.lib ${RS64_DAWN_DIR}/lib/libtint_*.a)

# The package's Tint headers include Dawn's src/utils/compiler.h, which the package doesn't install.
set(RS64_DAWN_EXTRA_INCLUDE ${CMAKE_BINARY_DIR}/dawn-extra)
file(DOWNLOAD https://raw.githubusercontent.com/google/dawn/${RS64_DAWN_COMMIT}/src/utils/compiler.h ${RS64_DAWN_EXTRA_INCLUDE}/src/utils/compiler.h
    EXPECTED_HASH SHA256=ce2b896c8f708730da5bd8831cac6ffbd6b339315a8a11f578fc615177e3e318)

# The Dawn package is a Release build: both targets match its runtime and STL debug level in every configuration.
add_library(spirv_to_wgsl_respirv OBJECT EXCLUDE_FROM_ALL ${RS64_RESPIRV_DIR}/re-spirv.cpp)
set_target_properties(spirv_to_wgsl_respirv PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
target_include_directories(spirv_to_wgsl_respirv PRIVATE ${RS64_RESPIRV_DIR}/external/SPIRV-Headers/include)
target_compile_definitions(spirv_to_wgsl_respirv PRIVATE _ITERATOR_DEBUG_LEVEL=0 _HAS_ITERATOR_DEBUGGING=0)

add_executable(spirv_to_wgsl EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tools/wgsl/spirv_to_wgsl.cpp
    $<TARGET_OBJECTS:spirv_to_wgsl_respirv>
    $<$<CXX_COMPILER_ID:MSVC,Clang>:$<$<BOOL:${WIN32}>:${CMAKE_SOURCE_DIR}/tools/wgsl/msvc_stl_shims.cpp>>
)
set_target_properties(spirv_to_wgsl PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
target_include_directories(spirv_to_wgsl PRIVATE
    ${RS64_DAWN_DIR}/include/src/tint
    ${RS64_DAWN_EXTRA_INCLUDE}
    ${RS64_RESPIRV_DIR}
)
target_compile_definitions(spirv_to_wgsl PRIVATE _ITERATOR_DEBUG_LEVEL=0 _HAS_ITERATOR_DEBUGGING=0
    NDEBUG DAWN_HAVE_UTILS_BUILD_CONFIG TINT_BUILD_SPV_READER=1 TINT_BUILD_WGSL_READER=1 TINT_BUILD_WGSL_WRITER=1)
target_link_libraries(spirv_to_wgsl PRIVATE ${RS64_TINT_LIBS} ${RS64_DAWN_DIR}/lib/webgpu_dawn.lib)

# Every SPIR-V shader the web build uses. Left out: the debug overlay (Im3D, DebugPS), bindless raster and terrain-pack variants, RayQuery shaders, and the typed-buffer shaders the WebGPU renderer rewrites.
set(RS64_WEB_SHADERS
    RasterVSDynamic RasterVSSpecConstant RasterVSSpecConstantFlat
    FbChangesClearCS FbChangesDrawColorPS FbChangesDrawDepthPS FbReinterpretCS
    GaussianFilterRGB3x3CS BoxFilterCS BicubicScalingCS
    HistogramAverageCS HistogramClearCS HistogramSetCS IdleCS LuminanceHistogramCS
    PostBlendDitherNoiseAddPS PostBlendDitherNoiseSubPS PostBlendDitherNoiseSubNegativePS
    RSPSmoothNormalCS RSPVertexTestZCS RSPVertexTestZCSMS
    RtCopyColorToDepthPS RtCopyColorToDepth2XPS RtCopyColorToDepth4XPS RtCopyColorToDepth8XPS
    RtCopyDepthToColorPS RtCopyDepthToColor2XPS RtCopyDepthToColor4XPS RtCopyDepthToColor8XPS
    RS64LightsPS RS64LightsUpsamplePS RS64DepthResolvePS RS64TerrainVS RS64TerrainPS
    TextureCopyPS TextureDecodeCS TextureResolveSamples2XPS TextureResolveSamples4XPS TextureResolveSamples8XPS
    VideoInterfacePSRegular VideoInterfacePSPixel FullScreenVS FxaaPS ComposePS PostProcessPS
)

# Converted after specializing to one pipeline: unspecialized, the slot ubershader is too large for Tint.
set(RS64_WEB_SHADERS_SPECIALIZED
    RasterPSSpecConstantSlots RasterPSSpecConstantSlotsMS RasterPSSpecConstantSlotsFlat RasterPSSpecConstantSlotsFlatMS
)

set(RS64_WGSL_DIR ${CMAKE_BINARY_DIR}/wgsl)
set(RS64_WGSL_OUTPUTS)

foreach(name ${RS64_WEB_SHADERS} ${RS64_WEB_SHADERS_SPECIALIZED})
    set(flags)

    if (name IN_LIST RS64_WEB_SHADERS_SPECIALIZED)
        set(flags --specialize)
    endif()

    add_custom_command(OUTPUT ${RS64_WGSL_DIR}/${name}.wgsl
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RS64_WGSL_DIR}
        COMMAND spirv_to_wgsl ${CMAKE_BINARY_DIR}/src/shaders/${name}.hlsl.spv ${RS64_WGSL_DIR}/${name}.wgsl ${flags}
        DEPENDS spirv_to_wgsl ${CMAKE_BINARY_DIR}/src/shaders/${name}.hlsl.spv
        VERBATIM)
    list(APPEND RS64_WGSL_OUTPUTS ${RS64_WGSL_DIR}/${name}.wgsl)
endforeach()

add_custom_target(rs64_web_shaders
    DEPENDS ${RS64_WGSL_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -DWGSL_DIR=${RS64_WGSL_DIR} -P ${CMAKE_SOURCE_DIR}/tools/wgsl/wgsl_limits.cmake
    VERBATIM)
add_dependencies(rs64_web_shaders rt64)
