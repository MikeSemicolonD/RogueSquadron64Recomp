#pragma once

// SPIR-V rewrites that let Tint convert DXC's output for RT64 to WGSL. Used by the spirv_to_wgsl build tool and the web build's runtime converter.

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rs64::wgsl {

    constexpr uint32_t kHeaderWords = 5;
    constexpr uint32_t kOpCapability = 17;
    constexpr uint32_t kOpControlBarrier = 224;
    constexpr uint32_t kOpLabel = 248;
    constexpr uint32_t kOpBranch = 249;
    constexpr uint32_t kOpSwitch = 251;

    inline uint32_t word_count(uint32_t word)
    {
        return word >> 16;
    }

    inline uint32_t opcode(uint32_t word)
    {
        return word & 0xFFFFu;
    }

    // DXC declares StorageImageReadWithoutFormat (55) for any Texture.Load, which Tint rejects.
    inline bool strip_capability(std::vector<uint32_t>& words, uint32_t capability)
    {

        std::vector<uint32_t> out(words.begin(), words.begin() + kHeaderWords);
        bool changed = false;

        for (size_t i = kHeaderWords; i < words.size();) {
            const uint32_t n = word_count(words[i]);

            if (n == 0)
            {
                return false;
            }

            if (opcode(words[i]) == kOpCapability && words[i + 1] == capability) {
                changed = true;
            }
            else {
                out.insert(out.end(), words.begin() + i, words.begin() + i + n);
            }

            i += n;
        }

        if (changed) {
            words.swap(out);
        }

        return changed;
    }

    // DXC lowers grouped case labels to empty blocks that branch into the next case; Tint rejects that as fallthrough.
    inline uint32_t retarget_switch_chains(std::vector<uint32_t>& words)
    {

        std::unordered_map<uint32_t, uint32_t> jump;
        std::vector<size_t> switches;
        size_t prev = 0;

        for (size_t i = kHeaderWords; i < words.size();) {
            const uint32_t n = word_count(words[i]);

            if (n == 0)
            {
                return 0;
            }

            if (opcode(words[i]) == kOpBranch && prev != 0 && opcode(words[prev]) == kOpLabel) {
                jump[words[prev + 1]] = words[i + 1];
            }

            if (opcode(words[i]) == kOpSwitch) {
                switches.push_back(i);
            }

            prev = i;
            i += n;
        }

        uint32_t changed = 0;

        for (size_t s : switches) {
            const uint32_t n = word_count(words[s]);
            std::unordered_set<uint32_t> targets = { words[s + 2] };

            for (uint32_t j = 4; j < n; j += 2) {
                targets.insert(words[s + j]);
            }

            for (uint32_t j = 4; j < n; j += 2) {
                uint32_t t = words[s + j];
                std::unordered_set<uint32_t> seen;

                while (jump.count(t) && targets.count(jump[t]) && seen.insert(t).second) {
                    t = jump[t];
                }

                if (t != words[s + j]) {
                    words[s + j] = t;
                    ++changed;
                }
            }
        }

        return changed;
    }

    // WGSL barriers are workgroup-scoped; DXC's AllMemoryBarrierWithGroupSync uses device memory scope.
    inline uint32_t workgroup_barrier_scope(std::vector<uint32_t>& words)
    {

        uint32_t changed = 0;

        for (size_t i = kHeaderWords; i < words.size();) {
            const uint32_t n = word_count(words[i]);

            if (n == 0)
            {
                return changed;
            }

            if (opcode(words[i]) == kOpControlBarrier && words[i + 2] != words[i + 1]) {
                words[i + 2] = words[i + 1];
                ++changed;
            }

            i += n;
        }

        return changed;
    }

    inline void apply_all(std::vector<uint32_t>& words)
    {

        strip_capability(words, 55);
        retarget_switch_chains(words);
        workgroup_barrier_scope(words);
    }

}
