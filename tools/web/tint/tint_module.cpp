// The browser build's SPIR-V to WGSL converter: Tint's SPIR-V reader and WGSL writer in a standalone wasm module the play page loads (rs64tint.js / rs64tint.wasm).
// Built offline by make-tint-wasm.ps1; the game calls it from the main browser thread through Module.rs64TintConvert.

#include "src/tint/api/tint.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

namespace {

    char* make_result(const char* prefix, const std::string& text)
    {

        const size_t prefixLength = std::strlen(prefix);
        char* out = static_cast<char*>(std::malloc(prefixLength + text.size() + 1));

        std::memcpy(out, prefix, prefixLength);
        std::memcpy(out + prefixLength, text.data(), text.size());
        out[prefixLength + text.size()] = '\0';
        return out;
    }

}

extern "C" {

    // Returns "ok\n" + WGSL or "error\n" + Tint's message; the caller frees it with rs64_tint_free.
    EMSCRIPTEN_KEEPALIVE char* rs64_tint_convert(const uint32_t* words, uint32_t count)
    {

        static bool initialized = false;

        if (!initialized) {
            tint::Initialize();
            initialized = true;
        }

        const std::vector<uint32_t> input(words, words + count);
        auto module = tint::spirv::reader::ReadIR(input);

        if (module != tint::Success)
        {
            return make_result("error\n", module.Failure().reason);
        }

        tint::wgsl::writer::Options options;

        options.allow_non_uniform_derivatives = true;
        options.allowed_features = tint::wgsl::AllowedFeatures::Everything();

        auto wgsl = tint::wgsl::writer::WgslFromIR(module.Get(), options);

        if (wgsl != tint::Success)
        {
            return make_result("error\n", wgsl.Failure().reason);
        }

        return make_result("ok\n", wgsl->wgsl);
    }

    EMSCRIPTEN_KEEPALIVE void rs64_tint_free(char* result)
    {
        std::free(result);
    }

}
