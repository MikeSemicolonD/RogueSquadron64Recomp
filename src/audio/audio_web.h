#pragma once

#include <cstdint>

// Browser audio output for audio.cpp: SDL2's web audio can't be driven from the game's worker thread.
namespace rs64::audio::web {

    // Returns a nonzero device id, like SDL_OpenAudioDevice.
    uint32_t open(int freq);
    void close();
    int queue(const void* data, uint32_t bytes);
    uint32_t queued_bytes();

}
