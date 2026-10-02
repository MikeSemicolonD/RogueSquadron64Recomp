#include "host_api.h"
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/mods.hpp"
#include "game_state.h"
#include "mips_call.h"
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif
#include <cstdio>
#include <initializer_list>
#include <string>

extern "C" void rs64_register_builtin_hooks();
extern "C" void osYieldThread_recomp(uint8_t*, recomp_context*);
extern "C" void rs64_menu_request_quit(void);

namespace {
void svc_yield(uint8_t* rdram, void* c) {
    if (c) {
        osYieldThread_recomp(rdram, (recomp_context*)c);
    }
}

void svc_text_input(int on) {
    if (on) {
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
}

int svc_screen_keyboard_shown() {
    if (!SDL_HasScreenKeyboardSupport()) {
        return -1;
    }
    return SDL_IsScreenKeyboardShown(SDL_GetKeyboardFocus()) == SDL_TRUE ? 1 : 0;
}

int svc_any_key_held() {
    int n = 0;
    const Uint8* keys = SDL_GetKeyboardState(&n);
    for (int k = 0; keys && k < n; ++k) {
        if (keys[k]) {
            return 1;
        }
    }
    return 0;
}

const char* svc_base_path() {
    static const std::string s = [] {
        std::string d;
        if (char* b = SDL_GetBasePath()) {
            d = b;
            SDL_free(b);
        }
        return d;
    }();
    return s.c_str();
}

uint32_t svc_call(uint8_t* rdram, void* c, uint32_t vram, const uint32_t* a, uint32_t n, const uint32_t* st, uint32_t ns, float f12, float f14, float* f0) {
    if (!c) return 0u;
    if (n > 4) fprintf(stderr, "[host] call: only the first 4 args are passed (%u given)\n", n);
    if (ns > 12) {
        fprintf(stderr, "[host] call: only the first 12 stack words are passed (%u given)\n", ns);
        ns = 12;
    }
    auto* ctx = (recomp_context*)c;
    const uint32_t v[4] = {n > 0 && a ? a[0] : 0u, n > 1 && a ? a[1] : 0u, n > 2 && a ? a[2] : 0u, n > 3 && a ? a[3] : 0u};
    const rs64::mips::Call r = rs64::mips::mips_call_n(rdram, ctx, vram, v, 4, st, st ? ns : 0u, f12, f14);
    if (f0) *f0 = r.f0;
    return r.v0;
}

uint32_t svc_alloc(uint8_t* rdram, uint32_t n) {
    void* p = recomp::alloc(rdram, n);
    return p ? (uint32_t)((uint8_t*)p - rdram) + 0x80000000u : 0u;
}

void svc_log(const char* s) {
    fprintf(stderr, "%s\n", s);
    fflush(stderr);
}

// A mod whose rs64_mod_init refuses has its registrations rolled back; mods init in load order, after the built-ins.
void init_mods(uint8_t* rdram) {
    for (const auto& m : recomp::mods::get_all_mod_details("rs64")) {
        if (!recomp::mods::is_mod_enabled(m.mod_id)) continue;
        recomp_func_t* fn = recomp::mods::get_mod_export(m.mod_id, RS64_MOD_INIT_EXPORT);
        if (!fn) continue;
        recomp_context ctx{};
        ctx.r4 = (gpr)(uintptr_t)rs64::host::api();
        ctx.r5 = (gpr)rs64::host::init_version();
        const rs64::host::Mark before = rs64::host::mark();
        fn(rdram, &ctx);
        const uint32_t built = (uint32_t)ctx.r3;
        const bool ok = rs64::host::accept_mod_init((uint32_t)ctx.r2, built);
        if (!ok) rs64::host::rollback(before);
        fprintf(stderr, "[host] mod %s %s the host API (host v%u, mod built for v%u, r2=%u)%s\n", m.mod_id.c_str(), ok ? "took" : "skipped", rs64::host::init_version(), built == 0 ? 1u : built, (uint32_t)ctx.r2, ok ? "" : ", its registrations were dropped");
        fflush(stderr);
    }
}

void init_once(uint8_t* rdram) {
    rs64::host::Services s;
    s.log = svc_log;
    s.call = svc_call;
    s.alloc = svc_alloc;
    s.state_id = rs64_state_current_id;
    s.in_cutscene = rs64_state_in_cinematic;
    s.cutscene_skips = rs64_state_cutscene_skips;
    s.yield = svc_yield;
    s.request_quit = rs64_menu_request_quit;
    s.text_input = svc_text_input;
    s.screen_keyboard_shown = svc_screen_keyboard_shown;
    s.any_key_held = svc_any_key_held;
    s.base_path = svc_base_path;
    rs64::host::bind_services(s);
    rs64_register_builtin_hooks();
    init_mods(rdram);
    rs64::host::seal();
}
}

// Built-ins then native mods register on the first hook call (thread-safe static init), then the registry is sealed so dispatch reads it without locks.
extern "C" int rs64_hook(uint32_t hook, uint8_t* rdram, recomp_context* ctx) {
    static const bool s_ready = [rdram] {
        init_once(rdram);
        return true;
    }();
    (void)s_ready;
    return rs64::host::dispatch(hook, rdram, ctx);
}
