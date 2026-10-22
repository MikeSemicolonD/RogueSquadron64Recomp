#pragma once

#include <cstddef>
#include <cstdint>
#include "librecomp/rsp.hpp"

// RSP microcode dispatch (MusyX synth) and the SDL audio output behind ultramodern's audio callbacks.
namespace rs64::audio {
    RspUcodeFunc* get_rsp_microcode(const OSTask* task);
    void queue_samples(int16_t* samples, size_t num_samples);
    size_t get_frames_remaining();
    void set_frequency(uint32_t freq);
#ifdef __ANDROID__
    // In the background the game keeps running but its audio is dropped; returning reopens the device.
    void set_foreground(bool on);
    bool foreground();
#endif
}
