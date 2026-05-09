// The game's "main" function was renamed to rs_main to avoid clashing with C main().
// Redefine it here so recomp_overlays.inl compiles correctly.
#define main rs_main

#include "../../../N64Recomp/RecompiledFuncs/recomp_overlays.inl"

#undef main

#include "librecomp/overlays.hpp"

// All overlays registered at boot; librecomp's section table covers the
// 3 .ovl.* overlays (mission/menu/cinematic), all sharing ram_addr
// 0x800A5130. The DMA-time per-overlay callback (`set_post_pi_dma_callback`)
// that was previously here is non-canonical — Zelda64Recomp registers
// overlays at boot only. If runtime DMA-driven overlay switching turns
// out to be needed for RS64, the right place is a thin wrapper inside
// our own load_overlays, not a librecomp modification.
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
}
