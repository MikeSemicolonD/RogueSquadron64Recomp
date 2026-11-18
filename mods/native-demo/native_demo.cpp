// Example native-library mod for the menu button system. A mod ships a DLL whose
// exports are referenced from roguesq_menu.json with a leading '@'. Each export
// has the recomp signature void(uint8_t* rdram, recomp_context* ctx); the menu
// system calls it with a zeroed ctx and marshals via registers: a toggle getter
// returns state in r2, a slider setter reads its value from r4. It also uses the
// rs64 host API (rs64_mod_init below) for a menu-frame hook and a live text source.
//
// A real mod would include the recomp mod SDK's recomp.h for the full
// recomp_context type. This example only needs the general-purpose register file
// prefix (r0..r31), so it defines a minimal view to stay self-contained.

#include <cstdint>
#include <cstdio>
#include <string>
#include "rs64/host_api.h"

#ifdef _WIN32
#define MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define MOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Prefix of recomp_context: the 32 general-purpose registers as 64-bit values.
struct RecompRegs { uint64_t r[32]; };

// librecomp checks this on load; must be 1.
MOD_EXPORT uint32_t recomp_api_version = 1;

// A custom action button behavior (referenced as "@demo_action").
MOD_EXPORT void demo_action(uint8_t* rdram, void* ctx) {
    (void)rdram; (void)ctx;
    std::fprintf(stderr, "[native-demo] demo_action fired\n");
    std::fflush(stderr);
}

// A custom toggle backed by native state ("@demo_toggle" flips, "demo_toggle_get"
// reports state in r2).
static int s_demo_on = 0;
MOD_EXPORT void demo_toggle(uint8_t* rdram, void* ctx) {
    (void)rdram; (void)ctx;
    s_demo_on = !s_demo_on;
    std::fprintf(stderr, "[native-demo] demo_toggle -> %d\n", s_demo_on);
    std::fflush(stderr);
}
MOD_EXPORT void demo_toggle_get(uint8_t* rdram, void* ctx) {
    (void)rdram;
    ((RecompRegs*)ctx)->r[2] = (uint64_t)s_demo_on;
}

// A custom slider backed by native state ("@demo_slider" sets from r4,
// "demo_slider_get" returns the value in r2).
static int s_demo_value = 50;
MOD_EXPORT void demo_slider(uint8_t* rdram, void* ctx) {
    (void)rdram;
    s_demo_value = (int)(uint32_t)((RecompRegs*)ctx)->r[4];
}
MOD_EXPORT void demo_slider_get(uint8_t* rdram, void* ctx) {
    (void)rdram;
    ((RecompRegs*)ctx)->r[2] = (uint64_t)(unsigned)s_demo_value;
}

// Host API (rs64_mod_init): takes the table from r4, registers a menu-frame hook and a text source, answers r2 = 0 and r3 = the API version built against.
// The host skips the mod and drops its registrations if r2 != 0 or r3 is newer than the host API.
static const rs64_host_api* g_api;
static int s_frames = 0;
static const char* demo_text(void*) {
    static char b[32];
    std::snprintf(b, sizeof b, "DEMO FRAMES %d", s_frames);
    return b;
}
static int demo_menu_frame(uint32_t, uint8_t*, void*, void*) {
    ++s_frames;
    return RS64_HOOK_CONTINUE;
}

// Level rules (host API v6): a rule action "demo_log key=value ..." logs its fields, then raises the custom when "demo.ping",
// which a rule "when custom demo.ping" in a level's rules.txt can answer.
static void demo_log(uint8_t*, void*, const char* const* keys, const char* const* values, int count, void*) {
    std::string line = "[native-demo] rule action demo_log";
    for (int i = 0; i < count; i++) line += std::string(" ") + keys[i] + "=" + values[i];
    g_api->log(line.c_str());
    g_api->fire_rule_event("demo.ping", "");
}

// Level rules check (host API v9): demo_kills() counts the units destroyed this mission (RS64_HOOK_UNIT_DESTROYED), so a rule can test
// "when true: demo_kills() >= 5". RS64_HOOK_MISSION_READY starts each mission's count at 0.
static int s_kills = 0;
static int demo_unit_destroyed(uint32_t, uint8_t*, void*, void*) {
    ++s_kills;
    return RS64_HOOK_CONTINUE;
}
static int demo_mission_ready(uint32_t, uint8_t*, void*, void*) {
    s_kills = 0;
    return RS64_HOOK_CONTINUE;
}
static double demo_kills(uint8_t*, void*, const char*, void*) {
    return s_kills;
}

MOD_EXPORT void rs64_mod_init(uint8_t* rdram, void* ctx) {
    (void)rdram;
    RecompRegs* r = (RecompRegs*)ctx;
    g_api = (const rs64_host_api*)(uintptr_t)r->r[4];
    if ((uint32_t)r->r[5] < 1 || !g_api || g_api->size < sizeof(rs64_host_api)) {
        r->r[3] = RS64_HOST_API_VERSION;
        r->r[2] = 1;
        return;
    }
    g_api->add_hook(RS64_HOOK_MENU_FRAME, demo_menu_frame, nullptr);
    g_api->add_text_source("demo_frames", demo_text, nullptr);
    g_api->add_rule_action("demo_log", demo_log, nullptr);
    g_api->add_hook(RS64_HOOK_UNIT_DESTROYED, demo_unit_destroyed, nullptr);
    g_api->add_hook(RS64_HOOK_MISSION_READY, demo_mission_ready, nullptr);
    g_api->add_rule_condition("demo_kills", demo_kills, nullptr);
    char line[64];
    std::snprintf(line, sizeof line, "[native-demo] host API v%d taken", (int)RS64_HOST_API_VERSION);
    g_api->log(line);
    r->r[3] = RS64_HOST_API_VERSION;
    r->r[2] = 0;
}
