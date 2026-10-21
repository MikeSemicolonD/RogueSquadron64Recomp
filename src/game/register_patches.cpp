// Separate from register_overlays.cpp: both recomp_overlays.inl files define a static section_table.
#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "../../RecompiledPatches/recomp_overlays.inl"
#include "patches_bin.h"
#include "host_api/host_api.h"
#include <cstdio>

// Manual patch symbol (patches/syms.ld): prints the NUL-terminated string at a0, the output of recomp_printf (patches/src/print.c).
extern "C" void rs64_host_puts(uint8_t* rdram, recomp_context* ctx) {

    for (int32_t i = 0;; ++i) {
        const char c = static_cast<char>(MEM_B(i, ctx->r4));

        if (c == '\0') break;

        std::fputc(c, stdout);
    }

    std::fflush(stdout);
}

// Manual patch symbol: v0 = a0 is in RAM a mod registered with add_valid_ram (patches/src/npc_health.c).
extern "C" void rs64_host_ram_ok(uint8_t*, recomp_context* ctx) {
    ctx->r2 = rs64::host::ram_ok((uint32_t)ctx->r4) ? 1 : 0;
}

const char* const* rs64_register_patches()
{

    recomp::overlays::register_patches(rs64_patches_bin, sizeof(rs64_patches_bin), section_table, ARRLEN(section_table));
    recomp::overlays::register_base_exports(export_table);
    recomp::overlays::register_manual_patch_symbols(manual_patch_symbols);

    return event_names;
}
