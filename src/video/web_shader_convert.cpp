#include "video/web_shader_convert.h"

#include "spirv_web_passes.h"

#include <cstdlib>
#include <cstring>

#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>

EM_JS_DEPS(rs64_web_shader_convert, "$stringToNewUTF8,malloc");

namespace rs64::wgsl {

    ConvertResult convert_on_main(std::vector<uint32_t> words)
    {

        apply_all(words);

        // Module.rs64Tint is the page's rs64tint instance; it has its own heap, so the words are copied across.
        char* raw = static_cast<char*>(MAIN_THREAD_EM_ASM_PTR({
            const tint = Module.rs64Tint;

            if (!tint)
              return stringToNewUTF8('error\nrs64tint is not loaded');

            const first = $0 >> 2;
            const src = tint._malloc($1 * 4);

            tint.HEAPU32.set(HEAPU32.subarray(first, first + $1), src >> 2);

            const out = tint._rs64_tint_convert(src, $1);
            const text = tint.UTF8ToString(out);

            tint._rs64_tint_free(out);
            tint._free(src);
            return stringToNewUTF8(text);
        }, words.data(), words.size()));

        ConvertResult result;
        static constexpr const char* kOk = "ok\n";
        static constexpr const char* kError = "error\n";

        if (std::strncmp(raw, kOk, std::strlen(kOk)) == 0) {
            result.ok = true;
            result.wgsl = raw + std::strlen(kOk);
        }
        else if (std::strncmp(raw, kError, std::strlen(kError)) == 0) {
            result.error = raw + std::strlen(kError);
        }
        else {
            result.error = raw;
        }

        std::free(raw);
        return result;
    }

}
