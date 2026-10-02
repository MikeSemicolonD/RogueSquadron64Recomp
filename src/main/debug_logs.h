// Centralized debug-log gates for the Rogue Squadron recompile.
//
// The project accumulated many fprintf/printf trace points during boot
// debugging — thread setup, GFX dispatch, framebuffer routing, RDP
// commands. Most of them are silent on a healthy run but still emit
// hundreds of lines per second when active, drowning out the few
// messages you actually want to see.
//
// Each category below is read once on first call (cached in a function-
// local static), so check overhead is a single load per call site after
// startup. Set the env var to "1" (or any non-zero value) to enable.
// Errors and crashes (SIGABRT/SEH/etc.) are always printed unconditionally;
// don't gate those.
//
// Usage:
//   if (recomp::dbg::log_vi()) {
//       fprintf(stderr, "[osViSwapBuffer #%d] fb=0x%08X\n", n, fb);
//   }

#pragma once

#include <cstdlib>
#include <cstring>
#include "os_compat.h"

namespace recomp::dbg {

// True iff the variable is set to a value other than 0, "false" or "no".
inline bool env_flag(const char *name) {
    const char *e = recomp::os::getenv(name);
    if (!e || !*e) return false;
    if (*e == '0') return false;
    if (std::strcmp(e, "false") == 0) return false;
    if (std::strcmp(e, "no") == 0) return false;
    return true;
}

// libultra VI shims: osViSetMode / osViSwapBuffer / osViSetXScale / osViSetYScale / osViBlack.
// ROGUESQ_LOG_VI=1.
inline bool log_vi() {
    static const bool v = env_flag("ROGUESQ_LOG_VI");
    return v;
}

// Thread lifecycle shims: osDestroyThread victim + queue check. ROGUESQ_LOG_THREADS=1.
inline bool log_threads() {
    static const bool v = env_flag("ROGUESQ_LOG_THREADS");
    return v;
}

// Rumble: game motor edges, effect ids, hit scaling, death-spiral sustain. ROGUESQ_LOG_RUMBLE=1.
inline bool log_rumble() {
    static const bool v = env_flag("ROGUESQ_LOG_RUMBLE");
    return v;
}

// Positional throttle: lever position, the craft's speed range, game vs. applied target. ROGUESQ_LOG_THROTTLE=1.
inline bool log_throttle() {
    static const bool v = env_flag("ROGUESQ_LOG_THROTTLE");
    return v;
}

// Behaviour knobs, read at the call site. env_on treats any value not starting with '0' as on.
inline const char* env_str(const char* name) { const char* v = recomp::os::getenv(name); return (v && *v) ? v : nullptr; }
inline bool env_on(const char* name, bool def = false) { const char* v = env_str(name); return v ? (*v != '0') : def; }
inline int env_int(const char* name, int def = 0) { const char* v = env_str(name); return v ? std::atoi(v) : def; }
inline unsigned env_u32(const char* name) { const char* v = env_str(name); return v ? (unsigned)std::strtoul(v, nullptr, 0) : 0u; }

} // namespace recomp::dbg
