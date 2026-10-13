// NPC health accessors (mission overlay 0x800F2070-0x800F21FC) with a guard on the per-difficulty slot. A structure's NPC torn down mid-explosion leaves npc+0x190 garbage, and the slot read/write AVs (Jade Moon AT-PTs that won't die, the demo gfx-barrier freeze). Bad slot = health 0, damage dropped; otherwise each matches the original instruction for instruction.
#include "rs64_api.h"

#define NPC_HEALTH_TABLE_INDEX (*(volatile unsigned int*)0x80137CE4)
#define HEALTH_PCT_SCALE_ARRAY (*(const float*)0x800A89D8)
#define HEALTH_PCT_SCALE_DIRECT (*(const float*)0x800A89DC)

#define IS_DIRECT(npc) (*(unsigned char*)((npc) + 0x1A7) != 0)
#define FIELD(npc, off) (*(int*)((npc) + (off)))

// Host side (register_patches.cpp, manual patch symbol in patches/syms.ld): the game's 8 MB or a host-RAM range a mod registered (e.g. the co-op imposter's DAT record).
int rs64_host_ram_ok(unsigned int addr);

static int slot_ok(unsigned int addr) {
    return ((addr & 0xE0000000u) == 0x80000000u && (addr & 0x1FFFFFFFu) < 0x800000u) || rs64_host_ram_ok(addr);
}

static unsigned int slot_addr(unsigned int base) {
    return base + (NPC_HEALTH_TABLE_INDEX << 2);
}

RECOMP_PATCH int setNpcHealth(char* npc, unsigned int health_array, char* info) {
    unsigned int addr = slot_addr(health_array);
    int slot = slot_ok(addr) ? *(int*)addr : 0;
    if (!IS_DIRECT(npc)) {
        FIELD(npc, 0x190) = (int)health_array;
        if ((unsigned int)FIELD(info, 0x54) == 0x80000000u) {
            FIELD(info, 0x54) = slot;
        }
        FIELD(npc, 0x194) = FIELD(info, 0x54);
    } else {
        FIELD(npc, 0x190) = slot;
        FIELD(npc, 0x194) = slot;
    }
    return slot;
}

RECOMP_PATCH int getNpcCurrentHealth(char* npc) {
    if (IS_DIRECT(npc)) {
        return FIELD(npc, 0x190);
    }
    unsigned int addr = slot_addr((unsigned int)FIELD(npc, 0x190));
    return slot_ok(addr) ? *(int*)addr : 0;
}

RECOMP_PATCH int dealDamageToNpc(char* npc, int damage) {
    if (IS_DIRECT(npc)) {
        FIELD(npc, 0x190) -= damage;
        return FIELD(npc, 0x190);
    }
    unsigned int addr = slot_addr((unsigned int)FIELD(npc, 0x190));
    if (!slot_ok(addr)) {
        return 0;
    }
    *(int*)addr -= damage;
    return *(int*)addr;
}

RECOMP_PATCH int getNpcMissingHealth(char* npc) {
    if (IS_DIRECT(npc)) {
        return FIELD(npc, 0x194) - FIELD(npc, 0x190);
    }
    unsigned int addr = slot_addr((unsigned int)FIELD(npc, 0x190));
    if (!slot_ok(addr)) {
        return FIELD(npc, 0x194);
    }
    return FIELD(npc, 0x194) - *(int*)addr;
}

RECOMP_PATCH float getNpcHealthPercentage(char* npc) {
    if (IS_DIRECT(npc)) {
        return (float)FIELD(npc, 0x190) / (float)FIELD(npc, 0x194) * HEALTH_PCT_SCALE_DIRECT;
    }
    unsigned int addr = slot_addr((unsigned int)FIELD(npc, 0x190));
    if (!slot_ok(addr)) {
        return 0.0f;
    }
    return (float)*(int*)addr / (float)FIELD(npc, 0x194) * HEALTH_PCT_SCALE_ARRAY;
}
