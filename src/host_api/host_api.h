#pragma once
#include "rs64/host_api.h"
#include "lockstep/lockstep_core.h"
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace rs64::host {
int add_hook(uint32_t hook, rs64_hook_fn fn, void* user);
int dispatch(uint32_t hook, uint8_t* rdram, void* ctx);
int add_text_source(const char* name, rs64_text_fn fn, void* user);
const char* text_source(const char* name);
int add_input_filter(rs64_input_fn fn, void* user);
void run_input_filters(int port, uint16_t* buttons, float* x, float* y);
int add_action(const char* name, rs64_action_fn fn, void* user);
bool run_action(const char* name);
int add_condition(const char* name, rs64_condition_fn fn, void* user);
int condition(const char* name);
int add_key_handler(rs64_key_fn fn, void* user);
int run_key_handlers(const char* utf8, int key);
int add_quit_handler(rs64_event_fn fn, void* user);
void run_quit_handlers();
// Hooks with a rogue_squadron.toml site (one each); the others are dispatched by host code.
constexpr bool hook_has_toml_site(uint32_t id) {
    return (id >= 1u && id <= RS64_HOOK_RADAR) || (id >= RS64_HOOK_POWERUP_TOUCH && id <= RS64_HOOK_ATST_TICK);
}
void set_flag(const char* name, int value);
int flag(const char* name);
void reset_for_tests();
const char* hook_name(uint32_t hook);
void seal();
struct Mark {
    std::vector<size_t> hooks;
    size_t filters = 0;
    std::set<std::string> text;
    std::set<std::string> actions, conditions, rule_actions;
    size_t keys = 0, quits = 0;
    std::set<std::string> services;
    size_t hash_parts = 0, valid_ram = 0, moved = 0;
};
int add_service(const char* name, const void* table);
const void* get_service(const char* name);
int add_hash_part(const char* name, const char* gate_flag, rs64_hash_fn fn, void* user);
// The registered hash parts whose flag is set, in registration order.
std::vector<rs64::ls::HashPart> hash_parts();
int add_valid_ram(uint32_t addr, uint32_t size);
int add_moved_range(uint32_t lo, uint32_t hi, uint32_t delta);
int add_rule_action(const char* name, rs64_rule_action_fn fn, void* user);
bool run_rule_action(const char* name, uint8_t* rdram, void* ctx, const char* const* keys, const char* const* values, int count);
int fire_rule_event(const char* name, const char* object);
int terrain_get_cell(uint8_t* rdram, uint32_t col, uint32_t row, int8_t heights[25], uint16_t* tex);
int terrain_set_cell(uint8_t* rdram, uint32_t col, uint32_t row, const int8_t heights[25], int tex);
// The (name, object) events fired since the last call, in order.
std::vector<std::pair<std::string, std::string>> take_rule_events();
const std::vector<rs64::ls::MovedRange>& moved_ranges();
// The game's 8 MB or a range a mod registered with add_valid_ram.
bool ram_ok(uint32_t addr);
Mark mark();
void rollback(const Mark& m);
uint32_t init_version();
bool accept_mod_init(uint32_t returned_r2, uint32_t returned_r3_built_version);

// What the game provides behind the table (host_api_game.cpp binds the real ones).
struct Services {
    void (*log)(const char*) = nullptr;
    uint32_t (*call)(uint8_t*, void*, uint32_t, const uint32_t*, uint32_t, const uint32_t*, uint32_t, float, float, float*) = nullptr;
    uint32_t (*alloc)(uint8_t*, uint32_t) = nullptr;
    const char* (*state_id)() = nullptr;
    int (*in_cutscene)() = nullptr;
    uint32_t (*cutscene_skips)() = nullptr;
    void (*yield)(uint8_t*, void*) = nullptr;
    void (*request_quit)() = nullptr;
    void (*text_input)(int) = nullptr;
    int (*screen_keyboard_shown)() = nullptr;
    int (*any_key_held)() = nullptr;
    const char* (*base_path)() = nullptr;
};
void bind_services(const Services& s);
const rs64_host_api* api();
}
