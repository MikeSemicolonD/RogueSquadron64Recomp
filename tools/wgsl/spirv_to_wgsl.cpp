// Converts one of RT64's SPIR-V shaders to WGSL for the browser build: spirv_to_wgsl <in.spv> <out.wgsl> [--specialize]
// --specialize first fixes the raster ubershader's spec constants to one real pipeline (re-spirv), as RT64 does per pipeline at runtime.

#include "spirv_web_passes.h"

#include "re-spirv.h"
#include "src/tint/api/tint.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

    bool read_words(const char* path, std::vector<uint32_t>& words)
    {

        std::ifstream in(path, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        if (!in.good() && !in.eof())
        {
            return false;
        }

        if (bytes.empty() || (bytes.size() % 4) != 0)
        {
            return false;
        }

        words.resize(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        return true;
    }

    // The pipeline constants re-spirv-cli uses: one real draw's othermode, combiner and flags.
    bool specialize(std::vector<uint32_t>& words)
    {

        respv::Shader shader;

        if (!shader.parse(words.data(), words.size() * 4))
        {
            return false;
        }

        const std::vector<respv::SpecConstant> constants = {
            respv::SpecConstant(0, { 3356565624U }),
            respv::SpecConstant(1, { 1584128U }),
            respv::SpecConstant(2, { 4229999620U }),
            respv::SpecConstant(3, { 4279211007U }),
            respv::SpecConstant(4, { 747626510U }),
        };

        std::vector<uint8_t> optimized;

        if (!respv::Optimizer::run(shader, constants.data(), uint32_t(constants.size()), optimized))
        {
            return false;
        }

        words.resize(optimized.size() / 4);
        std::memcpy(words.data(), optimized.data(), optimized.size());
        return true;
    }

}

int main(int argc, char** argv)
{

    if (argc < 3)
    {
        std::fprintf(stderr, "usage: spirv_to_wgsl <in.spv> <out.wgsl> [--specialize]\n");
        return 2;
    }

    std::vector<uint32_t> words;

    if (!read_words(argv[1], words))
    {
        std::fprintf(stderr, "%s: not a SPIR-V file\n", argv[1]);
        return 1;
    }

    if ((argc > 3) && (std::strcmp(argv[3], "--specialize") == 0) && !specialize(words))
    {
        std::fprintf(stderr, "%s: specialization failed\n", argv[1]);
        return 1;
    }

    rs64::wgsl::apply_all(words);
    tint::Initialize();

    auto module = tint::spirv::reader::ReadIR(words);

    if (module != tint::Success)
    {
        std::fprintf(stderr, "%s: %s\n", argv[1], module.Failure().reason.c_str());
        return 1;
    }

    tint::wgsl::writer::Options options;

    options.allow_non_uniform_derivatives = true;
    options.allowed_features = tint::wgsl::AllowedFeatures::Everything();

    auto wgsl = tint::wgsl::writer::WgslFromIR(module.Get(), options);

    if (wgsl != tint::Success)
    {
        std::fprintf(stderr, "%s: %s\n", argv[1], wgsl.Failure().reason.c_str());
        return 1;
    }

    std::ofstream out(argv[2], std::ios::binary);

    out << wgsl->wgsl;
    return out.good() ? 0 : 1;
}
