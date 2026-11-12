#include "audio/audio_ring.h"
#include "check.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

    std::vector<int16_t> frames(uint32_t count, int16_t first)
    {

        std::vector<int16_t> out(count * 2);

        for (uint32_t i = 0; i < count; i++) {
            out[i * 2] = int16_t(first + i);
            out[i * 2 + 1] = int16_t(-(first + int16_t(i)));
        }

        return out;
    }

    void pop_past_queued_pads_with_silence()
    {

        rs64::audio::AudioRing ring(256);
        const std::vector<int16_t> in = frames(100, 1);

        CHECK(ring.push(in.data(), 100) == 100);
        CHECK(ring.queued_frames() == 100);

        std::vector<int16_t> out(150 * 2, 77);

        CHECK(ring.pop(out.data(), 150) == 100);
        CHECK(out[0] == 1);
        CHECK(out[1] == -1);
        CHECK(out[99 * 2] == 100);
        CHECK(out[100 * 2] == 0);
        CHECK(out[149 * 2 + 1] == 0);
        CHECK(ring.queued_frames() == 0);
        CHECK(ring.consumed_frames() == 100);
    }

    void wraparound_keeps_order()
    {

        rs64::audio::AudioRing ring(64);
        std::vector<int16_t> out(40 * 2);

        for (int round = 0; round < 5; round++) {
            const std::vector<int16_t> in = frames(40, int16_t(round * 40));

            CHECK(ring.push(in.data(), 40) == 40);
            CHECK(ring.pop(out.data(), 40) == 40);
            CHECK(out[0] == round * 40);
            CHECK(out[39 * 2] == round * 40 + 39);
        }

        CHECK(ring.consumed_frames() == 200);
    }

    void full_ring_drops_the_excess()
    {

        rs64::audio::AudioRing ring(64);
        const std::vector<int16_t> in = frames(100, 1);

        CHECK(ring.push(in.data(), 100) == 64);
        CHECK(ring.queued_frames() == 64);
        CHECK(ring.discard(10) == 10);
        CHECK(ring.queued_frames() == 54);
        CHECK(ring.consumed_frames() == 10);
    }

}

int main()
{

    pop_past_queued_pads_with_silence();
    wraparound_keeps_order();
    full_ring_drops_the_excess();
    std::printf("audio_ring_test OK\n");
    return 0;
}
