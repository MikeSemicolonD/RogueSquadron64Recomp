#include "../lib/rt64/src/hle/rt64_rs64_transition.h"
#include <cassert>
#include <cstdio>

using namespace rs64transition;

int main() {
    assert(isBlankVStart(0x00250025u));
    assert(!isBlankVStart(0x002501FFu));
    assert(isBlankVStart(0u));

    assert(viShowsBuffer(0x790000u, 0x790000u));
    assert(viShowsBuffer(0x790400u, 0x790000u));
    assert(viShowsBuffer(0x80790400u, 0x80790000u));
    assert(!viShowsBuffer(0x791000u, 0x790000u));
    assert(!viShowsBuffer(0x720000u, 0x790000u));

    Gate g;
    assert(!g.shouldBlack(0x790000u, 0));

    // VI moves off the zeroed buffer: the game swapped to a filled one.
    g.arm(0x790000u, 10);
    assert(g.shouldBlack(0x790400u, 0));
    assert(g.shouldBlack(0x790000u, 0));
    assert(!g.shouldBlack(0x720000u, 0));
    assert(g.armedFb() == 0);
    assert(!g.shouldBlack(0x790000u, 0));

    // Single-buffered screen: a draw from a workload at or before the clear does not count, a later one does.
    g.arm(0x790000u, 10);
    assert(g.shouldBlack(0x790000u, 10));
    assert(g.shouldBlack(0x790000u, 9));
    assert(!g.shouldBlack(0x790000u, 11));
    assert(g.armedFb() == 0);

    // Cap: never black for more than kGateCap presents.
    g.arm(0x790000u, 10);
    for (int i = 0; i < kGateCap; ++i) assert(g.shouldBlack(0x790000u, 0));
    assert(!g.shouldBlack(0x790000u, 0));

    // Re-arm follows the newest clear.
    g.arm(0x790000u, 10);
    g.arm(0x720000u, 12);
    assert(!g.shouldBlack(0x790000u, 0));
    g.arm(0x720000u, 12);
    assert(g.shouldBlack(0x720000u, 12));

    printf("transition_gate_test OK\n");
    return 0;
}
