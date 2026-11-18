// Runs in Chrome (web_shader_convert_test.html, which loads rs64tint first): converts two of the web build's own shaders and a garbage module.

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

    const rs64::wgsl::ConvertResult garbage = rs64::wgsl::convert_on_main({ 0x07230203u, 0x00010000u, 0u });

    CHECK(!garbage.ok);
    CHECK(!garbage.error.empty());
    std::printf("web_shader_convert_test PASS\n");
    return 0;
}
