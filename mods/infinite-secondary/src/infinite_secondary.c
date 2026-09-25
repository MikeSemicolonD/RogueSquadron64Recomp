// Infinite Secondary: every craft/weapon starts the mission at 0xFF, which the game treats as unlimited (the decrement at 0x800FD2E8 skips it).

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))

RECOMP_PATCH unsigned int getSecondaryWeaponCount(unsigned int craft, unsigned int weapon) {
    return 0xFF;
}
