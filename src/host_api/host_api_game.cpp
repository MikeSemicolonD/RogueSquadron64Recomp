#include "host_api/host_api.h"
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "game/game_state.h"
#include "lockstep/lockstep_core.h"
#include "util/mips_call.h"
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif
#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

extern "C" void rs64_register_builtin_hooks();
extern "C" void rs64_camera_shots_register_service();
extern "C" void rs64_register_builtin_late_hooks();
extern "C" void osYieldThread_recomp(uint8_t*, recomp_context*);
extern "C" void rs64_menu_request_quit(void);
extern "C" float rs64_draw_dist(void);
extern "C" float rs64_terrain_dist(void);
extern "C" uint32_t rs64_tgrid_base(uint32_t original);

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

int svc_screen_keyboard_shown()
{

    if (!SDL_HasScreenKeyboardSupport())
    {
        return -1;
    }

    return SDL_IsScreenKeyboardShown(SDL_GetKeyboardFocus()) == SDL_TRUE ? 1 : 0;
}

int svc_any_key_held() {

    int n = 0;
    const Uint8* keys = SDL_GetKeyboardState(&n);

    for (int k = 0; keys && k < n; ++k) {
        if (keys[k])
        {
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

uint32_t svc_call(uint8_t* rdram, void* c, uint32_t vram, const uint32_t* a, uint32_t n, const uint32_t* st, uint32_t ns, float f12, float f14, float* f0)
{

    if (!c) return 0u;
    if (n > 4) fprintf(stderr, "[host] call: only the first 4 args are passed (%u given)\n", n);

    if (ns > 12)
    {
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

    for (const auto& m : recomp::mods::get_all_mod_details("rs64"))
    {
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
    rs64_camera_shots_register_service();
    init_mods(rdram);
    rs64::ls::set_moved_ranges(rs64::host::moved_ranges());
    rs64_register_builtin_late_hooks();
    rs64::host::seal();
}
}

// .nrm mods subscribe to a toml hook site with RECOMP_CALLBACK("*", rs64_on_<hook>). librecomp restores ctx after each callback, so a callback reaches registers only through rs64_event_return and the rs64_event_set_* writes, applied once every callback has run.
namespace {
size_t s_event_index[RS64_HOOK_COUNT];
bool s_events_registered = false;
bool s_event_return = false;
uint32_t s_event_r2 = 0;

struct RegWrite {
    bool fpr;
    uint32_t reg;
    uint32_t value;
};

struct RegWrites {
    RegWrite writes[8];
    int count = 0;
};

RegWrites s_reg_writes;

void queue_reg_write(bool fpr, uint32_t reg, uint32_t value) {

    if (s_reg_writes.count < 8)
    {
        s_reg_writes.writes[s_reg_writes.count++] = RegWrite{ fpr, reg, value };
    }
}

// rs64_event_set_gpr(reg, value): any GPR but r0 and sp (moving sp would break the game function's frame).
void rs64_event_set_gpr(uint8_t*, recomp_context* ctx)
{

    const uint32_t reg = (uint32_t)ctx->r4;

    if (reg != 0 && reg != 29 && reg < 32) queue_reg_write(false, reg, (uint32_t)ctx->r5);
}

// rs64_event_set_fpr(reg, value): a single-precision float register. o32 passes a float that follows an integer argument in a1, as its bits.
void rs64_event_set_fpr(uint8_t*, recomp_context* ctx) {

    const uint32_t reg = (uint32_t)ctx->r4;

    if (reg < 32) queue_reg_write(true, reg, (uint32_t)ctx->r5);
}

// The hook site's registers while its callbacks run (librecomp hands each callback a copy it then overwrites), for rs64_event_get_*.
const recomp_context* s_event_site = nullptr;

// Odd float registers in FR=0 mode are the high half of the even register below, as the recompiled code reads them.
uint32_t read_fpr_bits(const recomp_context* ctx, uint32_t reg)
{

    if ((reg & 1) == 0 || ctx->mips3_float_mode) return (&ctx->f0)[reg].u32l;

    return (&ctx->f0)[reg - 1].u32h;
}

// rs64_event_get_gpr(reg) / rs64_event_get_fpr(reg): a register as it was at the hook site; 0 outside a callback.
void rs64_event_get_gpr(uint8_t*, recomp_context* ctx)
{

    const uint32_t reg = (uint32_t)ctx->r4;
    ctx->r2 = (s_event_site != nullptr && reg < 32) ? (int32_t)(&s_event_site->r0)[reg] : 0;
}

void rs64_event_get_fpr(uint8_t*, recomp_context* ctx)
{

    const uint32_t reg = (uint32_t)ctx->r4;
    ctx->f0.u32l = (s_event_site != nullptr && reg < 32) ? read_fpr_bits(s_event_site, reg) : 0;
}

void apply_reg_writes(recomp_context* ctx, const RegWrites& pending) {

    for (int i = 0; i < pending.count; ++i) {
        const RegWrite& w = pending.writes[i];

        if (!w.fpr) {
            (&ctx->r0)[w.reg] = (int32_t)w.value;
        } else if ((w.reg & 1) == 0 || ctx->mips3_float_mode) {
            (&ctx->f0)[w.reg].u32l = w.value;
        } else {
            (&ctx->f0)[w.reg - 1].u32h = w.value;
        }
    }
}

// Only function-entry sites may return: a mid-function return skips the epilogue and leaves sp and the saved registers wrong for the caller.
bool hook_site_is_entry(uint32_t hook) {

    switch (hook) {
    case RS64_HOOK_MENU_FRAME:
    case RS64_HOOK_MISSION_END:
    case RS64_HOOK_TRANSITION_REQUEST:
    case RS64_HOOK_OBJECTIVE_COUNT:
    case RS64_HOOK_RESULT_FAIL:
    case RS64_HOOK_RESULT_SUCCESS:
    case RS64_HOOK_TRIGGER_EFFECT:
        return true;
    default:
        return false;
    }
}

std::string event_name(uint32_t hook)
{

    std::string name = rs64::host::hook_name(hook) + std::strlen("RS64_HOOK_");

    for (char& c : name) c = (char)std::tolower((unsigned char)c);

    return "rs64_on_" + name;
}

void rs64_event_return(uint8_t*, recomp_context* ctx) {

    s_event_r2 = (uint32_t)ctx->r4;
    s_event_return = true;
}

// Base exports for .nrm mods (RECOMP_IMPORT("*", ...)): MIPS calling convention, args in a0, results in v0 / f0.
void rs64_export_draw_dist(uint8_t*, recomp_context* ctx) { ctx->f0.fl = rs64_draw_dist(); }
void rs64_export_terrain_dist(uint8_t*, recomp_context* ctx) { ctx->f0.fl = rs64_terrain_dist(); }
void rs64_export_tgrid_base(uint8_t*, recomp_context* ctx) { ctx->r2 = (int32_t)rs64_tgrid_base((uint32_t)ctx->r4); }
void rs64_export_relocated(uint8_t*, recomp_context* ctx) { ctx->r2 = (int32_t)rs64::ls::relocated((uint32_t)ctx->r4); }

int trigger_hook_event(uint32_t hook, uint8_t* rdram, recomp_context* ctx)
{

    if (!recomp::mods::event_has_callbacks(s_event_index[hook])) return RS64_HOOK_CONTINUE;

    const bool outer_return = s_event_return;
    const uint32_t outer_r2 = s_event_r2;
    const RegWrites outer_writes = s_reg_writes;
    const recomp_context* outer_site = s_event_site;
    const recomp_context site = *ctx;

    s_event_site = &site;
    s_event_return = false;
    s_reg_writes.count = 0;

    recomp_trigger_event(rdram, ctx, (uint32_t)s_event_index[hook]);

    s_event_site = outer_site;

    const bool requested = s_event_return && hook_site_is_entry(hook);
    const uint32_t r2 = s_event_r2;
    const RegWrites writes = s_reg_writes;

    s_event_return = outer_return;
    s_event_r2 = outer_r2;
    s_reg_writes = outer_writes;

    apply_reg_writes(ctx, writes);

    if (!requested) return RS64_HOOK_CONTINUE;

    ctx->r2 = (int32_t)r2;

    return RS64_HOOK_RETURN;
}

}

// Must run before librecomp loads mods, or their callbacks fail to resolve. librecomp numbers base events per call, so the patches ELF's events come in here too.
void rs64_register_mod_events(const char* const* patch_events)
{

    static std::vector<std::string> names;
    static std::vector<const char*> list;

    for (size_t i = 0; patch_events && patch_events[i]; ++i) list.push_back(patch_events[i]);

    for (uint32_t h = 1; h < RS64_HOOK_COUNT; ++h)
    {
        if (rs64::host::hook_has_toml_site(h)) names.push_back(event_name(h));
    }

    for (const std::string& n : names) list.push_back(n.c_str());

    list.push_back(nullptr);
    recomp::overlays::register_base_events(list.data());

    for (uint32_t h = 0; h < RS64_HOOK_COUNT; ++h)
    {
        s_event_index[h] = rs64::host::hook_has_toml_site(h) ? recomp::overlays::get_base_event_index(event_name(h)) : (size_t)-1;
    }

    recomp::overlays::register_base_export("rs64_event_return", rs64_event_return);
    recomp::overlays::register_base_export("rs64_event_set_gpr", rs64_event_set_gpr);
    recomp::overlays::register_base_export("rs64_event_set_fpr", rs64_event_set_fpr);
    recomp::overlays::register_base_export("rs64_event_get_gpr", rs64_event_get_gpr);
    recomp::overlays::register_base_export("rs64_event_get_fpr", rs64_event_get_fpr);
    recomp::overlays::register_base_export("rs64_draw_dist", rs64_export_draw_dist);
    recomp::overlays::register_base_export("rs64_terrain_dist", rs64_export_terrain_dist);
    recomp::overlays::register_base_export("rs64_tgrid_base", rs64_export_tgrid_base);
    recomp::overlays::register_base_export("rs64_relocated", rs64_export_relocated);
    s_events_registered = true;
}

// Built-ins then native mods register on the first hook call (thread-safe static init), then the registry is sealed so dispatch reads it without locks. Native handlers run first; a RETURN from one skips the mod events.
extern "C" int rs64_hook(uint32_t hook, uint8_t* rdram, recomp_context* ctx)
{

    static const bool s_ready = [rdram] {

        init_once(rdram);

        return true;
    }();

    (void)s_ready;
    const int r = rs64::host::dispatch(hook, rdram, ctx);

    if (r == RS64_HOOK_RETURN || !ctx || !s_events_registered || hook >= RS64_HOOK_COUNT || s_event_index[hook] == (size_t)-1) return r;

    return trigger_hook_event(hook, rdram, ctx);
}
