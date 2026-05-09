// The game's "main" function was renamed to rs_main to avoid clashing with C main().
// Redefine it here so recomp_overlays.inl compiles correctly.
#define main rs_main

#include "../../../N64Recomp/RecompiledFuncs/recomp_overlays.inl"

#undef main

#include "librecomp/overlays.hpp"

// Rogue Squadron loads overlay code on-demand via PI DMA. After each ROM read,
// we ask librecomp to register the recompiled functions for that range so
// subsequent jumps dispatch correctly. load_overlays internally bounds-checks
// against known overlay sections — non-overlay asset reads are no-ops.
static void rs64_post_pi_dma(uint32_t rom_offset, int32_t rdram_address, uint32_t size) {
    load_overlays(rom_offset, rdram_address, size);
}

void rs64_register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
    recomp::set_post_pi_dma_callback(&rs64_post_pi_dma);
}
