#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rs64::audio {

    // Interleaved int16 stereo frames from one writer to one reader (the browser's audio worklet), without locks.
    class AudioRing
    {
    public:
        explicit AudioRing(uint32_t capacity_frames) : capacity_(capacity_frames), data_(size_t(capacity_frames) * 2) {}

        // Frames that don't fit are dropped; returns the frames kept.
        uint32_t push(const int16_t* frames, uint32_t count)
        {

            const uint64_t write = write_.load(std::memory_order_relaxed);
            const uint64_t read = read_.load(std::memory_order_acquire);
            const uint32_t kept = std::min<uint32_t>(count, uint32_t(capacity_ - (write - read)));

            for (uint32_t i = 0; i < kept; i++) {
                const size_t at = size_t((write + i) % capacity_) * 2;

                data_[at] = frames[i * 2];
                data_[at + 1] = frames[i * 2 + 1];
            }

            write_.store(write + kept, std::memory_order_release);
            return kept;
        }

        // Fills count frames, silence past what is queued; returns the frames read.
        uint32_t pop(int16_t* out, uint32_t count)
        {

            const uint64_t read = read_.load(std::memory_order_relaxed);
            const uint64_t write = write_.load(std::memory_order_acquire);
            const uint32_t available = std::min<uint32_t>(count, uint32_t(write - read));

            for (uint32_t i = 0; i < available; i++) {
                const size_t at = size_t((read + i) % capacity_) * 2;

                out[i * 2] = data_[at];
                out[i * 2 + 1] = data_[at + 1];
            }

            std::memset(out + size_t(available) * 2, 0, size_t(count - available) * 2 * sizeof(int16_t));
            read_.store(read + available, std::memory_order_release);
            return available;
        }

        // Drops up to count queued frames, as if played; returns the frames dropped.
        uint32_t discard(uint32_t count)
        {

            const uint64_t read = read_.load(std::memory_order_relaxed);
            const uint64_t write = write_.load(std::memory_order_acquire);
            const uint32_t dropped = std::min<uint32_t>(count, uint32_t(write - read));

            read_.store(read + dropped, std::memory_order_release);
            return dropped;
        }

        uint32_t queued_frames() const
        {
            return uint32_t(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire));
        }

        uint64_t consumed_frames() const
        {
            return read_.load(std::memory_order_acquire);
        }

    private:
        uint32_t capacity_;
        std::vector<int16_t> data_;
        std::atomic<uint64_t> write_{0};
        std::atomic<uint64_t> read_{0};
    };

}
