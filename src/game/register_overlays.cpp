// The game's "main" function was renamed to rs_main to avoid clashing with C main().
// Redefine it here so recomp_overlays.inl compiles correctly.
#define main rs_main

#include "../../RecompiledFuncs/recomp_overlays.inl"

static recomp_func_t* const s_statically_patched[] = {
#define RECOMP_PATCHED_FUNC(f, kinds) f,
#include "../../RecompiledFuncs/patched_funcs.inl"
#undef RECOMP_PATCHED_FUNC
};
static const int s_statically_patched_kinds[] = {
#define RECOMP_PATCHED_FUNC(f, kinds) kinds,
#include "../../RecompiledFuncs/patched_funcs.inl"
#undef RECOMP_PATCHED_FUNC
};

#undef main

#include "librecomp/overlays.hpp"

// The 3 .ovl.* overlays (mission/menu/cinematic, all at 0x800A5130) are registered here at boot.
// The loadOverlay hook (0x80000B3C in rogue_squadron.toml) switches func_map to the live one at runtime via rs64_load_overlay in upstream_compat.cpp.
void rs64_register_overlays()
{

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
    recomp::overlays::register_statically_patched_funcs(s_statically_patched, s_statically_patched_kinds, ARRLEN(s_statically_patched));
    recomp::overlays::register_static_patch_replays(static_patch_hooks, static_patch_instructions);
}
