// The game's "main" function was renamed to rs_main to avoid clashing with C main().
// Redefine it here so recomp_overlays.inl compiles correctly.
#define main rs_main

#include "../../../N64Recomp/RecompiledFuncs/recomp_overlays.inl"

#undef main

#include "librecomp/overlays.hpp"

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
