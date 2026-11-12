#include "../tools/wgsl/spirv_web_passes.h"
#include "check.h"
#include <cstdint>
#include <vector>

namespace {

    constexpr uint32_t OpNop = 0;
    constexpr uint32_t OpCapability = 17;
    constexpr uint32_t OpControlBarrier = 224;
    constexpr uint32_t OpLabel = 248;
    constexpr uint32_t OpBranch = 249;
    constexpr uint32_t OpSwitch = 251;

    uint32_t op(uint32_t code, uint32_t words)
    {
        return (words << 16) | code;
    }

    std::vector<uint32_t> module(std::vector<uint32_t> body)
    {

        std::vector<uint32_t> words = { 0x07230203u, 0x00010000u, 0u, 100u, 0u };

        words.insert(words.end(), body.begin(), body.end());
        return words;
    }

    void strip_capability_removes_only_55()
    {

        auto words = module({ op(OpCapability, 2), 1, op(OpCapability, 2), 55 });
        const size_t before = words.size();

        CHECK(rs64::wgsl::strip_capability(words, 55));
        CHECK(words.size() == before - 2);
        CHECK(words[5] == op(OpCapability, 2));
        CHECK(words[6] == 1);
        CHECK(!rs64::wgsl::strip_capability(words, 55));
    }

    void switch_chain_retargets()
    {

        // %sel = 1, default %d = 10, cases 0 -> %a (11), 2 -> %b (12), 3 -> %c (13); %b only branches to %c.
        auto words = module({
            op(OpSwitch, 9), 1, 10, 0, 11, 2, 12, 3, 13,
            op(OpLabel, 2), 12, op(OpBranch, 2), 13,
            op(OpLabel, 2), 13, op(OpBranch, 2), 99,
            op(OpLabel, 2), 11, op(OpBranch, 2), 99
        });

        CHECK(rs64::wgsl::retarget_switch_chains(words) == 1);
        CHECK(words[5 + 4] == 11);
        CHECK(words[5 + 5] == 2);
        CHECK(words[5 + 6] == 13);
        CHECK(words[5 + 8] == 13);
    }

    void switch_leaves_real_blocks()
    {

        // %b (12) does work before branching to %c, so case 2 keeps its own block.
        auto words = module({
            op(OpSwitch, 7), 1, 10, 2, 12, 3, 13,
            op(OpLabel, 2), 12, op(OpNop, 1), op(OpBranch, 2), 13,
            op(OpLabel, 2), 13, op(OpBranch, 2), 99
        });

        CHECK(rs64::wgsl::retarget_switch_chains(words) == 0);
        CHECK(words[5 + 4] == 12);
    }

    void barrier_scope_set()
    {

        // OpControlBarrier %exec(20 = Workgroup) %mem(21 = Device) %sem(22).
        auto words = module({ op(OpControlBarrier, 4), 20, 21, 22 });

        CHECK(rs64::wgsl::workgroup_barrier_scope(words) == 1);
        CHECK(words[5 + 1] == 20);
        CHECK(words[5 + 2] == 20);
        CHECK(words[5 + 3] == 22);
    }

    void push_constants_become_uniform()
    {

        constexpr uint32_t OpTypePointer = 32;
        constexpr uint32_t OpVariable = 59;
        constexpr uint32_t OpDecorate = 71;
        constexpr uint32_t PushConstant = 9;
        constexpr uint32_t Uniform = 2;

        // %40 = Block struct, %41 = PushConstant pointer, %42 = the variable, %43 = a member pointer an access chain uses.
        auto words = module({
            op(OpDecorate, 3), 40, 2,
            op(OpTypePointer, 4), 41, PushConstant, 40,
            op(OpTypePointer, 4), 43, PushConstant, 7,
            op(OpVariable, 4), 41, 42, PushConstant
        });

        CHECK(rs64::wgsl::push_constants_to_uniform(words, 0, 999) == 1);
        CHECK(words == module({
            op(OpDecorate, 3), 40, 2,
            op(OpDecorate, 4), 42, 34, 0,
            op(OpDecorate, 4), 42, 33, 999,
            op(OpTypePointer, 4), 41, Uniform, 40,
            op(OpTypePointer, 4), 43, Uniform, 7,
            op(OpVariable, 4), 41, 42, Uniform
        }));
        CHECK(rs64::wgsl::push_constants_to_uniform(words, 0, 999) == 0);
    }

    void apply_all_identity()
    {

        const auto original = module({ op(OpCapability, 2), 1, op(OpLabel, 2), 30, op(OpNop, 1), op(OpBranch, 2), 31 });
        auto words = original;

        rs64::wgsl::apply_all(words);
        CHECK(words == original);
    }

}

int main()
{

    strip_capability_removes_only_55();
    switch_chain_retargets();
    switch_leaves_real_blocks();
    barrier_scope_set();
    push_constants_become_uniform();
    apply_all_identity();
    std::printf("spirv_web_passes_test OK\n");
    return 0;
}
