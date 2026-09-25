// Any Craft, Any Mission: both craft-mask functions return the vanilla mask with every standard craft added (bit n = craft n: 0 X-wing, 1 Y-wing, 2 A-wing, 3 V-wing, 4 Snowspeeder, 5 Millennium Falcon, 6 TIE Interceptor).
// Vanilla only sets bits above 0x7F from the level's default craft (1 << table[level]); the per-level masks below keep those bits exactly as vanilla does.

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))

#define STANDARD_CRAFTS 0x7Fu

static const volatile unsigned int* const levelDefaultCraft = (const volatile unsigned int*)0x8009EC50;

static unsigned int allCraftsMask(unsigned int level) {
    unsigned int high = (1u << (levelDefaultCraft[level] & 31)) & ~STANDARD_CRAFTS;
    switch (level) {
        case 0x03: case 0x06: case 0x08: case 0x0B: case 0x11: case 0x12:
            high = 0;
            break;
        case 0x10:
            high &= 0x80;
            break;
    }
    return high | STANDARD_CRAFTS;
}

// Crafts selectable in the hangar.
RECOMP_PATCH unsigned int getAvailablePlayerCraftFlagsConsiderUnlocks(unsigned int level) {
    return allCraftsMask(level);
}

// Craft icons listed on SELECT LEVEL.
RECOMP_PATCH unsigned int getAvailablePlayerCraftFlagsIgnoreUnlocks(unsigned int level) {
    return allCraftsMask(level);
}
