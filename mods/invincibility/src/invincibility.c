// Invincibility: sets the game's unused per-player god-mode bit (0x800000 in gGameSettings+0xC), which skips weapon hits, terrain scrapes and ground crashes.
// The save loader only restores bits 0x7FFE00 of that word, so the bit never persists once the mod is off.

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))

#define gGameSettingsFlags (*(unsigned int*)0x80130B4C)
#define gMissionUnlockBits (*(unsigned int*)0x80130B18)

#define UNLOCK_MASK     0x007FFE00
#define GOD_MODE_BIT    0x00800000

// gatherActiveUnlockFlags, called by initMission.
RECOMP_PATCH void gatherActiveUnlockFlags(void) {
    gGameSettingsFlags |= GOD_MODE_BIT;
    gMissionUnlockBits = gGameSettingsFlags & UNLOCK_MASK;
}
