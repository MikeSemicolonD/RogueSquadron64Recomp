// Shared header for .nrm code mods (MIPS C built with tools/mods/code_mod.cmake). Section names are the ones RecompModTool reads.
#ifndef RS64_API_H
#define RS64_API_H

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))
#define RECOMP_FORCE_PATCH __attribute__((section(".recomp_force_patch")))
#define RECOMP_EXPORT __attribute__((section(".recomp_export")))
#define RECOMP_HOOK(func) __attribute__((section(".recomp_hook." func)))
#define RECOMP_HOOK_RETURN(func) __attribute__((section(".recomp_hook_return." func)))

// Runs on an event: RECOMP_CALLBACK("*", rs64_on_mission_frame_pads) for the game's hook sites ("*" is the base game), or another mod's id for its RECOMP_DECLARE_EVENT events.
#define RECOMP_CALLBACK(mod, event) __attribute__((section(".recomp_callback." mod ":" #event)))

// Declares a function from another mod ("*" = the base game); the empty weak body only gives RecompModTool a symbol to resolve.
#define RECOMP_IMPORT(mod, func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    _Pragma("GCC diagnostic ignored \"-Wreturn-type\"") \
    __attribute__((noinline, weak, used, section(".recomp_import." mod))) func {} \
    _Pragma("GCC diagnostic pop")

#define RECOMP_DECLARE_EVENT(func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    __attribute__((noinline, weak, used, section(".recomp_event"))) void func {} \
    _Pragma("GCC diagnostic pop")

// From an rs64_on_* callback at a function-entry site (menu_frame, mission_end, transition_request, objective_count, result_fail, result_success, trigger_effect): the game function returns at once with v0 = r2. Ignored elsewhere.
RECOMP_IMPORT("*", void rs64_event_return(unsigned int r2))

// The player's draw-distance multiplier (1.0 = the game's own) and the terrain reach / fog multiplier that follows it (capped at 2.5).
RECOMP_IMPORT("*", float rs64_draw_dist(void))
RECOMP_IMPORT("*", float rs64_terrain_dist(void))

// The terrain grid tables moved into host RAM with doubled strides: pass a table's original address, get where it lives now (others pass through).
RECOMP_IMPORT("*", unsigned int rs64_tgrid_base(unsigned int original))

// Where a per-player field moved for co-op (the R1-R4 ranges); addresses outside them pass through.
RECOMP_IMPORT("*", unsigned int rs64_relocated(unsigned int address))

#endif
