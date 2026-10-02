#include "../lib/rt64/src/hle/rt64_rs64_cable.h"
#include "check.h"
#include <cstdio>

using namespace rs64cable;

static CableState state(uint8_t phase, uint8_t head, uint8_t count) {
    return CableState{ phase, head, count };
}

int main() {
    const uint32_t pool = 0x80200000u;
    const auto inst = [&](uint32_t i) { return pool + i * kSlotStride + kInstanceOffset; };

    // slotIndex: only exact slot mesh-instance pointers inside the 255-slot window count.
    CHECK(slotIndex(pool, inst(0)) == 0);
    CHECK(slotIndex(pool, inst(150)) == 150);
    CHECK(slotIndex(pool, inst(0) + 4) < 0);
    CHECK(slotIndex(pool, pool) < 0);
    CHECK(slotIndex(pool, pool - 0x100u) < 0);
    CHECK(slotIndex(pool, inst(255)) < 0);
    CHECK(slotIndex(0u, inst(0)) < 0);

    // The scene node the traversal maps for a segment sits 0x0C into its mesh instance (measured: instance 80352D40 -> node 80352D4C).
    CHECK(sceneNodeOf(0x80352D40u) == 0x80352D4Cu);

    // State word as MEM_W reads it at 0x8010B6E8: phase, head, count from the top byte down.
    const CableState w = CableState::fromWord(0x02053C00u);
    CHECK(w.phase == 2);
    CHECK(w.head == 5);
    CHECK(w.count == 60);

    // isDrawnSlot: only the slots the renderer draws, (head - i) mod N for i < count.
    CHECK(isDrawnSlot(5, state(2, 5, 6)));
    CHECK(isDrawnSlot(0, state(2, 5, 6)));
    CHECK(!isDrawnSlot(6, state(2, 5, 6)));
    CHECK(isDrawnSlot(3, state(2, 5, 3)));
    CHECK(!isDrawnSlot(2, state(2, 5, 3)));
    // Wrapped ring (count == N): every slot below count is drawn, nothing past it.
    CHECK(isDrawnSlot(59, state(2, 2, 60)));
    CHECK(!isDrawnSlot(60, state(2, 2, 60)));
    // No cable out (phase 0 or count 0), e.g. a stale pool pointer after the cable was freed.
    CHECK(!isDrawnSlot(0, state(0, 0, 5)));
    CHECK(!isDrawnSlot(0, state(2, 0, 0)));
    CHECK(!isDrawnSlot(-1, state(2, 5, 6)));

    // Roles: frame order in the game is cable submit (bucket fill) -> scene traversal (onFrame at the ring flip, then node lookups).
    CableRoles r;
    r.onFrame();
    CHECK(r.aliasFor(0x100u) == 0);

    // Frame 1: segments submitted ship end first.
    r.onSubmit(0x100u, state(2, 2, 3));
    r.onSubmit(0x200u, state(2, 2, 3));
    r.onSubmit(0x300u, state(2, 2, 3));
    r.onFrame();
    const uint32_t role0 = r.aliasFor(0x100u);
    const uint32_t role1 = r.aliasFor(0x200u);
    CHECK(role0 != 0);
    CHECK((role0 >> 28) == 0x1);
    CHECK(role1 != 0);
    CHECK(role1 != role0);
    CHECK(r.aliasFor(0x999u) == 0);

    // Frame 2, no advance: the same nodes keep the same ids.
    r.onSubmit(0x100u, state(2, 2, 3));
    r.onSubmit(0x200u, state(2, 2, 3));
    r.onSubmit(0x300u, state(2, 2, 3));
    r.onFrame();
    CHECK(r.aliasFor(0x100u) == role0);
    CHECK(r.aliasFor(0x200u) == role1);

    // Frame 3, the head advanced: the new ship-side node takes role 0 and every older segment moves down one role, so nothing is left without a partner.
    r.onSubmit(0x400u, state(2, 3, 4));
    r.onSubmit(0x100u, state(2, 3, 4));
    r.onSubmit(0x200u, state(2, 3, 4));
    r.onSubmit(0x300u, state(2, 3, 4));
    r.onFrame();
    CHECK(r.aliasFor(0x400u) == role0);
    CHECK(r.aliasFor(0x100u) == role1);

    // Ids expire after a frame with no cable submit.
    r.onFrame();
    CHECK(r.aliasFor(0x400u) == 0);

    // A gap starts a new generation: the next cable's segments must not pair with the old one.
    r.onSubmit(0x500u, state(2, 0, 1));
    r.onFrame();
    const uint32_t gen2role0 = r.aliasFor(0x500u);
    CHECK(gen2role0 != 0);
    CHECK(gen2role0 != role0);

    // Re-fire without a gap: the old cable is dropping (phase 4) and the next frame a new one is fired (phase 1/2); equal counts, so only the phase change can tell.
    r.onSubmit(0x600u, state(4, 0, 1));
    r.onFrame();
    const uint32_t dropping = r.aliasFor(0x600u);
    r.onSubmit(0x700u, state(2, 0, 1));
    r.onFrame();
    CHECK(r.aliasFor(0x700u) != 0);
    CHECK(r.aliasFor(0x700u) != dropping);

    // The segment count falling (ring restarted) also starts a new generation.
    r.onSubmit(0x800u, state(2, 9, 10));
    r.onFrame();
    const uint32_t before = r.aliasFor(0x800u);
    r.onSubmit(0x900u, state(2, 1, 2));
    r.onFrame();
    CHECK(r.aliasFor(0x900u) != before);

    std::printf("cable_alias_test: OK\n");
    return 0;
}
