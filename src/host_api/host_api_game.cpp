#include "host_api/host_api.h"
#include "recomp.h"
#include "rs64_toml_hooks.h"
#include "librecomp/addresses.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "game/game_state.h"
#include "game/mod_content/terrain_edit.h"
#include "lockstep/lockstep_core.h"
#include "util/mips_call.h"
#include "util/host_ram_map.h"
#include "hle/rt64_rs64_lights.h"
#include "json/json.hpp"
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern "C" void rs64_register_builtin_hooks();
extern "C" void rs64_camera_shots_register_service();
extern "C" void rs64_register_builtin_late_hooks();
extern "C" void osYieldThread_recomp(uint8_t*, recomp_context*);
extern "C" void rs64_menu_request_quit(void);
extern "C" uint32_t rs64_rules_object(uint8_t*, void*, const char*);
extern "C" float rs64_rules_object_health(uint8_t*, void*, const char*);
extern "C" int rs64_rules_destroy_object(uint8_t*, void*, const char*);
extern "C" int rs64_rules_run_line(uint8_t*, void*, const char*);
extern "C" int32_t rs64_rules_var_get(const char*);
extern "C" void rs64_rules_var_set(const char*, int32_t);
extern "C" float rs64_rules_mission_seconds(void);
extern "C" int rs64_rules_spawn(uint8_t*, void*, const char*, const char*, const float*);
extern "C" void rs64_set_mod_camera_view(float, float, float, float, float);
extern "C" int rs64_hook(uint32_t hook, uint8_t* rdram, recomp_context* ctx);

namespace {
// Set once the built-ins and mods are registered; the v9 host-dispatched hooks never start that registration themselves.
std::atomic<bool> s_host_ready{false};

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

// The browser's clipboard belongs to the page's main thread; SDL's web clipboard is only its own copy.
void svc_set_clipboard(const char* utf8) {

#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({ const s = UTF8ToString($0); Module._free($0); navigator.clipboard.writeText(s).catch(() => {}); }, strdup(utf8));
#else
    SDL_SetClipboardText(utf8);
#endif
}

const char* svc_base_path() {

    static const std::string s = [] {

        std::string d;

#ifdef __EMSCRIPTEN__

        // The browser build's folder is the page's stored files (SDL's base path, "/", is lost on reload).
        return std::string("/persist/");

#endif

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

int svc_config_number(const char* mod, const char* option, double* out)
{

    const auto v = recomp::mods::get_mod_config_value(mod, option);

    if (const auto* u = std::get_if<uint32_t>(&v))
    {
        *out = (double)*u;

        return 1;
    }

    if (const auto* d = std::get_if<double>(&v))
    {
        *out = *d;

        return 1;
    }

    return 0;
}

uint8_t* s_rdram = nullptr;
std::atomic<bool> s_pilot_chosen{false};

// A mod's runtime light preset (RT64 applies it at its next frame); the JSON is checked here so a bad one is refused rather than logged every change.
int svc_light_set(const char* name, const char* preset)
{

    if (!preset || !*preset)
    {
        rs64lights::setRuntimePreset(name, {});

        return 0;
    }

    const nlohmann::json j = nlohmann::json::parse(preset, nullptr, false);

    if (!j.is_object() || !j.contains("lights") || !j["lights"].is_array()) return -2;

    rs64lights::setRuntimePreset(name, preset);

    return 0;
}

int svc_key_down(const char* key)
{

    const SDL_Scancode sc = SDL_GetScancodeFromName(key);
    int n = 0;
    const Uint8* keys = SDL_GetKeyboardState(&n);

    return sc != SDL_SCANCODE_UNKNOWN && keys && (int)sc < n && keys[sc] ? 1 : 0;
}

// mod_data/<mod_id>/ beside the game's settings; ids that could leave that folder get "".
const char* svc_mod_data_dir(const char* mod)
{

    static std::mutex mu;
    static std::map<std::string, std::string> dirs;
    const std::string id = mod;

    if (id.empty() || id.find_first_of("/\\:") != std::string::npos || id == "." || id == "..") return "";

    std::lock_guard<std::mutex> lk(mu);
    auto it = dirs.find(id);

    if (it != dirs.end()) return it->second.c_str();

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(svc_base_path()) / "mod_data" / id;

    std::filesystem::create_directories(dir, ec);

    const std::string out = ec ? std::string() : (dir.string() + (char)std::filesystem::path::preferred_separator);

    return dirs.emplace(id, out).first->second.c_str();
}

// Pilot slot byte 0x80130B44, once a pilot was confirmed (RS64_HOOK_MENU_PILOT_CHOSEN).
int svc_save_slot()
{

    uint8_t* rdram = s_rdram;

    return s_pilot_chosen.load() && rdram ? (int)(int8_t)MEM_B(0, (gpr)(int32_t)0x80130B44u) : -1;
}

int pilot_chosen(uint32_t, uint8_t*, void*, void*)
{

    s_pilot_chosen.store(true);

    return RS64_HOOK_CONTINUE;
}

// Valid until the next call (on any thread), as the table documents.
const char* svc_config_string(const char* mod, const char* option)
{

    static thread_local std::string s;
    const auto v = recomp::mods::get_mod_config_value(mod, option);
    const auto* str = std::get_if<std::string>(&v);

    s = str ? *str : std::string();

    return s.c_str();
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
    s.set_clipboard = svc_set_clipboard;
    s.frame_seconds = [] { return rs64_rate_scale() / 30.0f; };
    s.relocated = [](uint32_t a) { return rs64::ls::relocated(a); };
    s.object_by_name = rs64_rules_object;
    s.object_health = rs64_rules_object_health;
    s.destroy_object = rs64_rules_destroy_object;
    s.run_rule_line = rs64_rules_run_line;
    s.rule_var_get = rs64_rules_var_get;
    s.rule_var_set = rs64_rules_var_set;
    s.mission_seconds = rs64_rules_mission_seconds;
    s.config_number = svc_config_number;
    s.config_string = svc_config_string;
    s.key_down = svc_key_down;
    s.mod_data_dir = svc_mod_data_dir;
    s.save_slot = svc_save_slot;
    s.light_set = svc_light_set;
    s.spawn_unit = rs64_rules_spawn;
    s.camera_view = rs64_set_mod_camera_view;
    s_rdram = rdram;
    rs64::host::bind_services(s);
    rs64_register_builtin_hooks();
    rs64::host::add_hook(RS64_HOOK_MENU_PILOT_CHOSEN, pilot_chosen, nullptr);
    rs64_camera_shots_register_service();
    init_mods(rdram);

#ifdef __EMSCRIPTEN__

    // The browser build has the co-op relocation recompiled in, so its ranges hold even when the mod is off.
    if (rs64::host::moved_ranges().empty()) {
        static constexpr rs64::ls::MovedRange kBaked[] = {
#include "moved_ranges.inl"
        };

        for (const rs64::ls::MovedRange& r : kBaked) rs64::host::add_moved_range(r.lo, r.hi, r.delta);
    }

#endif

    rs64::ls::set_moved_ranges(rs64::host::moved_ranges());

    // The renderer parses an RDRAM snapshot that holds only the game's 8 MB unless told otherwise; moved data must be in it too.
    for (const rs64::ls::MovedRange& r : rs64::host::moved_ranges()) {
        rs64_snapshot_add_range(r.lo + r.delta - 0x80000000u, r.hi + r.delta - 0x80000000u);
    }

    // The tile table terrain_set_cell moves the map's tiles to.
    rs64_snapshot_add_range(rs64::terrain::kTable - 0x80000000u, rs64::terrain::kTableEnd - 0x80000000u);

    // Spawned units' DAT records: game records the NPC health guard must accept, and the renderer may read.
    rs64::host::add_valid_ram(RS64_HR_SPAWNED, RS64_HR_SPAWNED_END - RS64_HR_SPAWNED);
    rs64_snapshot_add_range(RS64_HR_SPAWNED - 0x80000000u, RS64_HR_SPAWNED_END - 0x80000000u);

    rs64_register_builtin_late_hooks();
    rs64::host::seal();
    s_host_ready.store(true);
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
    case RS64_HOOK_ATST_TICK:
    case RS64_HOOK_SONG_START:
    case RS64_HOOK_SOUND_3D:
    case RS64_HOOK_PLAYER_DAMAGE:
    case RS64_HOOK_PLAYER_FIRE:
    case RS64_HOOK_NPC_ATST_TICK:
    case RS64_HOOK_ATST_FIRE:
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

// A pointer into librecomp's RDRAM (the game's 8 MB or a mod's recomp_alloc block), with room for a read of len bytes.
bool guest_ptr(uint32_t addr, uint32_t len = 256u)
{
    return addr >= 0x80000000u && addr - 0x80000000u + len <= recomp::mem_size;
}

// A NUL-terminated string a .nrm mod passed (a KSEG0 pointer), at most max bytes.
std::string guest_str(uint8_t* rdram, uint32_t addr, uint32_t max = 255u)
{

    std::string s;

    if (!guest_ptr(addr, max + 1)) return s;

    for (uint32_t i = 0; i < max; ++i)
    {
        const char c = (char)MEM_B(i, (gpr)(int32_t)addr);

        if (!c) break;

        s += c;
    }

    return s;
}

void put_guest_str(uint8_t* rdram, uint32_t addr, uint32_t size, const char* s)
{

    if (!guest_ptr(addr) || size == 0) return;

    uint32_t i = 0;

    for (; s[i] && i + 1 < size; ++i) MEM_B(i, (gpr)(int32_t)addr) = (int8_t)s[i];

    MEM_B(i, (gpr)(int32_t)addr) = 0;
}

std::string arg_str(uint8_t* rdram, recomp_context* ctx, int reg)
{
    return guest_str(rdram, (uint32_t)(&ctx->r0)[reg]);
}

// The game-state, mission and level-rules exports (the v9 table's functions for .nrm mods). They run on the mod's guest stack, so game calls use its ctx.
void ex_state_id(uint8_t* rdram, recomp_context* ctx) { put_guest_str(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5, rs64_state_current_id()); }
void ex_in_cutscene(uint8_t*, recomp_context* ctx) { ctx->r2 = rs64_state_in_cinematic(); }
void ex_frame_seconds(uint8_t*, recomp_context* ctx) { ctx->f0.fl = rs64::host::api()->frame_seconds(); }
void ex_mission_seconds(uint8_t*, recomp_context* ctx) { ctx->f0.fl = rs64_rules_mission_seconds(); }
void ex_get_flag(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = rs64::host::flag(arg_str(rdram, ctx, 4).c_str()); }
void ex_set_flag(uint8_t* rdram, recomp_context* ctx) { rs64::host::set_flag(arg_str(rdram, ctx, 4).c_str(), (int32_t)ctx->r5); }
void ex_fire_rule_event(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = rs64::host::fire_rule_event(arg_str(rdram, ctx, 4).c_str(), arg_str(rdram, ctx, 5).c_str()); }
void ex_run_rule_line(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = rs64_rules_run_line(rdram, ctx, arg_str(rdram, ctx, 4).c_str()); }
void ex_rule_var_get(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = rs64_rules_var_get(arg_str(rdram, ctx, 4).c_str()); }
void ex_rule_var_set(uint8_t* rdram, recomp_context* ctx) { rs64_rules_var_set(arg_str(rdram, ctx, 4).c_str(), (int32_t)ctx->r5); }
void ex_object_by_name(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = (int32_t)rs64_rules_object(rdram, ctx, arg_str(rdram, ctx, 4).c_str()); }
void ex_object_health(uint8_t* rdram, recomp_context* ctx) { ctx->f0.fl = rs64_rules_object_health(rdram, ctx, arg_str(rdram, ctx, 4).c_str()); }
void ex_destroy_object(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = rs64_rules_destroy_object(rdram, ctx, arg_str(rdram, ctx, 4).c_str()); }
void ex_key_down(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = svc_key_down(arg_str(rdram, ctx, 4).c_str()); }
void ex_save_slot(uint8_t*, recomp_context* ctx) { ctx->r2 = svc_save_slot(); }

// rs64_spawn_unit(template_name, name, const float pos[3]): name may be NULL.
void ex_spawn_unit(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t at = (uint32_t)ctx->r6;
    const std::string tmpl = arg_str(rdram, ctx, 4);
    const std::string name = arg_str(rdram, ctx, 5);

    if (tmpl.empty() || !guest_ptr(at, 12))
    {
        ctx->r2 = -1;

        return;
    }

    float pos[3];

    for (int i = 0; i < 3; ++i)
    {
        const uint32_t bits = (uint32_t)MEM_W(4 * i, (gpr)(int32_t)at);

        std::memcpy(&pos[i], &bits, 4);
    }

    ctx->r2 = rs64_rules_spawn(rdram, ctx, tmpl.c_str(), name.empty() ? nullptr : name.c_str(), pos);
}

// rs64_camera_view(const float view[5]): yaw, pitch, then the offset x, y, z; NULL resets the view.
void ex_camera_view(uint8_t* rdram, recomp_context* ctx)
{

    const uint32_t at = (uint32_t)ctx->r4;
    float v[5] = {};

    for (int i = 0; guest_ptr(at, 20) && i < 5; ++i)
    {
        const uint32_t bits = (uint32_t)MEM_W(4 * i, (gpr)(int32_t)at);

        std::memcpy(&v[i], &bits, 4);
    }

    rs64_set_mod_camera_view(v[0], v[1], v[2], v[3], v[4]);
}

// rs64_light_set(name, preset_json): the JSON may run to 16 KB; a null pointer removes the preset.
void ex_light_set(uint8_t* rdram, recomp_context* ctx)
{

    const std::string name = arg_str(rdram, ctx, 4);
    const std::string preset = guest_str(rdram, (uint32_t)ctx->r5, 16383u);

    ctx->r2 = name.empty() ? -1 : svc_light_set(name.c_str(), preset.c_str());
}

// rs64_terrain_get_cell(col, row, signed char heights[25], unsigned short* tex) / rs64_terrain_set_cell(col, row, const signed char heights[25], int tex).
void ex_terrain_get_cell(uint8_t* rdram, recomp_context* ctx)
{

    int8_t h[25];
    uint16_t tex = 0;
    const uint32_t out = (uint32_t)ctx->r6, tex_out = (uint32_t)ctx->r7;
    const int r = rs64::terrain::get_cell(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5, h, &tex);

    if (r == 0 && guest_ptr(out))
    {
        for (int i = 0; i < 25; ++i) MEM_B(i, (gpr)(int32_t)out) = h[i];
    }

    if (r == 0 && guest_ptr(tex_out)) MEM_H(0, (gpr)(int32_t)tex_out) = (int16_t)tex;

    ctx->r2 = r;
}

void ex_terrain_set_cell(uint8_t* rdram, recomp_context* ctx)
{

    int8_t h[25];
    const uint32_t in = (uint32_t)ctx->r6;

    if (!guest_ptr(in))
    {
        ctx->r2 = rs64::terrain::NoMap;

        return;
    }

    for (int i = 0; i < 25; ++i) h[i] = (int8_t)MEM_B(i, (gpr)(int32_t)in);

    ctx->r2 = rs64::terrain::set_cell(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5, h, (int)(int32_t)ctx->r7);
}

const std::pair<const char*, recomp_func_t*> kNrmExports[] = {
    { "rs64_state_id", ex_state_id }, { "rs64_in_cutscene", ex_in_cutscene }, { "rs64_frame_seconds", ex_frame_seconds },
    { "rs64_mission_seconds", ex_mission_seconds }, { "rs64_get_flag", ex_get_flag }, { "rs64_set_flag", ex_set_flag },
    { "rs64_fire_rule_event", ex_fire_rule_event }, { "rs64_run_rule_line", ex_run_rule_line }, { "rs64_rule_var_get", ex_rule_var_get },
    { "rs64_rule_var_set", ex_rule_var_set }, { "rs64_object_by_name", ex_object_by_name }, { "rs64_object_health", ex_object_health },
    { "rs64_destroy_object", ex_destroy_object }, { "rs64_terrain_get_cell", ex_terrain_get_cell }, { "rs64_terrain_set_cell", ex_terrain_set_cell },
    { "rs64_key_down", ex_key_down }, { "rs64_save_slot", ex_save_slot }, { "rs64_light_set", ex_light_set }, { "rs64_spawn_unit", ex_spawn_unit }, { "rs64_camera_view", ex_camera_view },
};

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
        if (rs64::host::hook_has_event(h)) names.push_back(event_name(h));
    }

    for (const std::string& n : names) list.push_back(n.c_str());

    list.push_back(nullptr);
    recomp::overlays::register_base_events(list.data());

    for (uint32_t h = 0; h < RS64_HOOK_COUNT; ++h)
    {
        s_event_index[h] = rs64::host::hook_has_event(h) ? recomp::overlays::get_base_event_index(event_name(h)) : (size_t)-1;
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

    for (const auto& [name, fn] : kNrmExports) recomp::overlays::register_base_export(name, fn);

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

// The v9 host-dispatched hooks: a copy of the game's registers with a0-a2 set, so handlers read their values there and the game's own registers stay as they were.
extern "C" void rs64_hook_event(uint32_t hook, uint8_t* rdram, recomp_context* ctx, uint32_t a0, uint32_t a1, uint32_t a2)
{

    if (!s_host_ready.load(std::memory_order_acquire) || !ctx) return;

    recomp_context copy = *ctx;

    copy.r4 = (gpr)(int32_t)a0;
    copy.r5 = (gpr)(int32_t)a1;
    copy.r6 = (gpr)(int32_t)a2;
    rs64_hook(hook, rdram, &copy);
}

// osViSwapBuffer, on the game thread once a frame in every state: RS64_HOOK_FRAME, then state and cutscene changes.
extern "C" void rs64_host_frame(uint8_t* rdram, recomp_context* ctx, uint32_t fb)
{

    static int s_state = -1;
    static int s_cutscene = 0;

    rs64_hook_event(RS64_HOOK_FRAME, rdram, ctx, fb, 0, 0);

    const int state = rs64_state_current();

    if (state != s_state)
    {
        rs64_hook_event(RS64_HOOK_STATE_CHANGED, rdram, ctx, (uint32_t)state, (uint32_t)s_state, 0);
        s_state = state;
    }

    const int cutscene = rs64_state_in_cinematic() ? 1 : 0;

    if (cutscene != s_cutscene)
    {
        rs64_hook_event(RS64_HOOK_CUTSCENE, rdram, ctx, (uint32_t)cutscene, 0, 0);
        s_cutscene = cutscene;
    }
}
