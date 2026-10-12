// Counts front-end and mission frames into spare host RDRAM, returns from the first menu frame through rs64_event_return, and records the base exports and &gObjectiveCounts on the first mission frame.
#include "rs64_api.h"

#define MENU_FRAMES (*(volatile unsigned int*)0x80C00000)
#define MISSION_FRAMES (*(volatile unsigned int*)0x80C00004)
#define RETURNS (*(volatile unsigned int*)0x80C00008)

RECOMP_CALLBACK("*", rs64_on_menu_frame) void probe_menu_frame(void) {
    MENU_FRAMES++;
    if (RETURNS == 0) {
        RETURNS = 1;
        rs64_event_return(0);
    }
}

#define EXPORTS ((volatile unsigned int*)0x80C0000C)

extern unsigned int gObjectiveCounts[];

// Exported by the base game's patches ELF (patches/src/sanity.c).
RECOMP_IMPORT("*", unsigned int rs64_patches_sanity(void))

RECOMP_CALLBACK("*", rs64_on_mission_frame_pads) void probe_mission_frame(void) {
    if (MISSION_FRAMES++ == 0) {
        union { float f; unsigned int u; } d, t;
        d.f = rs64_draw_dist();
        t.f = rs64_terrain_dist();
        EXPORTS[0] = d.u;
        EXPORTS[1] = t.u;
        EXPORTS[2] = rs64_tgrid_base(0x80130C70);
        EXPORTS[3] = rs64_relocated(0x80138060);
        EXPORTS[4] = (unsigned int)gObjectiveCounts;
        EXPORTS[5] = rs64_patches_sanity();
    }
}
