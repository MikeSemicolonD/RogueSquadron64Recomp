// Counts front-end and mission frames into spare host RDRAM, returns from the first menu frame through rs64_event_return, and records the base exports and gObjectiveCounts' address (original symbol through rs64_relocated, which moves it while the multiplayer mod is on) on the first mission frame.
#include "rs64_api.h"

#include "recompdata.h"
#include "recomputils.h"

#define MENU_FRAMES (*(volatile unsigned int*)0x80C00000)
#define DATA_RESULTS ((volatile unsigned int*)0x80C00050)

// Expected at 0x80C00050..60: 1234, BEEF, 2, 77, CAFE.
static void check_recompdata(void) {
    unsigned long v = 0;
    U32ValueHashmapHandle vm = recomputil_create_u32_value_hashmap();
    recomputil_u32_value_hashmap_insert(vm, 7, 0x1234);
    recomputil_u32_value_hashmap_get(vm, 7, &v);
    DATA_RESULTS[0] = v;

    U32MemoryHashmapHandle mm = recomputil_create_u32_memory_hashmap(16);
    recomputil_u32_memory_hashmap_create(mm, 3);
    *(unsigned int*)recomputil_u32_memory_hashmap_get(mm, 3) = 0xBEEF;
    DATA_RESULTS[1] = *(unsigned int*)recomputil_u32_memory_hashmap_get(mm, 3);

    U32HashsetHandle hs = recomputil_create_u32_hashset();
    recomputil_u32_hashset_insert(hs, 1);
    recomputil_u32_hashset_insert(hs, 2);
    recomputil_u32_hashset_insert(hs, 2);
    DATA_RESULTS[2] = recomputil_u32_hashset_size(hs);

    U32SlotmapHandle sm = recomputil_create_u32_slotmap();
    collection_key_t key = recomputil_u32_slotmap_create(sm);
    recomputil_u32_slotmap_set(sm, key, 77);
    recomputil_u32_slotmap_get(sm, key, &v);
    DATA_RESULTS[3] = v;

    void* element = 0;
    MemorySlotmapHandle ms = recomputil_create_memory_slotmap(8);
    collection_key_t mkey = recomputil_memory_slotmap_create(ms);
    recomputil_memory_slotmap_get(ms, mkey, &element);
    *(unsigned int*)element = 0xCAFE;
    recomputil_memory_slotmap_get(ms, mkey, &element);
    DATA_RESULTS[4] = *(unsigned int*)element;

    recomputil_destroy_u32_memory_hashmap(mm);
    recomputil_destroy_memory_slotmap(ms);
}
#define MISSION_FRAMES (*(volatile unsigned int*)0x80C00004)
#define RETURNS (*(volatile unsigned int*)0x80C00008)

RECOMP_CALLBACK("*", rs64_on_menu_frame) void probe_menu_frame(void) {
    if (MENU_FRAMES++ == 0) {
        check_recompdata();
        recomp_printf("[event_probe] printf: %d %x %s %.2f %c\n", -42, 0xBEEFu, "ok", 1.5, 'Z');
        recomp_printf("[event_probe] long:%3000d|\n", 7);
    }
    if (RETURNS == 0) {
        RETURNS = 1;
        rs64_event_return(0);
    }
}

#define EXPORTS ((volatile unsigned int*)0x80C0000C)

extern unsigned int gObjectiveCounts[];

// Exported by the base game's patches ELF (patches/src/sanity.c).
RECOMP_IMPORT("*", unsigned int rs64_patches_sanity(void))

// Mission frames 30-39 run at double the frame time read back from f20, so the mission clock D_80130BA4 (which adds f20 each frame) gains 2/30 s on those frames.
RECOMP_CALLBACK("*", rs64_on_mission_frame_dt) void probe_frame_dt(void) {
    if (MISSION_FRAMES >= 30 && MISSION_FRAMES < 40) {
        rs64_event_set_fpr(20, rs64_event_get_fpr(20) * 2.0f);
    }
}

RECOMP_CALLBACK("*", rs64_on_mission_frame_pads) void probe_mission_frame(void) {
    if (MISSION_FRAMES++ == 0) {
        union { float f; unsigned int u; } d, t;
        d.f = rs64_draw_dist();
        t.f = rs64_terrain_dist();
        EXPORTS[0] = d.u;
        EXPORTS[1] = t.u;
        EXPORTS[2] = rs64_tgrid_base(0x80130C70);
        EXPORTS[3] = rs64_relocated(0x80138060);
        EXPORTS[4] = rs64_relocated((unsigned int)gObjectiveCounts);
        EXPORTS[5] = rs64_patches_sanity();
    }
}
