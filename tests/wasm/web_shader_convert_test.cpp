// Runs in Chrome (web_shader_convert_test.html, which loads rs64tint first): converts some of the web build's own shaders, the ray-traced
// passes among them, and a garbage module.

#include "../check.h"
#include "video/web_shader_convert.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
    extern const char ComposePSBlobSPIRV[];
    extern const size_t ComposePSBlobSPIRV_size;
    extern const char RSPSmoothNormalCSBlobSPIRV[];
    extern const size_t RSPSmoothNormalCSBlobSPIRV_size;
    extern const char RS64ShadowsPSBlobSPIRV[];
    extern const size_t RS64ShadowsPSBlobSPIRV_size;
    extern const char RS64ShadowsSoftPSBlobSPIRV[];
    extern const size_t RS64ShadowsSoftPSBlobSPIRV_size;
    extern const char RS64LightsShadowedPSBlobSPIRV[];
    extern const size_t RS64LightsShadowedPSBlobSPIRV_size;
    extern const char RS64FogShaftsPSBlobSPIRV[];
    extern const size_t RS64FogShaftsPSBlobSPIRV_size;
    extern const char RS64HazeShaftsPSBlobSPIRV[];
    extern const size_t RS64HazeShaftsPSBlobSPIRV_size;
    extern const char RS64HazeUpsamplePSBlobSPIRV[];
    extern const size_t RS64HazeUpsamplePSBlobSPIRV_size;
}

namespace {

    std::vector<uint32_t> words(const char* blob, size_t size)
    {

        std::vector<uint32_t> out(size / 4);

        std::memcpy(out.data(), blob, out.size() * 4);
        return out;
    }

}

int main()
{

    const rs64::wgsl::ConvertResult compose = rs64::wgsl::convert_on_main(words(ComposePSBlobSPIRV, ComposePSBlobSPIRV_size));

    CHECK(compose.ok);
    CHECK(compose.wgsl.find("@fragment") != std::string::npos);

    const rs64::wgsl::ConvertResult smooth = rs64::wgsl::convert_on_main(words(RSPSmoothNormalCSBlobSPIRV, RSPSmoothNormalCSBlobSPIRV_size));

    CHECK(smooth.ok);
    CHECK(smooth.wgsl.find("@compute") != std::string::npos);
    CHECK(smooth.wgsl.find("storageBarrier()") != std::string::npos);

    // The traced passes, with RS64SoftRayQuery inlined at every trace.
    struct Traced {
        const char* name;
        const char* blob;
        size_t size;
    };

    const Traced traced[] = {
        { "RS64ShadowsPS", RS64ShadowsPSBlobSPIRV, RS64ShadowsPSBlobSPIRV_size },
        { "RS64ShadowsSoftPS", RS64ShadowsSoftPSBlobSPIRV, RS64ShadowsSoftPSBlobSPIRV_size },
        { "RS64LightsShadowedPS", RS64LightsShadowedPSBlobSPIRV, RS64LightsShadowedPSBlobSPIRV_size },
        { "RS64FogShaftsPS", RS64FogShaftsPSBlobSPIRV, RS64FogShaftsPSBlobSPIRV_size },
        { "RS64HazeShaftsPS", RS64HazeShaftsPSBlobSPIRV, RS64HazeShaftsPSBlobSPIRV_size },
        { "RS64HazeUpsamplePS", RS64HazeUpsamplePSBlobSPIRV, RS64HazeUpsamplePSBlobSPIRV_size }
    };

    for (const Traced& t : traced) {
        std::printf("converting %s (%zu bytes)\n", t.name, t.size);

        const rs64::wgsl::ConvertResult result = rs64::wgsl::convert_on_main(words(t.blob, t.size));

        if (!result.ok) {
            std::printf("%s failed: %s\n", t.name, result.error.c_str());
        }

        CHECK(result.ok);
    }

    const rs64::wgsl::ConvertResult garbage = rs64::wgsl::convert_on_main({ 0x07230203u, 0x00010000u, 0u });

    CHECK(!garbage.ok);
    CHECK(!garbage.error.empty());
    std::printf("web_shader_convert_test PASS\n");
    return 0;
}
