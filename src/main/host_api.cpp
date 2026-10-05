#include "host_api.h"
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rs64::host {
namespace {
struct Hook { rs64_hook_fn fn; void* user; };
struct Text { rs64_text_fn fn; void* user; };
struct Filter { rs64_input_fn fn; void* user; };
std::vector<Hook> g_hooks[RS64_HOOK_COUNT];
std::map<std::string, Text> g_text;
std::vector<Filter> g_filters;
std::map<std::string, int> g_flags;
std::mutex g_flags_mu;
Services g_services;
bool g_sealed = false;
constexpr uint32_t kMaxHooks = sizeof(g_hooks) / sizeof(g_hooks[0]);

void log_line(const char* s) {
    if (g_services.log) g_services.log(s);
    else fprintf(stderr, "%s\n", s);
}
struct Action { rs64_action_fn fn; void* user; };
struct Condition { rs64_condition_fn fn; void* user; };
struct Key { rs64_key_fn fn; void* user; };
struct Quit { rs64_event_fn fn; void* user; };
std::map<std::string, Action> g_actions;
std::map<std::string, Condition> g_conditions;
std::vector<Key> g_keys;
std::vector<Quit> g_quits;
std::map<std::string, const void*> g_services_by_name;

bool refuse_sealed(const char* what) {
    if (!g_sealed) {
        return false;
    }
    log_line((std::string("[host] ") + what + " rejected: registry is sealed").c_str());
    return true;
}
uint32_t skips_fwd() { return g_services.cutscene_skips ? g_services.cutscene_skips() : 0u; }
void yield_fwd(uint8_t* r, void* c) {
    if (g_services.yield) {
        g_services.yield(r, c);
    }
}
void quit_fwd() {
    if (g_services.request_quit) {
        g_services.request_quit();
    }
}
void text_fwd(int on) {
    if (g_services.text_input) {
        g_services.text_input(on);
    }
}
int kb_fwd() { return g_services.screen_keyboard_shown ? g_services.screen_keyboard_shown() : -1; }
int held_fwd() { return g_services.any_key_held ? g_services.any_key_held() : 0; }
const char* base_fwd() { return g_services.base_path ? g_services.base_path() : ""; }
uint32_t call_fwd(uint8_t* r, void* c, uint32_t v, const uint32_t* a, uint32_t n, const uint32_t* st, uint32_t ns, float f12, float f14, float* f0) {
    return g_services.call ? g_services.call(r, c, v, a, n, st, ns, f12, f14, f0) : 0u;
}
uint32_t alloc_fwd(uint8_t* r, uint32_t n) { return g_services.alloc ? g_services.alloc(r, n) : 0u; }
const char* state_fwd() { return g_services.state_id ? g_services.state_id() : "unknown"; }
int cut_fwd() { return g_services.in_cutscene ? g_services.in_cutscene() : 0; }
}

int add_hook(uint32_t hook, rs64_hook_fn fn, void* user) {
    if (hook == RS64_HOOK_NONE || hook >= kMaxHooks || !fn) {
        if (hook == RS64_HOOK_NONE || hook >= kMaxHooks) {
            log_line((std::string("[host] add_hook rejected: invalid hook id ") + std::to_string(hook)).c_str());
        }
        return -1;
    }
    if (g_sealed) {
        log_line("[host] add_hook rejected: registry is sealed");
        return -1;
    }
    g_hooks[hook].push_back({fn, user});
    return 0;
}

int dispatch(uint32_t hook, uint8_t* rdram, void* ctx) {
    if (hook >= kMaxHooks) return RS64_HOOK_CONTINUE;
    const auto& hooks = g_hooks[hook];
    const size_t count = hooks.size();
    for (size_t i = 0; i < count; ++i) {
        if (hooks[i].fn(hook, rdram, ctx, hooks[i].user) == RS64_HOOK_RETURN) return RS64_HOOK_RETURN;
    }
    return RS64_HOOK_CONTINUE;
}

int add_text_source(const char* name, rs64_text_fn fn, void* user) {
    if (!name || !fn) return -1;
    if (g_sealed) {
        log_line("[host] add_text_source rejected: registry is sealed");
        return -1;
    }
    if (!g_text.emplace(name, Text{fn, user}).second) {
        log_line((std::string("[host] text source registered twice, keeping the first: ") + name).c_str());
        return -1;
    }
    return 0;
}

const char* text_source(const char* name) {
    auto it = g_text.find(name ? name : "");
    return it == g_text.end() ? nullptr : it->second.fn(it->second.user);
}

int add_input_filter(rs64_input_fn fn, void* user) {
    if (!fn) return -1;
    if (g_sealed) {
        log_line("[host] add_input_filter rejected: registry is sealed");
        return -1;
    }
    g_filters.push_back({fn, user});
    return 0;
}

void run_input_filters(int port, uint16_t* buttons, float* x, float* y) {
    const auto& filters = g_filters;
    const size_t count = filters.size();
    for (size_t i = 0; i < count; ++i) {
        filters[i].fn(port, buttons, x, y, filters[i].user);
    }
}

int add_action(const char* name, rs64_action_fn fn, void* user) {
    if (!name || !fn || refuse_sealed("add_action")) {
        return -1;
    }
    if (!g_actions.emplace(name, Action{fn, user}).second) {
        log_line((std::string("[host] action registered twice, keeping the first: ") + name).c_str());
        return -1;
    }
    return 0;
}

bool run_action(const char* name) {
    auto it = g_actions.find(name ? name : "");
    if (it == g_actions.end()) {
        return false;
    }
    it->second.fn(it->second.user);
    return true;
}

int add_condition(const char* name, rs64_condition_fn fn, void* user) {
    if (!name || !fn || refuse_sealed("add_condition")) {
        return -1;
    }
    if (!g_conditions.emplace(name, Condition{fn, user}).second) {
        log_line((std::string("[host] condition registered twice, keeping the first: ") + name).c_str());
        return -1;
    }
    return 0;
}

// -1 = no such condition, else 0 / 1.
int condition(const char* name) {
    auto it = g_conditions.find(name ? name : "");
    if (it == g_conditions.end()) {
        return -1;
    }
    return it->second.fn(it->second.user) != 0 ? 1 : 0;
}

int add_key_handler(rs64_key_fn fn, void* user) {
    if (!fn || refuse_sealed("add_key_handler")) {
        return -1;
    }
    g_keys.push_back({fn, user});
    return 0;
}

int run_key_handlers(const char* utf8, int key) {
    const size_t count = g_keys.size();
    for (size_t i = 0; i < count; ++i) {
        if (g_keys[i].fn(utf8, key, g_keys[i].user)) {
            return 1;
        }
    }
    return 0;
}

int add_quit_handler(rs64_event_fn fn, void* user) {
    if (!fn || refuse_sealed("add_quit_handler")) {
        return -1;
    }
    g_quits.push_back({fn, user});
    return 0;
}

int add_service(const char* name, const void* table) {
    if (!name || !table || refuse_sealed("add_service")) {
        return -1;
    }
    if (!g_services_by_name.emplace(name, table).second) {
        log_line((std::string("[host] service registered twice, keeping the first: ") + name).c_str());
        return -1;
    }
    return 0;
}

// Read-only after the registry seals, so lookups need no lock.
const void* get_service(const char* name) {
    auto it = g_services_by_name.find(name ? name : "");
    return it == g_services_by_name.end() ? nullptr : it->second;
}

void run_quit_handlers() {
    const size_t count = g_quits.size();
    for (size_t i = 0; i < count; ++i) {
        g_quits[i].fn(g_quits[i].user);
    }
}

void set_flag(const char* name, int value) {
    std::lock_guard<std::mutex> lk(g_flags_mu);
    g_flags[name ? name : ""] = value;
}

int flag(const char* name) {
    std::lock_guard<std::mutex> lk(g_flags_mu);
    auto it = g_flags.find(name ? name : "");
    return it == g_flags.end() ? 0 : it->second;
}

void reset_for_tests() {
    for (auto& v : g_hooks) v.clear();
    g_text.clear();
    g_filters.clear();
    g_actions.clear();
    g_conditions.clear();
    g_keys.clear();
    g_quits.clear();
    g_services_by_name.clear();
    g_services = Services{};
    g_sealed = false;
    std::lock_guard<std::mutex> lk(g_flags_mu);
    g_flags.clear();
}

const char* hook_name(uint32_t hook) {
    static const char* const names[] = {
        "RS64_HOOK_NONE", "RS64_HOOK_MENU_INPUT", "RS64_HOOK_MENU_PILOT_CHOSEN", "RS64_HOOK_MENU_FRAME",
        "RS64_HOOK_MISSION_INIT", "RS64_HOOK_MISSION_FRAME_DT", "RS64_HOOK_MISSION_FRAME_PADS", "RS64_HOOK_SONG_ACTIVE",
        "RS64_HOOK_SPEECH_RESPONSE_2", "RS64_HOOK_SPEECH_RESPONSE_1", "RS64_HOOK_MISSION_FRAME_NPCS", "RS64_HOOK_MISSION_END",
        "RS64_HOOK_TRANSITION_REQUEST", "RS64_HOOK_FREEZE_CHECK_A", "RS64_HOOK_FREEZE_CHECK_B", "RS64_HOOK_CUTSCENE_FREEZE_CHECK",
        "RS64_HOOK_OBJECTIVE_COUNT", "RS64_HOOK_RESULT_FAIL", "RS64_HOOK_RESULT_SUCCESS", "RS64_HOOK_WINGMAN_TICK",
        "RS64_HOOK_NPC_ACTIVATION", "RS64_HOOK_GRID_STREAM", "RS64_HOOK_MISSION_SELECT_INIT", "RS64_HOOK_MISSION_CONFIRMED",
        "RS64_HOOK_MISSION_SELECT_FONTS", "RS64_HOOK_MISSION_SELECT_TICK", "RS64_HOOK_CRAFT_SELECT_INIT", "RS64_HOOK_CRAFT_SELECT_TICK",
        "RS64_HOOK_CRAFT_ASSETS", "RS64_HOOK_HUD_FONTS", "RS64_HOOK_HUD_DRAW", "RS64_HOOK_RADAR",
        "RS64_HOOK_MENU_PAD", "RS64_HOOK_MAIN_MENU", "RS64_HOOK_POWERUP_TOUCH", "RS64_HOOK_POWERUP_COLLECT",
        "RS64_HOOK_TRIGGER_EFFECT", "RS64_HOOK_WALKER_TRIPPED",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == RS64_HOOK_COUNT, "hook_name table must list every hook id");
    return hook < RS64_HOOK_COUNT ? names[hook] : "RS64_HOOK_UNKNOWN";
}

void bind_services(const Services& s) { g_services = s; }

void seal() { g_sealed = true; }

Mark mark() {
    Mark m;
    for (const auto& h : g_hooks) m.hooks.push_back(h.size());
    m.filters = g_filters.size();
    for (const auto& t : g_text) m.text.insert(t.first);
    for (const auto& a : g_actions) m.actions.insert(a.first);
    for (const auto& c : g_conditions) m.conditions.insert(c.first);
    m.keys = g_keys.size();
    m.quits = g_quits.size();
    for (const auto& s : g_services_by_name) m.services.insert(s.first);
    return m;
}

void rollback(const Mark& m) {
    for (size_t i = 0; i < m.hooks.size() && i < kMaxHooks; ++i) g_hooks[i].resize(m.hooks[i]);
    g_filters.resize(m.filters);
    for (auto it = g_text.begin(); it != g_text.end();) {
        it = m.text.count(it->first) ? std::next(it) : g_text.erase(it);
    }
    for (auto it = g_actions.begin(); it != g_actions.end();) {
        it = m.actions.count(it->first) ? std::next(it) : g_actions.erase(it);
    }
    for (auto it = g_conditions.begin(); it != g_conditions.end();) {
        it = m.conditions.count(it->first) ? std::next(it) : g_conditions.erase(it);
    }
    g_keys.resize(m.keys);
    g_quits.resize(m.quits);
    for (auto it = g_services_by_name.begin(); it != g_services_by_name.end();) {
        it = m.services.count(it->first) ? std::next(it) : g_services_by_name.erase(it);
    }
}

uint32_t init_version() { return RS64_HOST_API_VERSION; }

// r3 = the API version the mod was built against (0 = not set, treated as 1).
bool accept_mod_init(uint32_t r2, uint32_t r3_built_version) {
    const uint32_t built = r3_built_version == 0 ? 1u : r3_built_version;
    return r2 == 0 && built <= RS64_HOST_API_VERSION;
}

const rs64_host_api* api() {
    static const rs64_host_api table = {
        RS64_HOST_API_VERSION, (uint32_t)sizeof(rs64_host_api), log_line, add_hook, add_text_source, add_input_filter,
        set_flag, flag, call_fwd, alloc_fwd, state_fwd, cut_fwd,
        add_action, add_condition, add_key_handler, add_quit_handler, skips_fwd, yield_fwd, quit_fwd, text_fwd, kb_fwd, held_fwd, base_fwd,
        add_service, get_service,
    };
    return &table;
}
}
