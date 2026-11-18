#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Browser build only: SPIR-V to WGSL through the page's rs64tint module, after the passes in tools/wgsl/spirv_web_passes.h.
namespace rs64::wgsl {

    struct ConvertResult {
        bool ok = false;
        std::string wgsl;
        std::string error;
    };

    // Runs on the main browser thread (where the page loaded rs64tint), waiting there when called from another thread.
    ConvertResult convert_on_main(std::vector<uint32_t> words);

    // Runs on the calling thread; a worker loads its own rs64tint on first use, so conversions don't hold up the main thread.
    ConvertResult convert_here(std::vector<uint32_t> words);

}
