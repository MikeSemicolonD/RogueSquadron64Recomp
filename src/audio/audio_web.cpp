#include "audio/audio_web.h"

#include <chrono>
#include <mutex>

// shortcut: samples are counted and drained on the clock at the output rate but not played, which paces the game's audio loop; replace with an AudioWorklet output for sound.
namespace {

    std::mutex g_mutex;
    int g_rate = 0;
    double g_queued_frames = 0.0;
    std::chrono::steady_clock::time_point g_last;

    void drain()
    {

        const auto now = std::chrono::steady_clock::now();
        const double played = std::chrono::duration<double>(now - g_last).count() * g_rate;

        g_last = now;
        g_queued_frames = (g_queued_frames > played) ? (g_queued_frames - played) : 0.0;
    }

}

uint32_t rs64::audio::web::open(int freq)
{

    std::lock_guard<std::mutex> lock(g_mutex);

    g_rate = freq;
    g_queued_frames = 0.0;
    g_last = std::chrono::steady_clock::now();
    return 1;
}

void rs64::audio::web::close()
{

    std::lock_guard<std::mutex> lock(g_mutex);

    g_queued_frames = 0.0;
}

int rs64::audio::web::queue(const void*, uint32_t bytes)
{

    std::lock_guard<std::mutex> lock(g_mutex);

    drain();
    g_queued_frames += bytes / 4.0;
    return 0;
}

uint32_t rs64::audio::web::queued_bytes()
{

    std::lock_guard<std::mutex> lock(g_mutex);

    drain();
    return uint32_t(g_queued_frames) * 4u;
}
