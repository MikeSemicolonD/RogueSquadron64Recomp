#include "menu_config.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "os_compat.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "recomp.h"   // recomp_context, recomp_func_t (for mod-provided @exports)
#include "video_config.h"

// Menu button system v2 — one typed button model for the front-end and pause
// menus, driven by mod JSON. See docs/adding-menus-and-buttons.md and
// plans/menu-button-system-v2.md. A "button" is { menu, type, behavior, labels,
// placement }; the install engine rebuilds the target menu's entry list, merging
// native entries with mod buttons by order/anchor. Behavior keys resolve to a
// host registry (built-in actions/toggles); mod-provided behavior (@export) is
// Phase 1b.

namespace recomp { void* alloc(uint8_t* rdram, size_t size); }
#include "main.h"   // rs64_menu_request_quit, rs64_toggle_fullscreen, rs64_get_fullscreen

namespace {

constexpr uint32_t GCMD = 0x800CE730u;
constexpr uint32_t GCMD_OFF = GCMD - 0x80000000u;   // byte offset into rdram
constexpr int MAX_ENTRIES = 8;                      // menu_entries[8]
constexpr int STR_CAP = 63;

// Front-end menu ids (gCurrentMenuData +0x04).
enum FrontMenu : uint8_t {
    MENU_MAIN                = 0,
    MENU_ACCOUNT             = 1,
    MENU_OPTIONS             = 2,
    MENU_GAME_SETTINGS       = 3,
    MENU_ELITE_ROGUES        = 4,
    MENU_CONTROLLER_SETTINGS = 5,
    MENU_SOUND_SETTINGS      = 6,
    MENU_PASSCODES           = 7,
    MENU_BIOGRAPHIES         = 8,
    // Ids 0-12 are native (setupMenuData range-checks < 13). An id >= 13 bails to
    // the shared tail with 0 entries, so our install hook fully builds it: a
    // custom page. See plans/custom-menu-pages-plan.md.
    MENU_MOD_PAGE            = 13,
    MENU_NONE                = 0xFF,
};

// Entry sub-types. 1 = submenu transition (self-targeting = re-run setupMenuData,
// the only way to re-commit labels); 4 = in-range no-op on confirm (intercept owns
// the action).
constexpr uint8_t SUBTYPE_SUBMENU = 1;
constexpr uint8_t SUBTYPE_HOST    = 4;
constexpr int16_t YESNO_X = -0x40;

bool env_disabled(const char* name) {
    const char* v = recomp::os::getenv(name);
    return v && *v && *v != '0';
}

// The custom front-end menu is suppressed when ROGUESQ_NO_MENU_BUTTONS is set, OR for the demo
// boot target -- the custom menu displaces the original title's attract-idle path, so demo needs
// the stock front end. See project_boot_target_nav_engine memory / nav_sequencer.cpp.
bool menu_buttons_off() {
    static const bool off = [] {
        if (env_disabled("ROGUESQ_NO_MENU_BUTTONS")) return true;
        const char* bt = recomp::os::getenv("ROGUESQ_BOOT_TARGET");
        return bt && std::strncmp(bt, "demo", 4) == 0;
    }();
    return off;
}

// A game_settings toggle that `replace`s a known native toggle slot flips in place
// via that slot's native sub-type-5 re-render (no reload). On by default; set
// ROGUESQ_NO_TOGGLE_INPLACE to force the reload path.
bool toggle_inplace() { static bool v = !env_disabled("ROGUESQ_NO_TOGGLE_INPLACE"); return v; }

// ---------------------------------------------------------------------------
// Config model
// ---------------------------------------------------------------------------

enum class BType { Action, Toggle, Slider, Submenu };

struct Button {
    std::string id;
    std::string menu;        // named location (main_menu, game_settings, pause, ...)
    BType type = BType::Action;
    std::string behavior;    // action or toggle registry key (or @export in 1b)
    std::string confirm;     // Action: non-empty => YES/NO prompt
    std::string label;       // Action label
    std::string label_on;    // Toggle labels
    std::string label_off;
    std::string page;        // Submenu: the page it opens; page entries use menu==page
    std::string title;       // Submenu: the opened page's title (default = label)
    // placement
    bool has_order = false;
    int order = 0;
    std::string before, after, replace;   // anchor to a native alias / button id
    int x = 0, y = 0;
    int flags = 0x4001;      // pause entry flag word (0x4000 style + 0x0001 selectable)
    std::string mod_id;      // owning mod (for @export resolution)
    // Slider (value control rendered as a font-glyph bar in the label).
    int smin = 0, smax = 100, sstep = 10;
    int bar_width = 10;
    std::string bar_filled = "|";
    std::string bar_empty  = ".";
};

struct MenuConfig {
    std::vector<Button> buttons;
    // menu name -> native aliases to hide
    std::unordered_map<std::string, std::vector<std::string>> hides;
};

MenuConfig default_config();

void parse_button(const nlohmann::json& e, const std::string& mod_id, std::vector<Button>& out) {
    if (!e.is_object()) return;
    Button b;
    b.mod_id = mod_id;
    b.id       = e.value("id", std::string{});
    b.menu     = e.value("menu", std::string{"main_menu"});
    std::string t = e.value("type", std::string{"action"});
    b.type = (t == "toggle") ? BType::Toggle : (t == "slider") ? BType::Slider
           : (t == "submenu") ? BType::Submenu : BType::Action;
    const char* bkey = (b.type == BType::Toggle) ? "toggle" : (b.type == BType::Slider) ? "slider" : "action";
    b.behavior = e.value(bkey, std::string{});
    b.page  = e.value("page",  std::string{});
    b.title = e.value("title", std::string{});
    b.smin  = e.value("min",  b.smin);
    b.smax  = e.value("max",  b.smax);
    b.sstep = e.value("step", b.sstep);
    b.bar_width  = e.value("bar_width",  b.bar_width);
    b.bar_filled = e.value("bar_filled", b.bar_filled);
    b.bar_empty  = e.value("bar_empty",  b.bar_empty);
    b.confirm  = e.value("confirm", std::string{});
    b.label    = e.value("label", std::string{});
    b.label_on  = e.value("label_on",  std::string{});
    b.label_off = e.value("label_off", std::string{});
    if (e.contains("order")) { b.has_order = true; b.order = e.value("order", 0); }
    b.before  = e.value("before",  std::string{});
    b.after   = e.value("after",   std::string{});
    b.replace = e.value("replace", std::string{});
    b.x = e.value("x", 0);
    b.y = e.value("y", 0);
    b.flags = e.value("flags", 0x4001);
    out.push_back(std::move(b));
}

void apply_json(MenuConfig& m, const std::string& path, const std::string& mod_id) {
    std::ifstream f(path);
    if (!f.is_open()) return;
    nlohmann::json j;
    try { f >> j; } catch (...) { return; }
    try {
        if (j.contains("buttons") && j["buttons"].is_array()) {
            for (const auto& e : j["buttons"]) parse_button(e, mod_id, m.buttons);
        }
        if (j.contains("hide") && j["hide"].is_object()) {
            for (auto it = j["hide"].begin(); it != j["hide"].end(); ++it) {
                if (!it.value().is_array()) continue;
                auto& v = m.hides[it.key()];
                for (const auto& a : it.value()) v.push_back(a.get<std::string>());
            }
        }
    } catch (...) {}
}

// Built-in defaults: the QUIT action on the main menu and QUIT TO DESKTOP in the
// pause menu (not mods — always present unless a mod overrides by id or hides).
MenuConfig default_config() {
    MenuConfig m;

    Button quit;
    quit.id = "quit";
    quit.menu = "main_menu";
    quit.type = BType::Action;
    quit.behavior = "quit";
    quit.label = "QUIT";
    quit.x = -165;
    m.buttons.push_back(std::move(quit));

    Button pause_quit;
    pause_quit.id = "pause_quit";
    pause_quit.menu = "pause";
    pause_quit.type = BType::Action;
    pause_quit.behavior = "quit";
    pause_quit.label = "QUIT TO DESKTOP";
    m.buttons.push_back(std::move(pause_quit));

    return m;
}

MenuConfig build_config() {
    MenuConfig m = default_config();

    if (char* base = SDL_GetBasePath()) {
        std::string dir = base; SDL_free(base);
        apply_json(m, dir + "roguesq_menu.json", std::string{});
    }
    for (const auto& d : recomp::mods::get_all_mod_details("rs64")) {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;
        std::filesystem::path root = recomp::mods::get_mod_filename(d.mod_id);
        if (root.empty()) continue;
        apply_json(m, (root / "roguesq_menu.json").string(), d.mod_id);
    }

    // id-based override: a later button (a mod) replaces an earlier one (the
    // default, or an earlier mod) with the same id, in place. Id-less buttons are
    // always kept.
    std::vector<Button> merged;
    std::unordered_map<std::string, size_t> by_id;
    for (auto& b : m.buttons) {
        if (!b.id.empty()) {
            auto it = by_id.find(b.id);
            if (it != by_id.end()) { merged[it->second] = std::move(b); continue; }
            by_id[b.id] = merged.size();
        }
        merged.push_back(std::move(b));
    }
    m.buttons = std::move(merged);
    return m;
}

// Built ONCE, lazily (never rebuilt on the game thread — mod-mutex + I/O).
const MenuConfig& config() {
    static const MenuConfig cached = build_config();
    return cached;
}

// ---------------------------------------------------------------------------
// Behavior registries (built-in). Phase 1b resolves @export names to mod code.
// ---------------------------------------------------------------------------

struct ToggleImpl { std::function<bool()> get; std::function<void()> toggle; };
struct SliderImpl { std::function<int()> get; std::function<void(int)> set; };

std::unordered_map<std::string, std::function<void()>>& actions() {
    static std::unordered_map<std::string, std::function<void()>> r = {
        { "quit", [] { rs64_menu_request_quit(); } },
    };
    return r;
}
std::unordered_map<std::string, ToggleImpl>& toggles() {
    static std::unordered_map<std::string, ToggleImpl> r = {
        { "fullscreen", { [] { return rs64_get_fullscreen() != 0; },
                          [] { rs64_toggle_fullscreen(); } } },
    };
    return r;
}
// draw_distance: the roguesq_video.json drawDistance multiplier as a percent (100-250), for mod menus; applies live and is saved.
std::unordered_map<std::string, SliderImpl>& sliders() {
    static std::unordered_map<std::string, SliderImpl> r = {
        { "draw_distance", { [] { return (int)std::lround(rs64::video::draw_distance() * 100.0f); },
                             [](int v) { rs64::video::set_draw_distance((float)v / 100.0f); } } },
    };
    return r;
}

void fire_action(const std::string& key) {
    auto it = actions().find(key);
    if (it != actions().end() && it->second) it->second();
}
bool toggle_state(const std::string& key) {
    auto it = toggles().find(key);
    return it != toggles().end() && it->second.get && it->second.get();
}
void toggle_flip(const std::string& key) {
    auto it = toggles().find(key);
    if (it != toggles().end() && it->second.toggle) it->second.toggle();
}

// Call a mod native-library export (native C, no guest stack needed): a zeroed
// ctx, marshaling via r2 (bool/int return) and r4 (int arg).
void call_export(uint8_t* rdram, recomp_func_t* fn) {
    if (!fn) return;
    recomp_context ctx{};
    fn(rdram, &ctx);
}
bool call_export_bool(uint8_t* rdram, recomp_func_t* fn) {
    if (!fn) return false;
    recomp_context ctx{};
    fn(rdram, &ctx);
    return ctx.r2 != 0;
}

// Resolve a mod native-library export by name, cached once found. Resolution is
// LAZY (at menu-show time) because config() is built at boot, before the game
// loads mod native libraries — resolving there would always miss.
recomp_func_t* resolve_export(const std::string& mod_id, const std::string& name) {
    if (mod_id.empty() || name.empty()) return nullptr;
    static std::unordered_map<std::string, recomp_func_t*> cache;
    std::string key = mod_id + "/" + name;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    recomp_func_t* fn = recomp::mods::get_mod_export(mod_id, name);
    if (fn) cache[key] = fn;   // only cache successes; retry until the lib loads
    return fn;
}

// Fill a button's behavior function pointers from its @export name (or leave them
// null for built-in behaviors, which resolve by key against the registries).
struct BehaviorFns { recomp_func_t* action = nullptr, * toggle_get = nullptr, * toggle_set = nullptr; };
BehaviorFns resolve_behavior(const std::string& mod_id, BType type, const std::string& behavior) {
    BehaviorFns f;
    if (behavior.size() < 2 || behavior[0] != '@') return f;
    std::string name = behavior.substr(1);
    if (type == BType::Action) f.action = resolve_export(mod_id, name);
    else { f.toggle_set = resolve_export(mod_id, name); f.toggle_get = resolve_export(mod_id, name + "_get"); }
    return f;
}

// ---------------------------------------------------------------------------
// rdram accessors + string alloc
// ---------------------------------------------------------------------------

inline int32_t rd_w(uint8_t* r, uint32_t off)           { return *(int32_t*)(r + off); }
inline int16_t rd_h(uint8_t* r, uint32_t off)           { return *(int16_t*)(r + (off ^ 2)); }
inline uint8_t rd_b(uint8_t* r, uint32_t off)           { return r[off ^ 3]; }
inline void    wr_w(uint8_t* r, uint32_t off, int32_t v){ *(int32_t*)(r + off) = v; }
inline void    wr_h(uint8_t* r, uint32_t off, int16_t v){ *(int16_t*)(r + (off ^ 2)) = v; }
inline void    wr_b(uint8_t* r, uint32_t off, uint8_t v){ r[off ^ 3] = v; }

uint32_t alloc_str(uint8_t* rdram, const std::string& s) {
    static std::unordered_map<std::string, uint32_t> cache;
    auto it = cache.find(s);
    if (it != cache.end()) return it->second;
    size_t n = s.size() > STR_CAP ? STR_CAP : s.size();
    void* p = recomp::alloc(rdram, n + 1);
    if (!p) return 0;
    uint32_t off = (uint32_t)((uint8_t*)p - rdram);
    for (size_t i = 0; i < n; ++i) rdram[(off + (uint32_t)i) ^ 3] = (uint8_t)s[i];
    rdram[(off + (uint32_t)n) ^ 3] = 0;
    uint32_t vaddr = off + 0x80000000u;
    cache.emplace(s, vaddr);
    return vaddr;
}

// ---------------------------------------------------------------------------
// Front-end menu entry slots
// ---------------------------------------------------------------------------

struct Slot { int32_t label; uint8_t sub; int32_t param; int16_t x, y; int32_t scaler; };

Slot read_slot(uint8_t* r, int i) {
    return { rd_w(r, GCMD_OFF + 0x08 + i * 4), rd_b(r, GCMD_OFF + 0x28 + i),
             rd_w(r, GCMD_OFF + 0x74 + i * 4), rd_h(r, GCMD_OFF + 0x30 + i * 4),
             rd_h(r, GCMD_OFF + 0x32 + i * 4), rd_w(r, GCMD_OFF + 0x54 + i * 4) };
}
void write_slot(uint8_t* r, int i, const Slot& s) {
    wr_w(r, GCMD_OFF + 0x08 + i * 4, s.label);
    wr_b(r, GCMD_OFF + 0x28 + i,     s.sub);
    wr_w(r, GCMD_OFF + 0x74 + i * 4, s.param);
    wr_h(r, GCMD_OFF + 0x30 + i * 4, s.x);
    wr_h(r, GCMD_OFF + 0x32 + i * 4, s.y);
    wr_w(r, GCMD_OFF + 0x54 + i * 4, s.scaler);
}
bool label_is_empty(uint8_t* r, uint32_t label_vaddr) {
    if (label_vaddr < 0x80000000u || label_vaddr >= 0x80800000u) return true;
    return rd_b(r, label_vaddr - 0x80000000u) == 0;
}

// ---------------------------------------------------------------------------
// Menu-name resolution + native aliases (for ordering anchors / replace / hide)
// ---------------------------------------------------------------------------

uint8_t frontend_menu_id(const std::string& name) {
    if (name == "main_menu")           return MENU_MAIN;
    if (name == "options")             return MENU_OPTIONS;
    if (name == "game_settings")       return MENU_GAME_SETTINGS;
    if (name == "sound_settings")      return MENU_SOUND_SETTINGS;
    if (name == "controller_settings") return MENU_CONTROLLER_SETTINGS;
    return MENU_NONE;
}
const char* frontend_menu_name(uint8_t id) {
    switch (id) {
        case MENU_MAIN:                return "main_menu";
        case MENU_OPTIONS:             return "options";
        case MENU_GAME_SETTINGS:       return "game_settings";
        case MENU_SOUND_SETTINGS:      return "sound_settings";
        case MENU_CONTROLLER_SETTINGS: return "controller_settings";
        default:                       return nullptr;
    }
}

// A native entry alias: match a built slot by sub-type (+ optionally unk74).
struct NativeAlias { uint8_t sub; int32_t param; bool match_param; };

const std::unordered_map<std::string, NativeAlias>* native_aliases(uint8_t menu_id) {
    static const std::unordered_map<std::string, NativeAlias> main_menu = {
        { "start",   { 1, 1, true } },
        { "options", { 1, 2, true } },
    };
    static const std::unordered_map<std::string, NativeAlias> game_settings = {
        { "auto_roll",        { 5, 0, true } },
        { "auto_level",       { 5, 1, true } },
        { "free_camera",      { 5, 2, true } },
        { "crosshairs",       { 5, 3, true } },
        { "high_resolution",  { 5, 4, true } },
        { "restore_defaults", { 14, 0, false } },
        { "back",             { 25, 0, false } },
    };
    switch (menu_id) {
        case MENU_MAIN:          return &main_menu;
        case MENU_GAME_SETTINGS: return &game_settings;
        default:                 return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Per-slot binding, read by the confirm intercept
// ---------------------------------------------------------------------------

struct SlotBinding {
    bool set = false; BType type = BType::Action; std::string key, confirm;
    // toggle_get_fn/toggle_set_fn double as the slider getter/setter @exports.
    recomp_func_t* action_fn = nullptr, * toggle_get_fn = nullptr, * toggle_set_fn = nullptr;
    int smin = 0, smax = 100, sstep = 10;   // slider range
    std::string label_on, label_off;        // toggle labels (for in-place relabel)
};
SlotBinding s_slot[MAX_ENTRIES];

// Custom mod pages: hosted on a real menu id (whose native builder runs the required
// text setup) and flag-gated so the host menu still works when reached normally. A
// "submenu" button opens a named page; buttons with menu==<page> are its entries.
constexpr uint8_t MOD_PAGE_HOST = MENU_GAME_SETTINGS;
std::string s_active_page;              // page currently shown on the host (empty = none)
uint8_t     s_page_parent = MENU_MAIN;  // menu to return to on Back

// In-place game_settings toggles reuse a native toggle slot (native sub-type 5) so the
// game re-renders in place. Each `replace`s a native toggle, taking its param and its
// ON/OFF label textIds, which we override with the mod's labels reflecting the mod's
// own state. Only slots whose textIds are known are supported.
struct InplaceToggle { int param; unsigned off_tid, on_tid; std::string on, off, key; recomp_func_t* get = nullptr; };
std::vector<InplaceToggle> s_inplace_tgls;

// Map a game_settings `replace` alias to its native toggle {param, OFF tid, ON tid}.
bool inplace_slot(const std::string& alias, int& param, unsigned& off_tid, unsigned& on_tid) {
    if (alias == "crosshairs") { param = 3; off_tid = 0x6C; on_tid = 0x6D; return true; }
    return false;   // other slots: probe their textIds before adding
}

enum class ConfirmState { None, Pending };
ConfirmState s_confirm_state = ConfirmState::None;
std::string s_confirm_action, s_confirm_prompt;
recomp_func_t* s_confirm_fn = nullptr;

// Toggle state / flip for a button, preferring its mod @export over the built-in.
bool button_toggle_state(uint8_t* rdram, const std::string& key, recomp_func_t* get_fn) {
    return get_fn ? call_export_bool(rdram, get_fn) : toggle_state(key);
}

// Slider value get/set, preferring the mod @export (get -> r2, set arg -> r4).
int slider_value(uint8_t* rdram, const std::string& key, recomp_func_t* get_fn) {
    if (get_fn) { recomp_context c{}; get_fn(rdram, &c); return (int)(int32_t)c.r2; }
    auto it = sliders().find(key);
    return (it != sliders().end() && it->second.get) ? it->second.get() : 0;
}
void slider_apply(uint8_t* rdram, const std::string& key, recomp_func_t* set_fn, int v) {
    if (set_fn) { recomp_context c{}; c.r4 = (gpr)(uint32_t)v; set_fn(rdram, &c); return; }
    auto it = sliders().find(key);
    if (it != sliders().end() && it->second.set) it->second.set(v);
}
// "LABEL |||||....." — a font-glyph bar (via the same text pipeline as any label).
std::string slider_bar_text(const std::string& label, int value, int vmin, int vmax,
                            int width, const std::string& fch, const std::string& ech) {
    if (vmax <= vmin) vmax = vmin + 1;
    if (width < 1) width = 1;
    long filled = (long)((double)(value - vmin) / (vmax - vmin) * width + 0.5);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    std::string bar;
    for (int i = 0; i < width; ++i) bar += (i < filled) ? fch : ech;
    return label.empty() ? bar : (label + " " + bar);
}
// Step a slider value on confirm: +step, wrapping max -> min.
int slider_next(int value, int vmin, int vmax, int step) {
    int v = value + step;
    if (v > vmax) v = vmin;
    if (v < vmin) v = vmin;
    return v;
}

// One item in a menu's merged entry list (native or synthesized mod button).
struct Item {
    double order;
    Slot slot;
    bool is_native;
    bool is_spacer;
    SlotBinding binding;   // for mod-button items
};

bool alias_matches(const Slot& s, const NativeAlias& a) {
    return s.sub == a.sub && (!a.match_param || s.param == a.param);
}

// Find the order of a native/button anchor by name, for before/after resolution.
bool anchor_order(const std::vector<Item>& items, uint8_t menu_id,
                  const std::string& name, double& out) {
    const auto* aliases = native_aliases(menu_id);
    const NativeAlias* na = nullptr;
    if (aliases) {
        auto it = aliases->find(name);
        if (it != aliases->end()) na = &it->second;
    }
    for (const auto& it : items) {
        if (it.is_native && na && alias_matches(it.slot, *na)) { out = it.order; return true; }
        if (!it.is_native && it.binding.key == name)           { out = it.order; return true; }
    }
    return false;
}

// Rebuild the current front-end menu's entry list = native entries merged with
// this menu's mod buttons, ordered by order/anchors, dropping hidden/replaced
// natives and spacers as needed to fit MAX_ENTRIES.
void rebuild_frontend_menu(uint8_t* rdram, uint8_t menu_id) {
    const char* mname = frontend_menu_name(menu_id);
    if (!mname) return;
    if (menu_id == MENU_GAME_SETTINGS) s_inplace_tgls.clear();   // re-detected below

    const uint8_t n = rd_b(rdram, GCMD_OFF + 0x95);
    int32_t ref_scaler = n > 0 ? read_slot(rdram, 0).scaler : 0x3F800000;

    std::vector<Item> items;
    for (uint8_t i = 0; i < n && i < MAX_ENTRIES; ++i) {
        Slot s = read_slot(rdram, i);
        Item it; it.order = i * 10.0; it.slot = s; it.is_native = true;
        it.is_spacer = label_is_empty(rdram, (uint32_t)s.label);
        items.push_back(it);
    }

    // hides for this menu
    const auto* aliases = native_aliases(menu_id);
    auto hide_it = config().hides.find(mname);
    if (hide_it != config().hides.end() && aliases) {
        for (const auto& alias : hide_it->second) {
            auto ai = aliases->find(alias);
            if (ai == aliases->end()) continue;
            for (auto vi = items.begin(); vi != items.end(); ) {
                if (vi->is_native && alias_matches(vi->slot, ai->second)) vi = items.erase(vi);
                else ++vi;
            }
        }
    }

    // merge mod buttons for this menu
    for (const auto& b : config().buttons) {
        if (frontend_menu_id(b.menu) != menu_id) continue;
        // Sound Settings sliders are placed natively by sound_settings_install.
        if (menu_id == MENU_SOUND_SETTINGS && b.type == BType::Slider) continue;

        // replace: drop the matching native and take its order
        double ord;
        bool have_ord = false;
        if (!b.replace.empty() && aliases) {
            auto ai = aliases->find(b.replace);
            if (ai != aliases->end()) {
                for (auto vi = items.begin(); vi != items.end(); ) {
                    if (vi->is_native && alias_matches(vi->slot, ai->second)) {
                        ord = vi->order; have_ord = true; vi = items.erase(vi);
                    } else ++vi;
                }
            }
        }
        if (!have_ord && !b.after.empty()  && anchor_order(items, menu_id, b.after,  ord)) { ord += 5;  have_ord = true; }
        if (!have_ord && !b.before.empty() && anchor_order(items, menu_id, b.before, ord)) { ord -= 5;  have_ord = true; }
        if (!have_ord && b.has_order) { ord = b.order; have_ord = true; }
        if (!have_ord) { double mx = 0; for (auto& it : items) mx = std::max(mx, it.order); ord = mx + 10; }

        // Submenu opener: sub-type 1 to the host menu; the binding carries the page.
        if (b.type == BType::Submenu) {
            uint32_t lbl = alloc_str(rdram, b.label);
            if (!lbl) continue;
            Item it; it.order = ord; it.is_native = false; it.is_spacer = false;
            it.slot = Slot{ (int32_t)lbl, SUBTYPE_SUBMENU, (int32_t)MOD_PAGE_HOST,
                            (int16_t)b.x, (int16_t)b.y, ref_scaler };
            it.binding = { true, BType::Submenu, b.page };
            items.push_back(it);
            continue;
        }

        // synthesize the slot
        BehaviorFns fns = resolve_behavior(b.mod_id, b.type, b.behavior);
        std::string text;
        if (b.type == BType::Toggle)
            text = button_toggle_state(rdram, b.behavior, fns.toggle_get) ? b.label_on : b.label_off;
        else if (b.type == BType::Slider)
            text = slider_bar_text(b.label, slider_value(rdram, b.behavior, fns.toggle_get),
                                   b.smin, b.smax, b.bar_width, b.bar_filled, b.bar_empty);
        else
            text = b.label;
        uint32_t lbl = alloc_str(rdram, text);
        if (!lbl) continue;
        // Toggle/Slider + confirm-action = self-targeting sub-type 1 (rebuild reflects
        // the new state / rides the YES-NO transition). Plain action = no-op host. In
        // native game_settings with ROGUESQ_TOGGLE_INPLACE, a toggle is emitted as the
        // native sub-type 5 (param = a game-settings toggle index) so the game's own
        // handler re-renders it in place (no reload). TEST: index 3 (crosshairs slot).
        uint8_t sub; int32_t param;
        int ip_param; unsigned ip_off, ip_on;
        if (b.type == BType::Toggle && toggle_inplace() && menu_id == MENU_GAME_SETTINGS
            && !b.replace.empty() && inplace_slot(b.replace, ip_param, ip_off, ip_on)) {
            sub = 5; param = ip_param;   // native in-place re-render on the replaced slot
            s_inplace_tgls.push_back({ ip_param, ip_off, ip_on, b.label_on, b.label_off,
                                       b.behavior, fns.toggle_get });
        } else {
            bool selfsub = (b.type == BType::Slider) || !b.confirm.empty() || b.type == BType::Toggle;
            sub = selfsub ? SUBTYPE_SUBMENU : SUBTYPE_HOST;
            param = selfsub ? (int32_t)menu_id : 0;
        }
        Slot s{ (int32_t)lbl, sub, param, (int16_t)b.x, (int16_t)b.y, ref_scaler };
        Item it; it.order = ord; it.slot = s; it.is_native = false; it.is_spacer = false;
        it.binding = { true, b.type, b.behavior, b.confirm, fns.action, fns.toggle_get, fns.toggle_set,
                       b.smin, b.smax, b.sstep, b.label_on, b.label_off };
        items.push_back(it);
    }

    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b){ return a.order < b.order; });

    // trim to MAX_ENTRIES: drop spacers first, then trailing
    auto count_kept = [&]{ return (int)items.size(); };
    for (auto vi = items.begin(); vi != items.end() && count_kept() > MAX_ENTRIES; ) {
        if (vi->is_spacer) vi = items.erase(vi); else ++vi;
    }
    if ((int)items.size() > MAX_ENTRIES) items.resize(MAX_ENTRIES);

    // write back
    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};
    int j = 0;
    for (auto& it : items) {
        if (j >= MAX_ENTRIES) break;
        write_slot(rdram, j, it.slot);
        if (!it.is_native) s_slot[j] = it.binding;
        ++j;
    }
    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)j);
}

// ---------------------------------------------------------------------------
// Sound Settings native slider. A mod `type:"slider"` with `menu:"sound_settings"`
// and a `replace` anchor becomes a real volume-slider entry (sub-type 21). Its
// channel index is redirected to an unused settings byte so the value is
// independent of audio; endpoint labels (label_off/label_on) and the value (synced
// to the mod's @export) are overridden. See docs/adding-menus-and-buttons.md.
// ---------------------------------------------------------------------------

constexpr uint32_t VOL_BYTES = 0x80130B60u - 0x80000000u;  // channel volume bytes (0..N)
constexpr int      VOL_MAX   = 0x80;                       // native slider range
constexpr uint8_t  SND_SLIDER_SUB = 21;                    // music-volume sub-type
constexpr int      SND_CH_FIRST = 3, SND_CH_LAST = 7;      // unused channel bytes

struct SndSlider {
    int slot = -1, channel = 0, smin = 0, smax = 100;
    std::string off_label, on_label, key;
    recomp_func_t* get_fn = nullptr, * set_fn = nullptr;
};
std::vector<SndSlider> s_snd_sliders;

// Resolve a Sound Settings native entry name to its current slot index.
int snd_alias_slot(uint8_t* rdram, const std::string& name) {
    struct A { uint8_t sub; int32_t param; };
    static const std::unordered_map<std::string, A> al = {
        { "music", { 21, 8 } }, { "sound_fx", { 22, 4 } }, { "speech", { 23, 7 } },
        { "subtitles", { 5, 5 } }, { "stereo", { 5, 6 } },
        { "restore_defaults", { 15, 5 } }, { "back", { 25, 0 } },
    };
    auto it = al.find(name);
    if (it == al.end()) return -1;
    uint8_t n = rd_b(rdram, GCMD_OFF + 0x95);
    for (uint8_t i = 0; i < n && i < MAX_ENTRIES; ++i) {
        Slot s = read_slot(rdram, i);
        if (s.sub == it->second.sub && s.param == it->second.param) return i;
    }
    return -1;
}

int byte_from_value(int v, int lo, int hi) {
    if (hi <= lo) return 0;
    long b = (long)(v - lo) * VOL_MAX / (hi - lo);
    return b < 0 ? 0 : (b > VOL_MAX ? VOL_MAX : (int)b);
}
int value_from_byte(int b, int lo, int hi) {
    return hi <= lo ? lo : lo + (int)((long)b * (hi - lo) / VOL_MAX);
}

// Install mod sliders into Sound Settings (called from the setupMenuData tail).
void sound_settings_install(uint8_t* rdram) {
    s_snd_sliders.clear();
    int next_ch = SND_CH_FIRST;
    for (const auto& b : config().buttons) {
        if (b.type != BType::Slider || frontend_menu_id(b.menu) != MENU_SOUND_SETTINGS) continue;
        int slot = b.replace.empty() ? -1 : snd_alias_slot(rdram, b.replace);
        if (slot < 0 || next_ch > SND_CH_LAST) continue;   // needs a replace anchor + free channel
        int ch = next_ch++;
        BehaviorFns fns = resolve_behavior(b.mod_id, b.type, b.behavior);
        Slot s = read_slot(rdram, slot);
        s.label = (int32_t)alloc_str(rdram, b.label);
        s.sub = SND_SLIDER_SUB;
        write_slot(rdram, slot, s);
        SndSlider ss;
        ss.slot = slot; ss.channel = ch; ss.smin = b.smin; ss.smax = b.smax;
        ss.off_label = b.label_off.empty() ? "OFF" : b.label_off;
        ss.on_label  = b.label_on.empty()  ? "MAX" : b.label_on;
        ss.key = b.behavior; ss.get_fn = fns.toggle_get; ss.set_fn = fns.toggle_set;
        wr_b(rdram, VOL_BYTES + ch,
             (uint8_t)byte_from_value(slider_value(rdram, ss.key, ss.get_fn), ss.smin, ss.smax));
        s_snd_sliders.push_back(std::move(ss));
    }
}

const SndSlider* snd_slider_for_slot(int slot) {
    for (const auto& ss : s_snd_sliders) if (ss.slot == slot) return &ss;
    return nullptr;
}

// Build a custom mod page on the host menu: title from the opening submenu button,
// entries from config buttons whose menu == the page name, plus a Back to the parent
// menu. Reserves a slot for Back and caps at MAX_ENTRIES.
void build_mod_page(uint8_t* rdram, const std::string& page, uint8_t parent) {
    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};
    const int32_t sc = 0x3F800000;

    std::string title = page;
    for (const auto& b : config().buttons)
        if (b.type == BType::Submenu && b.page == page) { title = b.title.empty() ? b.label : b.title; break; }
    wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, title));

    int j = 0;
    for (const auto& b : config().buttons) {
        if (b.menu != page) continue;
        if (j >= MAX_ENTRIES - 1) break;   // reserve a slot for Back
        BehaviorFns fns = resolve_behavior(b.mod_id, b.type, b.behavior);
        std::string text;
        if (b.type == BType::Toggle)
            text = button_toggle_state(rdram, b.behavior, fns.toggle_get) ? b.label_on : b.label_off;
        else if (b.type == BType::Slider)
            text = slider_bar_text(b.label, slider_value(rdram, b.behavior, fns.toggle_get),
                                   b.smin, b.smax, b.bar_width, b.bar_filled, b.bar_empty);
        else
            text = b.label;
        uint32_t lbl = alloc_str(rdram, text);
        if (!lbl) continue;
        // Page toggles reload (self-target the host): in-place sub-type 5 is not
        // usable on a page (it re-asserts native game_settings over the page).
        bool selfsub = (b.type == BType::Toggle) || (b.type == BType::Slider) || !b.confirm.empty();
        write_slot(rdram, j, Slot{ (int32_t)lbl, selfsub ? SUBTYPE_SUBMENU : SUBTYPE_HOST,
                   selfsub ? (int32_t)MOD_PAGE_HOST : 0, (int16_t)b.x, (int16_t)b.y, sc });
        s_slot[j] = { true, b.type, b.behavior, b.confirm, fns.action, fns.toggle_get, fns.toggle_set,
                      b.smin, b.smax, b.sstep, b.label_on, b.label_off };
        ++j;
    }
    write_slot(rdram, j, Slot{ (int32_t)alloc_str(rdram, "BACK"), SUBTYPE_SUBMENU, parent, 0, 0, sc });
    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)(j + 1));
}

} // namespace

// ---------------------------------------------------------------------------
// Front-end hook entry points
// ---------------------------------------------------------------------------

extern "C" void rs64_menu_config_mark_dirty(void) {}
extern "C" void rs64_menu_config_init(void) { (void)config(); }

extern "C" void rs64_menu_install_main(uint8_t* rdram) {
    static const bool off = menu_buttons_off();
    if (off) return;
    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    const uint8_t menu = rd_b(rdram, GCMD_OFF + 0x04);

    // YES/NO confirm screen (MAIN_MENU-scoped), rides the engine's transitions.
    if (menu == MENU_MAIN && s_confirm_state == ConfirmState::Pending) {
        wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, s_confirm_prompt));
        Slot yes{ (int32_t)alloc_str(rdram, "YES"), SUBTYPE_SUBMENU, 0, YESNO_X, 0, rd_w(rdram, GCMD_OFF + 0x54) };
        Slot no { (int32_t)alloc_str(rdram, "NO"),  SUBTYPE_SUBMENU, 0, YESNO_X, 0, rd_w(rdram, GCMD_OFF + 0x54 + 4) };
        write_slot(rdram, 0, yes);
        write_slot(rdram, 1, no);
        wr_b(rdram, GCMD_OFF + 0x95, 2);
        return;
    }

    // Leaving the host menu ends the mod page (a native visit to the host is normal).
    if (menu != MOD_PAGE_HOST) s_active_page.clear();

    // A mod page, rendered on the host menu so its native builder does the text
    // setup; we override the title + entries. Flag-gated by s_active_page so a normal
    // visit to the host menu is untouched.
    if (menu == MOD_PAGE_HOST && !s_active_page.empty()) {
        build_mod_page(rdram, s_active_page, s_page_parent);
        return;
    }

    if (frontend_menu_name(menu)) rebuild_frontend_menu(rdram, menu);
    if (menu == MENU_SOUND_SETTINGS) sound_settings_install(rdram);
}

extern "C" void rs64_menu_confirm_main(uint8_t* rdram) {
    static const bool off = menu_buttons_off();
    if (off) return;
    uint8_t e = rd_b(rdram, GCMD_OFF + 0x94);   // highlighted entry

    if (s_confirm_state == ConfirmState::Pending) {
        if (rd_b(rdram, GCMD_OFF + 0x04) != 0) { s_confirm_state = ConfirmState::None; return; }
        if (e == 0) {
            if (s_confirm_fn) call_export(rdram, s_confirm_fn); else fire_action(s_confirm_action);
            s_confirm_state = ConfirmState::None;
        } else if (e == 1) { s_confirm_state = ConfirmState::None; }
        return;
    }

    if (e >= MAX_ENTRIES || !s_slot[e].set) return;
    const SlotBinding& b = s_slot[e];
    if (b.type == BType::Submenu) {
        // Open the page: flag it and remember the parent to return to. The entry's
        // sub-type-1 transition to the host menu then renders the page.
        s_active_page = b.key;
        s_page_parent = rd_b(rdram, GCMD_OFF + 0x04);
    } else if (b.type == BType::Toggle) {
        if (b.toggle_set_fn) call_export(rdram, b.toggle_set_fn); else toggle_flip(b.key);
        // In-place toggles are emitted as native sub-type 5, so the game re-renders
        // them; reload toggles ride the sub-type-1 self-target. Either way, just flip.
    } else if (b.type == BType::Slider) {
        int v = slider_value(rdram, b.key, b.toggle_get_fn);
        slider_apply(rdram, b.key, b.toggle_set_fn, slider_next(v, b.smin, b.smax, b.sstep));
    } else if (!b.confirm.empty()) {
        s_confirm_state = ConfirmState::Pending;
        s_confirm_action = b.key;
        s_confirm_prompt = b.confirm;
        s_confirm_fn = b.action_fn;
    } else {
        if (b.action_fn) call_export(rdram, b.action_fn); else fire_action(b.key);
    }
}

// ---------------------------------------------------------------------------
// Sound Settings native slider hooks. The common volume-slider handler
// (menuControllerInput 0x800b5920) takes a channel index at sp+0xEF; when the
// selected entry is a mod slider we redirect it to that slider's unused channel
// (independent value). getGameOrFrontText 0xB7/0xB8 = OFF/MAX endpoint text; we
// override them per slider. updateMenuPerFrame syncs the byte to the mod @export.
// ---------------------------------------------------------------------------

// Redirect the channel index for a mod slider (hook: 0x800b5920, sp = ctx->r29).
extern "C" void rs64_slider_redirect(uint8_t* rdram, uint32_t sp) {
    if (rd_b(rdram, GCMD_OFF + 0x04) != MENU_SOUND_SETTINGS || sp < 0x80000000u) return;
    const SndSlider* ss = snd_slider_for_slot(rd_b(rdram, GCMD_OFF + 0x94));
    if (ss) wr_b(rdram, (sp - 0x80000000u) + 0xEF, (uint8_t)ss->channel);
}

// Override a textId's returned string (hook: getGameOrFrontText jr-ra 0x800558c4):
// mod slider OFF/MAX endpoints, and in-place game-settings toggle labels.
extern "C" unsigned rs64_gof_override(uint8_t* rdram, unsigned textId) {
    // In-place game_settings toggles: the replaced native toggle's label textIds show
    // the mod's labels, reflecting the mod's own state.
    for (const auto& t : s_inplace_tgls) {
        if (textId == t.off_tid || textId == t.on_tid) {
            bool on = button_toggle_state(rdram, t.key, t.get);
            return alloc_str(rdram, on ? t.on : t.off);
        }
    }
    if (rd_b(rdram, GCMD_OFF + 0x04) != MENU_SOUND_SETTINGS) return 0;
    const SndSlider* ss = snd_slider_for_slot(rd_b(rdram, GCMD_OFF + 0x94));
    if (!ss) return 0;
    if (textId == 0xB7) return alloc_str(rdram, ss->off_label);
    if (textId == 0xB8) return alloc_str(rdram, ss->on_label);
    return 0;
}

// Sync each mod slider's channel byte to its @export value (hook: updateMenuPerFrame).
extern "C" void rs64_slider_sync(uint8_t* rdram) {
    if (rd_b(rdram, GCMD_OFF + 0x04) != MENU_SOUND_SETTINGS) return;
    for (const auto& ss : s_snd_sliders) {
        int byte = rd_b(rdram, VOL_BYTES + ss.channel);
        slider_apply(rdram, ss.key, ss.set_fn, value_from_byte(byte, ss.smin, ss.smax));
    }
}

// ---------------------------------------------------------------------------
// Pause menu (in-mission). Buttons are placed into a spacer/terminator slot in
// their target PauseMenuStuff array and tagged with a unique nextMenu sentinel
// (0xF010+). The render/confirm hooks dispatch by sentinel via the maps below.
// ---------------------------------------------------------------------------

namespace {

constexpr uint16_t PAUSE_TEXTID    = 0x0002;   // any valid textId (render overrides)
constexpr uint16_t PAUSE_MARK_BASE = 0xF010;

struct PauseBinding {
    BType type; std::string key, label, label_on, label_off;
    recomp_func_t* action_fn = nullptr, * toggle_get_fn = nullptr, * toggle_set_fn = nullptr;
    int smin = 0, smax = 100, sstep = 10, bar_width = 10;
    std::string bar_filled = "|", bar_empty = ".";
};
std::unordered_map<uint16_t, PauseBinding> s_pause_marks;   // sentinel -> behavior

// Pause array slot targets. Root objectives terminator, and the game-settings
// spacer. (Phase 1 supports one button per target; multi-entry needs relocation.)
struct PauseTarget { uint32_t entry_off; bool is_terminator; };
bool pause_target(const std::string& menu, PauseTarget& out) {
    if (menu == "pause")               { out = { 0x80109E1Cu - 0x80000000u, true  }; return true; }
    if (menu == "pause_game_settings") { out = { 0x80109E54u - 0x80000000u, false }; return true; }
    return false;
}

} // namespace

extern "C" void rs64_pause_install(uint8_t* rdram) {
    static const bool off = env_disabled("ROGUESQ_NO_PAUSE_BUTTONS");
    if (off) return;
    // Only while the pause menu is OPEN (D_8010CA20 == 5) — touching these arrays
    // during level-load/briefing starves the audio scheduler.
    if (rd_w(rdram, 0x8010CA20u - 0x80000000u) != 5) return;

    s_pause_marks.clear();
    uint16_t next_mark = PAUSE_MARK_BASE;

    for (const auto& b : config().buttons) {
        PauseTarget tgt;
        if (!pause_target(b.menu, tgt)) continue;
        if (next_mark - PAUSE_MARK_BASE >= 0xF0) break;
        const uint16_t mark = next_mark++;

        // Skip if this slot already carries a sentinel we set this frame (idempotent
        // re-DMA handling: the array resets on load, we re-apply while paused).
        if ((uint16_t)rd_h(rdram, tgt.entry_off + 0x4) == mark) {
            // already installed with this mark; keep the binding
        } else {
            wr_h(rdram, tgt.entry_off + 0x0, (int16_t)b.flags);
            wr_h(rdram, tgt.entry_off + 0x2, (int16_t)PAUSE_TEXTID);
            wr_h(rdram, tgt.entry_off + 0x4, (int16_t)mark);
            if (tgt.is_terminator) wr_h(rdram, tgt.entry_off + 0x6, (int16_t)0xFFFF);  // new terminator
        }
        BehaviorFns fns = resolve_behavior(b.mod_id, b.type, b.behavior);
        s_pause_marks[mark] = { b.type, b.behavior, b.label, b.label_on, b.label_off,
                                fns.action, fns.toggle_get, fns.toggle_set,
                                b.smin, b.smax, b.sstep, b.bar_width, b.bar_filled, b.bar_empty };
    }
}

// Render hook: return the label vaddr for a pause sentinel, or 0 if not ours.
extern "C" unsigned rs64_pause_label_for(uint8_t* rdram, unsigned marker) {
    auto it = s_pause_marks.find((uint16_t)marker);
    if (it == s_pause_marks.end()) return 0;
    const PauseBinding& b = it->second;
    std::string text;
    if (b.type == BType::Toggle)
        text = button_toggle_state(rdram, b.key, b.toggle_get_fn) ? b.label_on : b.label_off;
    else if (b.type == BType::Slider)
        text = slider_bar_text(b.label, slider_value(rdram, b.key, b.toggle_get_fn),
                               b.smin, b.smax, b.bar_width, b.bar_filled, b.bar_empty);
    else
        text = b.label;
    return alloc_str(rdram, text);
}

// Confirm hook: run the behavior for a pause sentinel.
extern "C" void rs64_pause_confirm(uint8_t* rdram, unsigned marker) {
    auto it = s_pause_marks.find((uint16_t)marker);
    if (it == s_pause_marks.end()) return;
    const PauseBinding& b = it->second;
    if (b.type == BType::Toggle) {
        if (b.toggle_set_fn) call_export(rdram, b.toggle_set_fn); else toggle_flip(b.key);
    } else if (b.type == BType::Slider) {
        int v = slider_value(rdram, b.key, b.toggle_get_fn);
        slider_apply(rdram, b.key, b.toggle_set_fn, slider_next(v, b.smin, b.smax, b.sstep));
    } else {
        if (b.action_fn) call_export(rdram, b.action_fn); else fire_action(b.key);
    }
}
