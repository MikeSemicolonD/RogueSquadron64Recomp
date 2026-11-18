#include "audio/audio_web.h"
#include "audio/audio_ring.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>

EM_JS_DEPS(rs64_audio_web, "$emscriptenGetAudioObject");

// Plays through an AudioWorklet that reads the game's frames from a ring in shared memory. Browsers hold audio until a click or key, and
// the game paces itself on what is queued, so while the worklet isn't pulling frames the ring drains on the clock as if played.
namespace {

    using Clock = std::chrono::steady_clock;

    // Half a second at the highest rate the game opens.
    constexpr uint32_t kRingFrames = 24000;
    constexpr int kWorkletFrames = 128;
    constexpr const char* kProcessorName = "rs64-output";

    rs64::audio::AudioRing g_ring(kRingFrames);
    std::mutex g_mutex;
    int g_rate = 0;
    uint64_t g_seen_consumed = 0;
    Clock::time_point g_seen_at;
    Clock::time_point g_drained_at;

    EMSCRIPTEN_WEBAUDIO_T g_context = 0;
    int g_context_rate = 0;
    alignas(16) uint8_t g_worklet_stack[16 * 1024];

    bool process(int, const AudioSampleFrame*, int output_count, AudioSampleFrame* outputs, int, const AudioParamFrame*, void*)
    {

        if (output_count < 1)
        {
            return true;
        }

        AudioSampleFrame& out = outputs[0];
        const int frames = std::min(out.samplesPerChannel, kWorkletFrames);
        int16_t pcm[kWorkletFrames * 2];

        g_ring.pop(pcm, uint32_t(frames));

        for (int i = 0; i < frames; i++) {
            out.data[i] = pcm[i * 2] / 32768.0f;

            if (out.numberOfChannels > 1) {
                out.data[out.samplesPerChannel + i] = pcm[i * 2 + 1] / 32768.0f;
            }
        }

        return true;
    }

    void node_created(EMSCRIPTEN_WEBAUDIO_T context, bool ok, void*)
    {

        if (!ok)
        {
            return;
        }

        int channels[1] = { 2 };
        EmscriptenAudioWorkletNodeCreateOptions options = {};

        options.numberOfInputs = 0;
        options.numberOfOutputs = 1;
        options.outputChannelCounts = channels;

        const EMSCRIPTEN_WEBAUDIO_T node = emscripten_create_wasm_audio_worklet_node(context, kProcessorName, &options, process, nullptr);

        emscripten_audio_node_connect(node, context, 0, 0);
    }

    void worklet_started(EMSCRIPTEN_WEBAUDIO_T context, bool ok, void*)
    {

        if (!ok)
        {
            return;
        }

        WebAudioWorkletProcessorCreateOptions options = {};

        options.name = kProcessorName;
        emscripten_create_wasm_audio_worklet_processor_async(context, &options, node_created, nullptr);
    }

    // Main thread: an AudioContext at the game's rate, so the browser resamples, and the page's handle to resume it on a click.
    void start_context(void* rate_arg)
    {

        const int rate = int(reinterpret_cast<intptr_t>(rate_arg));

        if (g_context != 0 && g_context_rate == rate)
        {
            return;
        }

        if (g_context != 0) {
            emscripten_destroy_audio_context(g_context);
        }

        EmscriptenWebAudioCreateAttributes attributes = {};

        attributes.latencyHint = "interactive";
        attributes.sampleRate = uint32_t(rate);
        g_context = emscripten_create_audio_context(&attributes);
        g_context_rate = rate;
        EM_ASM({ Module.rs64AudioContext = emscriptenGetAudioObject($0); }, g_context);
        emscripten_start_wasm_audio_worklet_thread_async(g_context, g_worklet_stack, sizeof(g_worklet_stack), worklet_started, nullptr);
    }

    // While the worklet isn't pulling (audio still locked, or the tab hidden), frames older than the clock count as played.
    void drain_unplayed()
    {

        const Clock::time_point now = Clock::now();
        const uint64_t consumed = g_ring.consumed_frames();

        if (consumed != g_seen_consumed) {
            g_seen_consumed = consumed;
            g_seen_at = now;
            g_drained_at = now;
            return;
        }

        if ((now - g_seen_at) < std::chrono::milliseconds(100))
        {
            return;
        }

        const double elapsed = std::chrono::duration<double>(now - g_drained_at).count();

        g_drained_at = now;
        g_seen_consumed += g_ring.discard(uint32_t(elapsed * g_rate));
    }

}

uint32_t rs64::audio::web::open(int freq)
{

    std::lock_guard<std::mutex> lock(g_mutex);

    g_rate = freq;
    g_ring.discard(g_ring.queued_frames());
    g_seen_consumed = g_ring.consumed_frames();
    g_seen_at = Clock::now();
    g_drained_at = g_seen_at;
    emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), start_context, reinterpret_cast<void*>(intptr_t(freq)));
    return 1;
}

void rs64::audio::web::close()
{

    std::lock_guard<std::mutex> lock(g_mutex);

    g_ring.discard(g_ring.queued_frames());
}

int rs64::audio::web::queue(const void* data, uint32_t bytes)
{

    std::lock_guard<std::mutex> lock(g_mutex);

    drain_unplayed();
    g_ring.push(static_cast<const int16_t*>(data), bytes / 4);
    return 0;
}

uint32_t rs64::audio::web::queued_bytes()
{

    std::lock_guard<std::mutex> lock(g_mutex);

    drain_unplayed();
    return g_ring.queued_frames() * 4u;
}
