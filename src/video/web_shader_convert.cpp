#include "video/web_shader_convert.h"

#include "spirv_web_passes.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

EM_JS_DEPS(rs64_web_shader_convert, "$stringToNewUTF8,malloc");

// Converts on the calling thread: the page's rs64tint on the main thread, or a worker's own instance, loaded synchronously on its first conversion.
// Returns "ok\n" + WGSL or "error\n" + a message, malloc'd in the game's heap.
EM_JS(char*, rs64_tint_convert_here, (const uint32_t* words, uint32_t count), {
    let tint = Module.rs64Tint;

    if (typeof WorkerGlobalScope !== 'undefined')
    {
        tint = self.rs64TintHere;

        if (!tint) {
            try {
                importScripts('rs64tint.js');

                const xhr = new XMLHttpRequest();

                xhr.open('GET', 'rs64tint.wasm', false);
                xhr.responseType = 'arraybuffer';
                xhr.send();

                const wasm = new WebAssembly.Module(xhr.response);
                const instance = {
                    instantiateWasm: (imports, receive) => {
                        const created = new WebAssembly.Instance(wasm, imports);

                        receive(created, wasm);
                        return created.exports;
                    }
                };

                rs64Tint(instance);
                self.rs64TintHere = instance;
                tint = instance;
            } catch (e) {
                return stringToNewUTF8('error\nthis worker couldn\'t load rs64tint: ' + e.message);
            }
        }
    }

    if (!tint || !tint._rs64_tint_convert)
      return stringToNewUTF8('error\nrs64tint is not loaded');

    const first = words >> 2;
    const src = tint._malloc(count * 4);

    tint.HEAPU32.set(HEAPU32.subarray(first, first + count), src >> 2);

    const out = tint._rs64_tint_convert(src, count);
    const text = tint.UTF8ToString(out);

    tint._rs64_tint_free(out);
    tint._free(src);
    return stringToNewUTF8(text);
});

namespace rs64::wgsl {

    namespace {

        static constexpr const char* kCacheDir = "/persist/wgsl";

        ConvertResult parse(char* raw)
        {

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

    ConvertResult convert_on_main(std::vector<uint32_t> words)
    {

        apply_all(words);

        if (emscripten_is_main_runtime_thread())
        {
            return parse(rs64_tint_convert_here(words.data(), uint32_t(words.size())));
        }

        struct Job {
            const std::vector<uint32_t>* words;
            char* raw;
        };

        Job job{ &words, nullptr };

        emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), [](void* arg) {
            Job* j = static_cast<Job*>(arg);

            j->raw = rs64_tint_convert_here(j->words->data(), uint32_t(j->words->size()));
        }, &job);
        return parse(job.raw);
    }

    ConvertResult convert_here(std::vector<uint32_t> words)
    {

        apply_all(words);

        // Converted shaders are kept in the browser's storage (/persist, backed by IndexedDB), so a later visit skips Tint.
        // Bump kCacheVersion when the SPIR-V passes or rs64tint change.
        static constexpr uint64_t kCacheVersion = 1;
        uint64_t hash = 14695981039346656037ULL ^ kCacheVersion;

        for (uint32_t word : words) {
            hash = (hash ^ word) * 1099511628211ULL;
        }

        char name[64];

        std::snprintf(name, sizeof(name), "%s/%016llx.wgsl", kCacheDir, static_cast<unsigned long long>(hash));

        std::ifstream cached(name, std::ios::binary);

        if (cached)
        {
            ConvertResult result;

            result.wgsl.assign(std::istreambuf_iterator<char>(cached), std::istreambuf_iterator<char>());
            result.ok = !result.wgsl.empty();

            if (result.ok)
            {
                return result;
            }
        }

        ConvertResult result = parse(rs64_tint_convert_here(words.data(), uint32_t(words.size())));

        // Written aside and renamed, so another thread converting the same shader never reads half a file.
        if (result.ok) {
            std::error_code ec;
            const std::string temporary = std::string(name) + "." + std::to_string(reinterpret_cast<uintptr_t>(&ec)) + ".tmp";

            std::filesystem::create_directories(kCacheDir, ec);
            std::ofstream(temporary, std::ios::binary) << result.wgsl;
            std::filesystem::rename(temporary, name, ec);
        }

        return result;
    }

}
