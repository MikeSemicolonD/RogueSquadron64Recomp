#pragma once
// Every fixed range the port uses in host RAM between the game's 8 MB and the mod area (0x81000000), end-exclusive and in address order. C and C++.
// Add a range here and to the chain at the bottom; a mod takes its memory from the host API's alloc instead.

// librecomp's cart PI handle, then the patches ELF data (recomp::patch_rdram_start).
#define RS64_HR_START 0x80800000u
#define RS64_HR_PATCH_DATA 0x80801000u
#define RS64_HR_PATCH_DATA_END 0x80810000u

// Scene texture pool, second region (language mod).
#define RS64_HR_TEX_POOL_B 0x80810000u
#define RS64_HR_TEX_POOL_B_END 0x80A00000u

// F5 renderer vertex and viewport scratch (F5_VTX_SCRATCH / F5_VP_SCRATCH in lib/rt64's rt64_gbi_f3dfactor5.cpp).
#define RS64_HR_F5_SCRATCH 0x80A00000u
#define RS64_HR_F5_SCRATCH_END 0x80A02000u

// Counters the probe mods under tests/mods write, at fixed addresses so a run can watch them.
#define RS64_HR_TEST_PROBES 0x80A02000u
#define RS64_HR_TEST_PROBES_END 0x80A03000u

// Terrain edit API tile table.
#define RS64_HR_TERRAIN_EDIT 0x80A10000u
#define RS64_HR_TERRAIN_EDIT_END 0x80A4C000u

// Far camera-space matrix translations for RT64.
#define RS64_HR_FAR_MTX 0x80A4C000u
#define RS64_HR_FAR_MTX_END 0x80A5C000u

// Draw-only stand-ins for unspawned static objects.
#define RS64_HR_STANDINS 0x80A5C000u
#define RS64_HR_STANDINS_END 0x80B00000u

// 128x128 terrain grid tables.
#define RS64_HR_TGRID 0x80B00000u
#define RS64_HR_TGRID_END 0x80B20000u

// tickFormatMessageWorker reply ring.
#define RS64_HR_REPLY_RING 0x80B20000u
#define RS64_HR_REPLY_RING_END 0x80B20600u

// Co-op mod: relocated per-player ranges (kMovedRanges in mp_core.h, delta 0x00A00000).
#define RS64_HR_COOP_MOVED_A 0x80B38058u
#define RS64_HR_COOP_MOVED_A_END 0x80B39020u

// Co-op mod: imposter DAT record and message scratch (kImposterRec in mp_core.h).
#define RS64_HR_COOP_SCRATCH 0x80B40000u
#define RS64_HR_COOP_SCRATCH_END 0x80B40160u

// Co-op mod: relocated per-view block (kMovedRanges, delta 0x00A20000).
#define RS64_HR_COOP_MOVED_B 0x80B58930u
#define RS64_HR_COOP_MOVED_B_END 0x80B58D10u

// Per-mesh face list (the lui 0x80B6 instruction patches in rogue_squadron.toml).
#define RS64_HR_FACE_LIST 0x80B60000u
#define RS64_HR_FACE_LIST_END 0x80B80000u

// Extra display list chunks when a mod replaces a player craft.
#define RS64_HR_DL_CHUNKS 0x80B80000u
#define RS64_HR_DL_CHUNKS_END 0x80C00000u

// Manifests grown with mod assets, 64 KB per gManifestTable slot.
#define RS64_HR_MANIFESTS 0x80C00000u
#define RS64_HR_MANIFESTS_END 0x80C40000u

// Mod level scratch: names and text the game reads by address.
#define RS64_HR_SHOT_NAME 0x80C40000u
#define RS64_HR_SHOT_NAME_END 0x80C40100u
#define RS64_HR_LOOKUP_NAME 0x80C40100u
#define RS64_HR_LOOKUP_NAME_END 0x80C40200u
#define RS64_HR_MOD_MEDALS 0x80C40200u
#define RS64_HR_MOD_MEDALS_END 0x80C40224u
#define RS64_HR_MEDAL_TEXT 0x80C40300u
#define RS64_HR_MEDAL_TEXT_END 0x80C40368u
#define RS64_HR_CRAWL_TEXT 0x80C41000u
#define RS64_HR_CRAWL_TEXT_END 0x80C42000u
#define RS64_HR_OBJECTIVE_TEXT 0x80C42000u
#define RS64_HR_OBJECTIVE_TEXT_END 0x80C42100u
#define RS64_HR_OBJECTIVE_CAMS 0x80C42100u
#define RS64_HR_OBJECTIVE_CAMS_END 0x80C42180u
#define RS64_HR_RULE_NAME 0x80C42200u
#define RS64_HR_RULE_NAME_END 0x80C42240u
#define RS64_HR_PACKAGE_NAME 0x80C42300u
#define RS64_HR_PACKAGE_NAME_END 0x80C42400u

// Pause menu GAME SETTINGS, mod-page and root arrays.
#define RS64_HR_PAUSE_MENU 0x80C43000u
#define RS64_HR_PAUSE_MENU_END 0x80C44000u

// Language mod: extended fonts, enlarged texture and material tables, encoded text.
#define RS64_HR_LANG 0x80C44000u
#define RS64_HR_LANG_END 0x80CE4000u

// Scene texture pool, first region (language mod), up to where mods load (recomp::mod_rdram_start).
#define RS64_HR_TEX_POOL_A 0x80CE4000u
#define RS64_HR_TEX_POOL_A_END 0x81000000u

#ifdef __cplusplus

#define RS64_HR_ORDERED(name, next) static_assert(RS64_HR_##name < RS64_HR_##name##_END && RS64_HR_##name##_END <= RS64_HR_##next, "host RAM ranges overlap: " #name " / " #next)

static_assert(RS64_HR_START < RS64_HR_PATCH_DATA, "host RAM map starts below the game's 8 MB");
RS64_HR_ORDERED(PATCH_DATA, TEX_POOL_B);
RS64_HR_ORDERED(TEX_POOL_B, F5_SCRATCH);
RS64_HR_ORDERED(F5_SCRATCH, TEST_PROBES);
RS64_HR_ORDERED(TEST_PROBES, TERRAIN_EDIT);
RS64_HR_ORDERED(TERRAIN_EDIT, FAR_MTX);
RS64_HR_ORDERED(FAR_MTX, STANDINS);
RS64_HR_ORDERED(STANDINS, TGRID);
RS64_HR_ORDERED(TGRID, REPLY_RING);
RS64_HR_ORDERED(REPLY_RING, COOP_MOVED_A);
RS64_HR_ORDERED(COOP_MOVED_A, COOP_SCRATCH);
RS64_HR_ORDERED(COOP_SCRATCH, COOP_MOVED_B);
RS64_HR_ORDERED(COOP_MOVED_B, FACE_LIST);
RS64_HR_ORDERED(FACE_LIST, DL_CHUNKS);
RS64_HR_ORDERED(DL_CHUNKS, MANIFESTS);
RS64_HR_ORDERED(MANIFESTS, SHOT_NAME);
RS64_HR_ORDERED(SHOT_NAME, LOOKUP_NAME);
RS64_HR_ORDERED(LOOKUP_NAME, MOD_MEDALS);
RS64_HR_ORDERED(MOD_MEDALS, MEDAL_TEXT);
RS64_HR_ORDERED(MEDAL_TEXT, CRAWL_TEXT);
RS64_HR_ORDERED(CRAWL_TEXT, OBJECTIVE_TEXT);
RS64_HR_ORDERED(OBJECTIVE_TEXT, OBJECTIVE_CAMS);
RS64_HR_ORDERED(OBJECTIVE_CAMS, RULE_NAME);
RS64_HR_ORDERED(RULE_NAME, PACKAGE_NAME);
RS64_HR_ORDERED(PACKAGE_NAME, PAUSE_MENU);
RS64_HR_ORDERED(PAUSE_MENU, LANG);
RS64_HR_ORDERED(LANG, TEX_POOL_A);
static_assert(RS64_HR_TEX_POOL_A < RS64_HR_TEX_POOL_A_END, "host RAM map ends below its last range");

#undef RS64_HR_ORDERED

#endif
