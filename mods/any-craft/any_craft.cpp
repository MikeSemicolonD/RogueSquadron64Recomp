// Any Craft, Any Mission: a native-library mod for the base game's craft-mask filters.
// Each filter receives the game's mask in r4 and returns the new mask in r2; bit n = craft n
// (0 X-wing, 1 Y-wing, 2 A-wing, 3 V-wing, 4 Snowspeeder, 5 Millennium Falcon, 6 TIE Interceptor).

#include <cstdint>

#ifdef _WIN32
#define MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define MOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Prefix of recomp_context: the 32 general-purpose registers as 64-bit values.
struct RecompRegs { uint64_t r[32]; };

// librecomp checks this on load; must be 1.
MOD_EXPORT uint32_t recomp_api_version = 1;

constexpr uint64_t kStandardCrafts = 0x7F;

// Crafts selectable in the hangar.
MOD_EXPORT void hangar_craft_mask(uint8_t* rdram, void* ctx) {
    (void)rdram;
    RecompRegs* regs = static_cast<RecompRegs*>(ctx);
    regs->r[2] = regs->r[4] | kStandardCrafts;
}

// Craft icons listed on SELECT LEVEL.
MOD_EXPORT void level_craft_icons(uint8_t* rdram, void* ctx) {
    (void)rdram;
    RecompRegs* regs = static_cast<RecompRegs*>(ctx);
    regs->r[2] = regs->r[4] | kStandardCrafts;
}
