// Any Craft, Any Mission: both craft-mask functions return the vanilla mask with every standard craft added (bit n = craft n: 0 X-wing, 1 Y-wing, 2 A-wing, 3 V-wing, 4 Snowspeeder, 5 Millennium Falcon, 6 TIE Interceptor).
// Vanilla only sets bits above 0x7F from the level's default craft (1 << table[level]); the per-level masks below keep those bits exactly as vanilla does.

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))
#define RECOMP_HOOK(func) __attribute__((section(".recomp_hook." func)))

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

// Gliders (V-wing, snowspeeder) cap their altitude at hover height (+0x108) above the ground under them (func_800AF04C).
// Where that ground lies below the level's floor (Taloraan's heightmap is ~620 units down) they sink to it, so there the hover height is raised until the cap becomes the level ceiling, as for any other craft.

#define PLAYER_CRAFT     0x80137DBCu
#define CRAFT_TYPE(c)    (*(volatile unsigned short*)((c) + 0xB4))
#define CRAFT_X(c)       (*(volatile float*)((c) + 0x4))
#define CRAFT_Z(c)       (*(volatile float*)((c) + 0xC))
#define CRAFT_HOVER(c)   (*(volatile float*)((c) + 0x108))
#define LEVEL_CEILING_Y  (*(volatile float*)0x8010B7A0)
#define LEVEL_FLOOR_Y    (*(volatile float*)0x8010B7A4)

// Terrain height (world Y, down positive) at x, z; the third argument is unused and the fourth is an optional surface record.
float func_80067D90(float x, float z, void* unused, void* surface);

static float gliderHover;
static float gliderHoverWritten;

static void fitGliderToLevel(void) {
    const unsigned int craft = PLAYER_CRAFT;
    const unsigned short type = CRAFT_TYPE(craft);
    if (type != 3 && type != 4) {
        return;
    }
    if (CRAFT_HOVER(craft) != gliderHoverWritten) {
        gliderHover = CRAFT_HOVER(craft);
    }
    const float groundY = func_80067D90(CRAFT_X(craft), CRAFT_Z(craft), 0, 0);
    float hover = gliderHover;
    if (gliderHover - groundY < -LEVEL_FLOOR_Y) {
        hover = -LEVEL_CEILING_Y + groundY + 1.0f;
    }
    CRAFT_HOVER(craft) = hover;
    gliderHoverWritten = hover;
}

RECOMP_HOOK("playerVwingUpdate") void anyCraftVwingUpdate(void) {
    fitGliderToLevel();
}

RECOMP_HOOK("playerSnowSpeederUpdate") void anyCraftSnowSpeederUpdate(void) {
    fitGliderToLevel();
}
