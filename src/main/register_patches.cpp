// Separate from register_overlays.cpp: both recomp_overlays.inl files define a static section_table.
#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "../../RecompiledPatches/recomp_overlays.inl"
#include "patches_bin.h"

const char* const* rs64_register_patches() {
    recomp::overlays::register_patches(rs64_patches_bin, sizeof(rs64_patches_bin), section_table, ARRLEN(section_table));
    recomp::overlays::register_base_exports(export_table);
    recomp::overlays::register_manual_patch_symbols(manual_patch_symbols);
    return event_names;
}
