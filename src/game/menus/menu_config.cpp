#include "game/menus/menu_config.h"
#include "game/menus/menu_config_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "os_compat.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
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
#include "librecomp/addresses.hpp"
#include "rs64_toml_hooks.h"
#include "game/lang/lang.h"
#include "game/menus/menu_layout.h"
#include "game/menus/mods_page.h"
#include "video/video_config.h"

// Menu button system v2: one typed button model { menu, type, behavior, labels, placement } for the front-end and pause menus, driven by mod JSON (docs/guides/adding-menus-and-buttons.md).
// The install engine rebuilds the target menu's entry list, merging native entries with mod buttons by order/anchor; behavior keys resolve to a host registry or a mod @export.

#include "main/main.h"   // rs64_menu_request_quit, rs64_toggle_fullscreen, rs64_get_fullscreen
#include "input/input_host.h"
#include "input/touch_config.h"
#include "util/mips_call.h"
#include "host_api/host_api.h"
#include "util/debug_logs.h"

namespace rs64::menucfg {

constexpr const char* kLanguagesPage = "languages";
constexpr int STR_CAP = 63;

// The LANGUAGE page's pick: menuControllerInput frees the text, stores the entry's param to gGameSettings+6 and reloads front_TXT (0x800B57F8).
constexpr uint8_t SUBTYPE_LANGUAGE = 24;

constexpr uint8_t SUBTYPE_TOGGLE = 5;
constexpr uint8_t SUBTYPE_BACK   = 25;

// The game's text languages, by gGameSettings+6; a button label can map these codes to text.
constexpr const char* kTextLanguages[] = { "en", "de", "fr", "it", "es" };
constexpr int kTextLanguageCount = (int)(sizeof(kTextLanguages) / sizeof(kTextLanguages[0]));

// Config index for the active language mod's labels (after the built-in text languages).
constexpr int kModLanguage = kTextLanguageCount;

// Language of the text files last loaded (loadTxtFile hook); picks the menu config's label set.
std::atomic<int> s_text_language{0};

// The game's BACK (text 0xA4) in the shown text language.
const char* back_text()
{

    static const char* const kBack[] = { "BACK", "ZUR\334CK", "RETOUR", "BACK", "BACK" };

    return kBack[std::clamp(s_text_language.load(std::memory_order_relaxed), 0, kTextLanguageCount - 1)];
}

// Non-selectable line: the menu navigation skips it.
constexpr uint8_t SUBTYPE_LABEL   = 13;
constexpr int16_t YESNO_X = -0x40;

bool env_disabled(const char* name) {

    return recomp::dbg::env_on(name);
}

// The custom front-end menu is off when ROGUESQ_NO_MENU_BUTTONS is set or for the demo boot target, which needs the stock front end (the custom menu displaces the attract-idle path).
bool menu_buttons_off() {

    static const bool off = [] {

        if (env_disabled("ROGUESQ_NO_MENU_BUTTONS")) return true;

        const char* bt = recomp::dbg::env_str("ROGUESQ_BOOT_TARGET");

        return bt && std::strncmp(bt, "demo", 4) == 0;
    }();

    return off;
}

// ---- Config model ----

MenuConfig default_config(int lang);

using rs64::menu::to_latin1;

rs64::lang::MenuText s_menu_text;

// A label field is a string or a map of language code -> string ({"en": "BACK", "de": "ZURÜCK"}, optionally "key" for other mods' translations); a missing language falls back to "en", then to the first entry.
std::string text_field(const nlohmann::json& e, const char* key, int lang)
{

    if (!e.contains(key)) return {};

    const nlohmann::json& v = e[key];

    if (v.is_string()) return to_latin1(v.get<std::string>());
    if (!v.is_object() || v.empty()) return {};

    // shortcut: a mod-language label is encoded once, so its code page slots can be reused by other text after 127 newer characters; re-encode per draw if that shows up.
    const rs64::lang::Language* mod = lang == kModLanguage ? rs64::lang::active() : nullptr;
    const std::string code = mod ? mod->code : lang < kTextLanguageCount ? kTextLanguages[lang] : "en";
    const std::string text_key = v.contains("key") && v["key"].is_string() ? v["key"].get<std::string>() : std::string{};
    const std::string* keyed = nullptr;

    // A text key: the active language mod's lang/menu.json, then mods' menu_text.json, before the map's own entries.
    if (!text_key.empty() && mod)
    {
        const auto own = mod->menu.find(text_key);

        if (own != mod->menu.end()) keyed = &own->second;
    }

    if (!text_key.empty() && !keyed) keyed = rs64::lang::menu_text_for(s_menu_text, code, text_key);

    const std::string* mod_text = keyed ? keyed : mod && v.contains(mod->code) && v[mod->code].is_string() ? v[mod->code].get_ptr<const std::string*>() : nullptr;

    if (mod && mod_text)
    {
        const std::string shown = rs64::lang::encode_text(*mod_text);

        rs64::lang::note_original(shown, text_field(e, key, 0));

        return shown;
    }

    if (keyed) return to_latin1(*keyed);

    for (const char* c : { code.c_str(), "en" })
        if (v.contains(c) && v[c].is_string()) return to_latin1(v[c].get<std::string>());

    for (const auto& [k, t] : v.items())
        if (k != "key" && t.is_string()) return to_latin1(t.get<std::string>());

    return {};
}

std::string mods_text(const char* key, const char* en, const char* de, const char* fr)
{

    const nlohmann::json j = { { "t", { { "en", en }, { "de", de }, { "fr", fr }, { "key", std::string("mods.") + key } } } };
    const int lang = rs64::lang::active() ? kModLanguage : std::clamp(s_text_language.load(std::memory_order_relaxed), 0, kTextLanguageCount - 1);

    return text_field(j, "t", lang);
}

// "sound": a name below or a raw SFX id (0-127); absent keeps the game's sound.
MenuSfx parse_sound(const nlohmann::json& e, const std::string& id)
{

    static const std::pair<const char*, MenuSfx> kNames[] = {
        { "move", { 0x17, 0x7F } }, { "confirm", { 0x18, 0x6E } }, { "back", kSfxBack }, { "reject", { 0x19, 0x64 } },
        { "toggle", { 0x6A, 0x64 } }, { "type", { 0x31, 0x50 } }, { "delete", { 0x7C, 0x7F } }, { "none", { -2, 0 } },
    };

    if (!e.contains("sound")) return {};

    const nlohmann::json& v = e["sound"];

    if (v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() < 0x80) return { v.get<int>(), 0x64 };

    if (v.is_string())
    {
        for (const auto& [name, sfx] : kNames)
            if (v.get<std::string>() == name) return sfx;
    }

    fprintf(stderr, "[menu] %s: unknown sound %s\n", id.c_str(), v.dump().c_str());

    return {};
}

void parse_button(const nlohmann::json& e, const std::string& mod_id, std::vector<Button>& out, int lang)
{

    if (!e.is_object()) return;

    Button b;
    b.mod_id = mod_id;
    b.id       = e.value("id", std::string{});
    b.menu     = e.value("menu", std::string{"main_menu"});
    std::string t = e.value("type", std::string{"action"});

    // A cycle is a toggle whose key names a cycles() entry; its label shows the current choice.
    b.type = (t == "toggle" || t == "cycle") ? BType::Toggle : (t == "slider") ? BType::Slider
           : (t == "submenu") ? BType::Submenu : (t == "label") ? BType::Label : BType::Action;
    const char* bkey = (t == "cycle") ? "cycle" : (b.type == BType::Toggle) ? "toggle" : (b.type == BType::Slider) ? "slider" : "action";
    b.behavior = e.value(bkey, std::string{});

    // "config:<option>" names an option in the mod's own mod.toml config schema; the key carries the mod id so the registries need no button.
    if (b.behavior.rfind("config:", 0) == 0 && !mod_id.empty()) b.behavior = "config:" + mod_id + ":" + b.behavior.substr(7);

    b.page  = e.value("page",  std::string{});
    b.title = text_field(e, "title", lang);
    b.title_y = e.value("title_y", 0);
    b.leave_action = e.value("leave_action", std::string{});
    b.left_action = e.value("left_action", std::string{});
    b.right_action = e.value("right_action", std::string{});
    b.label_src = e.value("label_src", std::string{});
    b.note = b.type == BType::Label && e.value("note", false);
    b.transition = e.value("transition", std::string{});
    b.exit_when = e.value("exit_when", std::string{});
    b.smin  = e.value("min",  b.smin);
    b.smax  = e.value("max",  b.smax);
    b.sstep = e.value("step", b.sstep);
    b.bar_width  = e.value("bar_width",  b.bar_width);
    b.bar_filled = e.value("bar_filled", b.bar_filled);
    b.bar_empty  = e.value("bar_empty",  b.bar_empty);
    b.confirm  = text_field(e, "confirm", lang);
    b.label    = text_field(e, "label", lang);
    b.label_on  = text_field(e, "label_on", lang);
    b.label_off = text_field(e, "label_off", lang);

    if (e.contains("order")) { b.has_order = true; b.order = e.value("order", 0); }

    b.before  = e.value("before",  std::string{});
    b.after   = e.value("after",   std::string{});
    b.replace = e.value("replace", std::string{});

    if (e.contains("index") && e["index"].is_number_integer()) {
        b.has_index = true;
        b.index = e["index"].get<int>();

        if (!b.before.empty() || !b.after.empty() || e.contains("order"))
        {
            fprintf(stderr, "[menu] %s: index overrides its anchor\n", b.id.c_str());
        }
    }

    // "requires": one condition or a list, all of which must hold.
    if (e.contains("requires") && e["requires"].is_string()) b.requires_conds.push_back(rs64::menu::condition_key(e["requires"].get<std::string>(), mod_id));

    if (e.contains("requires") && e["requires"].is_array())
    {
        for (const auto& c : e["requires"])
            if (c.is_string()) b.requires_conds.push_back(rs64::menu::condition_key(c.get<std::string>(), mod_id));
    }

    if (e.contains("labels") && e["labels"].is_array())
    {
        for (const auto& l : e["labels"]) b.value_labels.push_back(text_field(nlohmann::json{ { "v", l } }, "v", lang));
    }

    b.label_other = text_field(e, "label_other", lang);
    b.x = e.value("x", 0);
    b.y = e.value("y", 0);
    b.gap_after = e.value("gap_after", 0);
    b.row = e.value("row", std::string{});
    b.scale = e.value("scale", 1.0f);
    b.flags = e.value("flags", 0x4001);
    b.sound = parse_sound(e, b.id);
    out.push_back(std::move(b));
}

void apply_json_text(MenuConfig& m, const std::string& text, const std::string& mod_id, int lang) {

    const char* file = mod_id.empty() ? "roguesq_menu.json" : mod_id.c_str();
    nlohmann::json j;

    try { j = nlohmann::json::parse(text); } catch (const std::exception& e)
    {
        fprintf(stderr, "[menu] %s: menu file not read: %s\n", file, e.what());

        return;
    }

    try {
        if (j.contains("buttons") && j["buttons"].is_array())
        {
            for (const auto& e : j["buttons"]) parse_button(e, mod_id, m.buttons, lang);
        }

        if (j.contains("hide") && j["hide"].is_object()) {
            for (auto it = j["hide"].begin(); it != j["hide"].end(); ++it) {
                if (!it.value().is_array()) continue;

                auto& v = m.hides[it.key()];

                for (const auto& a : it.value())
                {
                    if (a.is_string()) v.push_back(a.get<std::string>());
                    else fprintf(stderr, "[menu] %s: hide entry %s under %s is not a name; skipped\n", file, a.dump().c_str(), it.key().c_str());
                }
            }
        }

        if (j.contains("titles") && j["titles"].is_object())
        {
            for (auto it = j["titles"].begin(); it != j["titles"].end(); ++it)
            {
                if (!it.value().is_object()) continue;

                MenuTitle t;
                t.text = text_field(it.value(), "label", lang);
                t.x = it.value().value("x", 0);
                t.has_x = it.value().contains("x");
                t.has_y = it.value().contains("y");
                t.y = it.value().value("y", 0);
                m.titles[it.key()] = t;
            }
        }
    } catch (const std::exception& e)
    {
        fprintf(stderr, "[menu] %s: menu file read in part: %s\n", file, e.what());
    }
}

// Built-in defaults: LANGUAGE in OPTIONS, the QUIT action on the main menu and QUIT TO DESKTOP in the
// pause menu (not mods — always present unless a mod overrides by id or hides).
MenuConfig default_config(int lang) {

    MenuConfig m;

    // The game's LANGUAGE page, a GAME SETTINGS entry only on PAL consoles; labels are the game's text 0xB2 per language.
    static const char* const kLanguageLabel[] = { "LANGUAGE", "SPRACHE", "LANGUE", "LANGUAGE", "LANGUAGE" };

    Button language;
    language.id = "language";
    language.menu = "options";
    language.type = BType::Action;
    language.transition = "language";
    language.label = lang < kTextLanguageCount ? kLanguageLabel[lang] : "LANGUAGE";
    language.has_index = true;
    language.index = -2;

    // With language mods installed it opens a host page listing every language; each entry is the game's own pick (transition "language:<index>", mods after en/de/fr).
    if (rs64::lang::installed())
    {
        static const char* const kBuiltinNames[] = { "ENGLISH", "DEUTSCH", "FRANCAIS" };
        const std::vector<std::string> codes = rs64::lang::codes();

        language.type = BType::Submenu;
        language.transition.clear();
        language.page = kLanguagesPage;
        language.title = language.label;

        for (size_t i = 0; i < codes.size(); ++i)
        {
            const rs64::lang::Language* mod = rs64::lang::find(codes[i]);
            std::string name = mod ? to_latin1(mod->name) : kBuiltinNames[i];

            if (lang == kModLanguage && mod && mod == rs64::lang::active()) name = rs64::lang::encode_text(mod->name);
            else if (name.find('?') != std::string::npos) name = codes[i];

            Button e;
            e.id = "language_" + codes[i];
            e.menu = kLanguagesPage;
            e.type = BType::Action;
            e.transition = "language:" + std::to_string(i);
            e.label = name;

            // Space before BACK, as the DISPLAY SETTINGS pages have, under the last row shown (more languages scroll, languages_install).
            if (i + 1 == std::min(codes.size(), (size_t)MAX_ENTRIES - 1)) e.gap_after = 16;

            m.buttons.push_back(std::move(e));
        }
    }

    m.buttons.push_back(std::move(language));

    // MODS: the host's mod list (mods_install), on the main menu between OPTIONS and QUIT.
    Button mods;
    mods.id = "mods";
    mods.menu = "main_menu";
    mods.type = BType::Submenu;
    mods.page = kModsPage;
    mods.label = "MODS";
    mods.title = "MODS";
    mods.x = -163;
    m.buttons.push_back(std::move(mods));

    // No quitting to desktop on Android; the system handles app exit.
#ifndef __ANDROID__

    Button quit;
    quit.id = "quit";
    quit.menu = "main_menu";
    quit.type = BType::Action;
    quit.behavior = "quit";
    quit.label = "QUIT";
    quit.x = -170;
    m.buttons.push_back(std::move(quit));

    Button pause_quit;
    pause_quit.id = "pause_quit";
    pause_quit.menu = "pause";
    pause_quit.type = BType::Action;
    pause_quit.behavior = "quit";
    pause_quit.label = "QUIT TO DESKTOP";
    m.buttons.push_back(std::move(pause_quit));

#endif

    return m;
}

// Every enabled mod's menu_text.json ({ "<code>": { "<key>": "text" } }), in mod order.
void load_menu_text()
{

    s_menu_text = {};

    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        bool exists = false;
        const std::vector<char> text = recomp::mods::read_mod_file(d.mod_id, "menu_text.json", exists);

        if (!exists) continue;

        const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);

        if (!j.is_object())
        {
            fprintf(stderr, "[menu] %s: menu_text.json is not a JSON object\n", d.mod_id.c_str());
            continue;
        }

        for (const auto& [code, table] : j.items()) rs64::lang::add_menu_text(s_menu_text, code, table);
    }
}

// Menu files as { text, owning mod id }: roguesq_menu.json next to the exe, then each enabled mod's.
std::vector<std::pair<std::string, std::string>> menu_files()
{

    std::vector<std::pair<std::string, std::string>> files;

    if (char* base = SDL_GetBasePath())
    {
        std::string dir = base;
        SDL_free(base);
        std::ifstream f(dir + "roguesq_menu.json", std::ios::binary);

        if (f.is_open()) files.emplace_back(std::string(std::istreambuf_iterator<char>(f), {}), std::string{});
    }

    // A mod's menu file sits in its folder or inside its .nrm (RecompModTool additional_files).
    for (const auto& d : recomp::mods::get_all_mod_details("rs64"))
    {
        if (!recomp::mods::is_mod_enabled(d.mod_id)) continue;

        bool exists = false;
        const std::vector<char> text = recomp::mods::read_mod_file(d.mod_id, "roguesq_menu.json", exists);

        if (exists) files.emplace_back(std::string(text.begin(), text.end()), d.mod_id);
    }

    load_menu_text();

    return files;
}

MenuConfig build_config(const std::vector<std::pair<std::string, std::string>>& files, int lang)
{

    MenuConfig m = default_config(lang);

    for (const auto& [text, mod_id] : files) apply_json_text(m, text, mod_id, lang);

    // A later button with the same id replaces the earlier one in place; id-less buttons are always kept.
    std::vector<Button> merged;
    std::unordered_map<std::string, size_t> by_id;

    for (auto& b : m.buttons) {
        if (!b.id.empty())
        {
            auto it = by_id.find(b.id);

            if (it != by_id.end()) { merged[it->second] = std::move(b); continue; }

            by_id[b.id] = merged.size();
        }

        merged.push_back(std::move(b));
    }

    m.buttons = std::move(merged);

    return m;
}

// The menu files are read once, lazily (no mod-mutex I/O again on the game thread); one set per built-in text language, and the active mod language's set.
const MenuConfig& config()
{

    static const auto files = menu_files();

    static const std::vector<MenuConfig> cached = [] {

        std::vector<MenuConfig> v;

        for (int lang = 0; lang < kTextLanguageCount; ++lang) v.push_back(build_config(files, lang));

        return v;
    }();

    // A mod language's set is rebuilt when the active mod language changes (the files are already read; this only parses).
    if (const rs64::lang::Language* mod = rs64::lang::active())
    {
        static std::string s_code;
        static MenuConfig s_mod;

        if (s_code != mod->code)
        {
            s_mod = build_config(files, kModLanguage);
            s_code = mod->code;
        }

        return s_mod;
    }

    return cached[std::clamp(s_text_language.load(std::memory_order_relaxed), 0, kTextLanguageCount - 1)];
}

// ---- Behavior registries (built-in) ----

struct ToggleImpl { std::function<bool()> get; std::function<void()> toggle; };
struct SliderImpl { std::function<int()> get; std::function<void(int)> set; };

static void open_mods_folder()
{

#ifndef __ANDROID__

    std::string url = "file://";
    const std::string dir = recomp::mods::get_mods_directory().generic_string();

    if (dir.empty() || dir[0] != '/') url += '/';

    for (char c : dir) url += c == ' ' ? std::string("%20") : std::string(1, c);

    if (SDL_OpenURL(url.c_str()) != 0) fprintf(stderr, "[menu] open_mods_folder: %s\n", SDL_GetError());

#endif
}

std::unordered_map<std::string, std::function<void()>>& actions() {

    static std::unordered_map<std::string, std::function<void()>> r = {
        { "quit", [] { rs64_menu_request_quit(); } },
        { "reset_controls", [] { reset_controls(); } },
        { "open_mods_folder", [] { open_mods_folder(); } },
        { "touch_layout", [] { rs64_touch_layout_request(); } },
        { "none", [] {} },
    };

    return r;
}

// Live label text for page entries with a `label_src` (upper case, digits, spaces and < > only: menu font 5).
std::string source_text(const std::string& key) {

    const char* s = rs64::host::text_source(key.c_str());

    return s ? std::string(s) : std::string{};
}

bool hires_get();
void hires_flip();

// An on/off RT64 display setting (applied by the renderer at its next present, then saved).
ToggleImpl display_toggle(rs64::video::Display d) {

    return { [d] { return rs64::video::display_value(d) == 1; },
             [d] { rs64::video::step_display(d); } };
}

// Ray-tracing switches: shows the live state and flips the saved setting; a switch pinned by its env var does not respond.
ToggleImpl rt_toggle(rs64lights::Feature f) {

    return { [f] { return rs64lights::feature(f); },
             [f] {
                 if (!rs64lights::featurePinned(f)) {
                     rs64::video::set_rt_setting(f, !rs64lights::feature(f));
                 }
             } };
}

// Input settings shown in menus: read and changed under the bindings lock, saved to roguesq_input.json like the F6 window does.
int input_get(const std::function<int(const rs64::input::Bindings&)>& get)
{

    std::lock_guard<std::mutex> lk(rs64::app::g_bindings_mtx);

    return get(rs64::app::g_bindings);
}

void input_set(const std::function<void(rs64::input::Bindings&)>& set)
{

    std::lock_guard<std::mutex> lk(rs64::app::g_bindings_mtx);

    set(rs64::app::g_bindings);
    rs64::input::save_bindings(rs64::app::g_bindings, rs64::input::default_config_path());
}

std::unordered_map<std::string, ToggleImpl>& toggles() {

    static std::unordered_map<std::string, ToggleImpl> r = {
        { "fullscreen", { [] { return rs64_get_fullscreen() != 0; },
                          [] { rs64_toggle_fullscreen(); } } },
        { "gyro", { [] { return rs64::touch::gyro_enabled(); },
                    [] { rs64::touch::set_gyro_enabled(!rs64::touch::gyro_enabled()); } } },
        { "high_resolution", { [] { return hires_get(); }, [] { hires_flip(); } } },
        { "widescreen", display_toggle(rs64::video::Display::Widescreen) },
        { "frame_interpolation", display_toggle(rs64::video::Display::FrameInterpolation) },
        { "cutscene_draw_distance", { [] { return rs64::video::keep_cutscene_draw_distance(); },
                                      [] { rs64::video::set_keep_cutscene_draw_distance(!rs64::video::keep_cutscene_draw_distance()); } } },
        // The game's own terrain (caps the draw distance at 1.3X); takes effect at the next frame.
        { "legacy_terrain", { [] { return rs64::video::legacy_terrain(); },
                              [] { rs64::video::set_legacy_terrain(!rs64::video::legacy_terrain()); } } },
        { "framebuffer_writeback", { [] { return rs64::video::framebuffer_writeback(); },
                                     [] { rs64::video::set_framebuffer_writeback(!rs64::video::framebuffer_writeback()); } } },
        { "rt_lights", rt_toggle(rs64lights::Feature::Lights) },
        { "rt_shadows", rt_toggle(rs64lights::Feature::Shadows) },
        { "rt_soft_shadows", rt_toggle(rs64lights::Feature::SoftShadows) },
        { "rt_fog_shafts", rt_toggle(rs64lights::Feature::FogShafts) },
        { "rt_ao", rt_toggle(rs64lights::Feature::AmbientOcclusion) },
        { "rt_gi", rt_toggle(rs64lights::Feature::GlobalIllumination) },
        { "rt_reflections", rt_toggle(rs64lights::Feature::Reflections) },

        // Rumble turned on mid-session also reports the Rumble Pak, so effects start from the next mission (as in the F6 window).
        { "rumble", { [] { return input_get([](const rs64::input::Bindings& b) { return b.rumble.enabled ? 1 : 0; }) != 0; },
                      [] {
                          input_set([](rs64::input::Bindings& b) { b.rumble.enabled = !b.rumble.enabled; });

                          if (input_get([](const rs64::input::Bindings& b) { return b.rumble.enabled ? 1 : 0; })) rs64::app::g_report_rumble_pak.store(true);
                      } } },
    };

    return r;
}

// draw_distance: the roguesq_video.json drawDistance multiplier as a percent (100-800), for mod menus; applies live and is saved.
std::unordered_map<std::string, SliderImpl>& sliders() {

    static std::unordered_map<std::string, SliderImpl> r = {
        { "draw_distance", { [] { return (int)std::lround(rs64::video::draw_distance() * 100.0f); },
                             [](int v) { rs64::video::set_draw_distance((float)v / 100.0f); } } },

        // Mouse sensitivity in thousandths, smoothing and dead zone in percent; the dead zone is the gamepad's and every joystick's.
        { "mouse_sensitivity", { [] { return input_get([](const rs64::input::Bindings& b) { return (int)std::lround(b.mouse_sensitivity * 1000.0f); }); },
                                 [](int v) { input_set([v](rs64::input::Bindings& b) { b.mouse_sensitivity = (float)v / 1000.0f; }); } } },
        { "mouse_smoothing", { [] { return input_get([](const rs64::input::Bindings& b) { return (int)std::lround(b.mouse_smoothing * 100.0f); }); },
                               [](int v) { input_set([v](rs64::input::Bindings& b) { b.mouse_smoothing = (float)v / 100.0f; }); } } },
        { "deadzone", { [] { return input_get([](const rs64::input::Bindings& b) { return (int)std::lround(b.pad_deadzone * 100.0f); }); },
                        [](int v) {
                            input_set([v](rs64::input::Bindings& b) {
                                b.pad_deadzone = (float)v / 100.0f;

                                for (auto& d : b.joy_devices) d.deadzone = (float)v / 100.0f;
                            });
                        } } },
    };

    return r;
}

// Multi-choice settings ("type": "cycle"): confirm steps to the next choice and the label shows "LABEL: CHOICE". get -1 = a combination matching no choice (CUSTOM).
struct CycleImpl { std::function<int()> get; std::function<void()> next; std::function<std::string(int)> text; };

CycleImpl display_cycle(rs64::video::Display d)
{

    return { [d] { return rs64::video::display_value(d); },
             [d] { rs64::video::step_display(d); },
             [d](int v) { return rs64::video::display_label(d, v); } };
}

// draw_distance as a cycle of multipliers from 1X to 8X, shown as text (the menu font has no bar glyph). -1 = a saved value between steps.
// Legacy terrain caps whatever is chosen at 1.3X (hook_helpers.cpp draw_dist_setting).
constexpr int kDrawSteps = 9;
constexpr float kDrawStepValues[kDrawSteps] = { 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 6.0f, 8.0f };

int draw_step()
{

    const float d = rs64::video::draw_distance();

    for (int i = 0; i < kDrawSteps; i++) {
        if (std::fabs(d - kDrawStepValues[i]) < 0.01f) return i;
    }

    return -1;
}

// The next step above d that the current maximum allows, then the maximum itself (legacy terrain's 1.3X), wrapping to 1X.
float next_draw_step(float d, float max)
{

    for (float v : kDrawStepValues) {
        if (v > d + 0.01f && v <= max + 0.01f) return v;
    }

    return d < max - 0.01f ? max : kDrawStepValues[0];
}

std::string draw_step_text(int i)
{

    static const char* const kText[kDrawSteps] = { "1X", "1.25X", "1.5X", "2X", "2.5X", "3X", "4X", "6X", "8X" };

    if (i >= 0) return kText[i];

    char buf[16];

    std::snprintf(buf, sizeof(buf), "%.3gX", rs64::video::draw_distance());

    return buf;
}

// hud_placement in quarters from the 4:3 spots (OFF) to the screen edges (FULL); the menu font has no '%'. -1 = a saved value between steps (CUSTOM).
constexpr int kHudSteps = 5;

int hud_step()
{

    const float p = rs64::video::hud_placement() * (kHudSteps - 1);
    const int i = (int)std::lround(p);

    return std::fabs(p - (float)i) < 0.01f ? i : -1;
}

std::string hud_step_text(int i)
{

    static const char* const kText[kHudSteps] = { "OFF", "1/4", "1/2", "3/4", "FULL" };

    return (i >= 0 && i < kHudSteps) ? kText[i] : "CUSTOM";
}

std::unordered_map<std::string, CycleImpl>& cycles() {

    static std::unordered_map<std::string, CycleImpl> r = {
        { "hud_placement", { [] { return hud_step(); },
                             [] { rs64::video::set_hud_placement((float)((hud_step() + 1) % kHudSteps) / (kHudSteps - 1)); },
                             [](int i) { return hud_step_text(i); } } },
        { "draw_distance", { [] { return draw_step(); },
                             [] { rs64::video::set_draw_distance(next_draw_step(rs64::video::draw_distance(), rs64::video::draw_distance_max())); },
                             [](int i) { return draw_step_text(i); } } },
        { "resolution", display_cycle(rs64::video::Display::Resolution) },
        { "antialiasing", display_cycle(rs64::video::Display::AntiAliasing) },
        { "target_fps", display_cycle(rs64::video::Display::TargetFps) },
        { "texture_filter", display_cycle(rs64::video::Display::TextureFilter) },
        { "upscale_filter", display_cycle(rs64::video::Display::UpscaleFilter) },
    };

    return r;
}

// "config:<mod>:<option>": an Enum option is a toggle (on = any option but the first; selecting it steps to the next option), a Number option a slider (librecomp saves both).
bool config_key(const std::string& key, std::string& mod, std::string& opt) {

    if (key.rfind("config:", 0) != 0) return false;

    const size_t colon = key.find(':', 7);

    if (colon == std::string::npos) return false;

    mod = key.substr(7, colon - 7);
    opt = key.substr(colon + 1);

    return true;
}
const recomp::mods::ConfigOption* config_option(const std::string& mod, const std::string& opt)
{

    const recomp::mods::ConfigSchema& s = recomp::mods::get_mod_config_schema(mod);
    auto it = s.options_by_id.find(opt);

    return it == s.options_by_id.end() ? nullptr : &s.options[it->second];
}
uint32_t config_enum(const std::string& mod, const std::string& opt)
{

    const recomp::mods::ConfigValueVariant v = recomp::mods::get_mod_config_value(mod, opt);
    const uint32_t* u = std::get_if<uint32_t>(&v);

    return u ? *u : 0;
}
double config_number(const std::string& mod, const std::string& opt)
{

    const recomp::mods::ConfigValueVariant v = recomp::mods::get_mod_config_value(mod, opt);
    const double* d = std::get_if<double>(&v);

    return d ? *d : 0.0;
}

void fire_action(const std::string& key) {

    auto it = actions().find(key);

    if (it != actions().end() && it->second) {
        it->second();

        return;
    }

    rs64::host::run_action(key.c_str());
}
bool toggle_state(const std::string& key) {

    std::string mod, opt;

    if (config_key(key, mod, opt)) return config_enum(mod, opt) != 0;

    auto it = toggles().find(key);

    return it != toggles().end() && it->second.get && it->second.get();
}
void toggle_flip(const std::string& key) {

    std::string mod, opt;

    if (config_key(key, mod, opt))
    {
        const recomp::mods::ConfigOption* o = config_option(mod, opt);
        const auto* e = o ? std::get_if<recomp::mods::ConfigOptionEnum>(&o->variant) : nullptr;

        if (e && !e->options.empty()) recomp::mods::set_mod_config_value(mod, opt, (uint32_t)((config_enum(mod, opt) + 1) % e->options.size()));

        return;
    }

    auto ci = cycles().find(key);

    if (ci != cycles().end())
    {
        ci->second.next();

        return;
    }

    auto it = toggles().find(key);

    if (it != toggles().end() && it->second.toggle) it->second.toggle();
}

// A zeroed ctx on a stack of its own, so a .nrm export (recompiled MIPS) can call game functions as safely as a native one; marshaling via r2 (bool/int return) and r4 (int arg).
recomp_context export_ctx(uint8_t* rdram) {

    static uint32_t stack_top = 0;

    if (!stack_top) {
        const uint32_t base = rs64::host::api()->alloc(rdram, 0x4000);
        stack_top = base ? base + 0x4000 - 0x10 : 0;
    }

    recomp_context ctx{};
    ctx.r29 = (gpr)(int32_t)stack_top;

    return ctx;
}
void call_export(uint8_t* rdram, recomp_func_t* fn) {

    if (!fn) return;

    recomp_context ctx = export_ctx(rdram);
    fn(rdram, &ctx);
}
bool call_export_bool(uint8_t* rdram, recomp_func_t* fn) {

    if (!fn) return false;

    recomp_context ctx = export_ctx(rdram);
    fn(rdram, &ctx);

    return ctx.r2 != 0;
}

// Resolve a mod native-library export by name, cached once found. Lazy (at menu-show time): config() is built at boot, before mod native libraries load.
recomp_func_t* resolve_export(const std::string& mod_id, const std::string& name) {

    if (mod_id.empty() || name.empty()) return nullptr;

    static std::unordered_map<std::string, recomp_func_t*> cache;
    std::string key = mod_id + "/" + name;
    auto it = cache.find(key);

    if (it != cache.end()) return it->second;

    recomp_func_t* fn = recomp::mods::get_mod_export(mod_id, name);

    // Only cache successes; retry until the lib loads.
    if (fn) cache[key] = fn;

    return fn;
}

// Fill a button's behavior function pointers from its @export name (or leave them
// null for built-in behaviors, which resolve by key against the registries).
BehaviorFns resolve_behavior(const std::string& mod_id, BType type, const std::string& behavior)
{

    BehaviorFns f;

    if (behavior.size() < 2 || behavior[0] != '@') return f;

    std::string name = behavior.substr(1);

    if (type == BType::Action) f.action = resolve_export(mod_id, name);
    else { f.toggle_set = resolve_export(mod_id, name); f.toggle_get = resolve_export(mod_id, name + "_get"); }

    return f;
}

// ---- rdram accessors + string alloc ----


// HIGH RESOLUTION is bit 0 of gGameSettings+0x10 (byte 0x80130B53). Per-frame readers use it mid-mission, so a pause-menu flip waits for the next screen setup
// (applyLevelScreenAndCameraSettings, hook rs64_menu_hires_apply); either way the next menu pass saves the settings, as the native toggle does.
constexpr uint32_t HIRES_BYTE = 0x130B53;
uint8_t* s_rdram = nullptr;
int s_hires_pending = -1;
bool s_hires_save = false;

// Set while the in-mission pause menu is open (rs64_pause_install); 0x8010CA20 is mission-overlay memory and holds other data in the front end.
bool s_in_pause = false;

bool in_pause_menu() { return s_in_pause; }

bool hires_get()
{

    if (s_hires_pending >= 0) return s_hires_pending != 0;

    return s_rdram && (rd_b(s_rdram, HIRES_BYTE) & 1) != 0;
}

void hires_flip()
{

    if (!s_rdram) return;

    const bool on = !hires_get();

    if (in_pause_menu()) {
        s_hires_pending = on ? 1 : 0;
    } else {
        wr_b(s_rdram, HIRES_BYTE, (uint8_t)((rd_b(s_rdram, HIRES_BYTE) & ~1u) | (on ? 1u : 0u)));
        s_hires_save = true;
    }
}

uint32_t alloc_str(uint8_t* rdram, const std::string& s)
{

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

// ---- Front-end menu entry slots ----

Slot read_slot(uint8_t* r, int i)
{

    return { rd_w(r, GCMD_OFF + kRowLabel + i * 4), rd_b(r, GCMD_OFF + kRowType + i),
             rd_w(r, GCMD_OFF + kRowParam + i * 4), rd_h(r, GCMD_OFF + kRowPos + i * 4),
             rd_h(r, GCMD_OFF + kRowPos + 2 + i * 4), rd_w(r, GCMD_OFF + kRowScale + i * 4) };
}
void write_slot(uint8_t* r, int i, const Slot& s) {

    wr_w(r, GCMD_OFF + kRowLabel + i * 4, s.label);
    wr_b(r, GCMD_OFF + kRowType + i,      s.sub);
    wr_w(r, GCMD_OFF + kRowParam + i * 4, s.param);
    wr_h(r, GCMD_OFF + kRowPos + i * 4,   s.x);
    wr_h(r, GCMD_OFF + kRowPos + 2 + i * 4, s.y);
    wr_w(r, GCMD_OFF + kRowScale + i * 4, s.scaler);
}
bool label_is_empty(uint8_t* r, uint32_t label_vaddr)
{

    // Labels can live in host RAM above the game's 8 MB (language mod text, mod button labels), anywhere in the 512 MB RDRAM.
    if (label_vaddr < 0x80000000u || label_vaddr >= 0xA0000000u) return true;

    return rd_b(r, label_vaddr - 0x80000000u) == 0;
}

// ---- Menu-name resolution + native aliases (for ordering anchors / replace / hide) ----

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

using rs64::menu::NativeAlias;

const NativeAlias* native_alias_of(uint8_t menu_id, const std::string& name)
{

    const char* menu = frontend_menu_name(menu_id);

    return menu ? rs64::menu::native_alias(menu, name) : nullptr;
}

// ---- Per-slot binding, read by the confirm intercept ----

SlotBinding s_slot[MAX_ENTRIES];

// The current page's line per selectable slot (-1: not selectable); set only when the page has a shared row.
int s_slot_line[MAX_ENTRIES];
bool s_page_rows = false;

// The last built page's note label (a copy: the config can be rebuilt) and that page's menu id; s_note_text is what is drawn.
std::optional<Button> s_note;
uint8_t s_note_menu = 0;
std::string s_note_text;

// Whether a mod page is on screen, for the touch code on the SDL thread.
std::atomic<bool> s_page_shown{false};

constexpr size_t LIVE_CAP = 40;

// One buffer per button id, kept for the session.
uint32_t live_buffer(uint8_t* rdram, const std::string& id) {

    static std::unordered_map<std::string, uint32_t> bufs;
    auto it = bufs.find(id);

    if (it != bufs.end()) return it->second;

    void* p = recomp::alloc(rdram, LIVE_CAP + 1);

    if (!p) return 0;

    const uint32_t v = (uint32_t)((uint8_t*)p - rdram) + 0x80000000u;
    bufs.emplace(id, v);

    return v;
}

void write_live(uint8_t* rdram, uint32_t vaddr, const std::string& s)
{

    const size_t n = std::min(s.size(), LIVE_CAP);
    const uint32_t off = vaddr - 0x80000000u;

    for (size_t i = 0; i < n; ++i) rdram[(off + (uint32_t)i) ^ 3] = (uint8_t)s[i];

    rdram[(off + (uint32_t)n) ^ 3] = 0;
}

// Custom mod pages get their own menu ids from 13 (setupMenuData skips its per-menu builders for them; the build hook supplies the entry count).
// A "submenu" button opens a named page; buttons with menu==<page> are its entries. B and BACK return to the menu that opened the page (+0x05), so pages nest.

const std::vector<std::string>& page_names()
{

    static const std::vector<std::string> names = [] {

        std::vector<std::string> v;

        // A submenu whose page names a front-end menu (controller_settings) opens that menu, not a page.
        for (const auto& b : config().buttons)
            if (!b.page.empty() && frontend_menu_id(b.page) == MENU_NONE && std::find(v.begin(), v.end(), b.page) == v.end()) v.push_back(b.page);

        return v;
    }();

    return names;
}

uint8_t page_id(const std::string& page)
{

    const auto& v = page_names();
    const auto it = std::find(v.begin(), v.end(), page);

    if (it == v.end() || (it - v.begin()) + FIRST_PAGE_ID > 0xFE) return 0xFF;

    return (uint8_t)(FIRST_PAGE_ID + (it - v.begin()));
}

const std::string* page_name(uint8_t id)
{

    const auto& v = page_names();

    if (id < FIRST_PAGE_ID || id - FIRST_PAGE_ID >= (int)v.size()) return nullptr;

    return &v[id - FIRST_PAGE_ID];
}

// Menu each page was opened from (Back's target); the page last built, for leave_action.
std::unordered_map<uint8_t, uint8_t> s_page_parent_of;
uint8_t s_shown_page = 0xFF;

uint8_t page_parent(uint8_t id)
{

    auto it = s_page_parent_of.find(id);

    return it != s_page_parent_of.end() ? it->second : MENU_MAIN;
}

// The page and its ancestor pages, innermost first.
std::vector<uint8_t> page_chain(uint8_t id)
{

    std::vector<uint8_t> v;

    while (page_name(id) && std::find(v.begin(), v.end(), id) == v.end())
    {
        v.push_back(id);
        id = page_parent(id);
    }

    return v;
}

// A pilot-first page (action with transition "account"): armed on confirm, shown when the pilot is chosen, left when its exit condition holds.
std::string s_pilot_page, s_pilot_exit;

// In-place game_settings toggles reuse a native toggle slot (sub-type 5) so the game re-renders in place: each `replace`s a native toggle, taking its param and ON/OFF textIds,
// which are overridden with the mod's labels. Only slots with known textIds are supported.
struct InplaceToggle { int param; unsigned off_tid, on_tid; std::string on, off, key; recomp_func_t* get = nullptr; };
std::vector<InplaceToggle> s_inplace_tgls;

// Map a game_settings `replace` alias to its native toggle {param, OFF tid, ON tid}.
bool inplace_slot(const std::string& alias, int& param, unsigned& off_tid, unsigned& on_tid)
{

    if (alias == "crosshairs") { param = 3; off_tid = 0x6C; on_tid = 0x6D; return true; }

    // Other slots: probe their textIds before adding.
    return false;
}

// A confirm prompt: Pending from the entry's press, on the menu it was pressed on; shown once that menu is rebuilt as YES / NO.
enum class ConfirmState { None, Pending };
ConfirmState s_confirm_state = ConfirmState::None;
uint8_t s_confirm_menu = 0;
bool s_confirm_shown = false;
std::string s_confirm_action, s_confirm_prompt;
recomp_func_t* s_confirm_fn = nullptr;

// Toggle state / flip for a button, preferring its mod @export over the built-in.
bool button_toggle_state(uint8_t* rdram, const std::string& key, recomp_func_t* get_fn) {
    return get_fn ? call_export_bool(rdram, get_fn) : toggle_state(key);
}

// Menu font 5 text: capitals, digits and spaces (no lower case, no punctuation).
std::string menu_font_text(const std::string& s) {

    std::string out;

    for (char c : s)
    {
        if (std::isalnum((unsigned char)c) || c == ' ') out.push_back((char)std::toupper((unsigned char)c));
    }

    return out;
}

// A toggle's label: a config Enum with more than two options (or no label_on/label_off) shows "label: OPTION", else label_on or label_off by state.
std::string toggle_text(uint8_t* rdram, const std::string& key, recomp_func_t* get_fn, const std::string& label, const std::string& on, const std::string& off)
{

    std::string mod, opt;
    auto ci = cycles().find(key);

    // A cycle shows its choice: the button's labels override (per language), else the built-in text.
    if (!get_fn && ci != cycles().end())
    {
        const int v = ci->second.get();
        std::string name = ci->second.text(v);

        for (const auto& b : config().buttons)
        {
            if (b.behavior != key) continue;
            if (v >= 0 && v < (int)b.value_labels.size() && !b.value_labels[v].empty()) name = b.value_labels[v];
            if (v < 0 && !b.label_other.empty()) name = b.label_other;

            break;
        }

        return label.empty() ? name : label + ": " + name;
    }

    if (!get_fn && config_key(key, mod, opt))
    {
        const recomp::mods::ConfigOption* o = config_option(mod, opt);
        const auto* e = o ? std::get_if<recomp::mods::ConfigOptionEnum>(&o->variant) : nullptr;

        if (e && !e->options.empty() && (e->options.size() > 2 || (on.empty() && off.empty())))
        {
            uint32_t v = config_enum(mod, opt);

            if (v >= e->options.size()) v = 0;

            const std::string name = menu_font_text(e->options[v]);

            return label.empty() ? name : label + ": " + name;
        }
    }

    return button_toggle_state(rdram, key, get_fn) ? on : off;
}

// Slider value get/set, preferring the mod @export (get -> r2, set arg -> r4).
int slider_value(uint8_t* rdram, const std::string& key, recomp_func_t* get_fn)
{

    if (get_fn) { recomp_context c = export_ctx(rdram); get_fn(rdram, &c); return (int)(int32_t)c.r2; }

    std::string mod, opt;

    if (config_key(key, mod, opt)) return (int)std::lround(config_number(mod, opt));

    auto it = sliders().find(key);

    return (it != sliders().end() && it->second.get) ? it->second.get() : 0;
}
void slider_apply(uint8_t* rdram, const std::string& key, recomp_func_t* set_fn, int v)
{

    if (set_fn) { recomp_context c = export_ctx(rdram); c.r4 = (gpr)(int32_t)v; set_fn(rdram, &c); return; }

    std::string mod, opt;

    if (config_key(key, mod, opt)) {
        recomp::mods::set_mod_config_value(mod, opt, (double)v);

        return;
    }

    auto it = sliders().find(key);

    if (it != sliders().end() && it->second.set) it->second.set(v);
}

// "LABEL |||||....." — a font-glyph bar (via the same text pipeline as any label).
std::string slider_bar_text(const std::string& label, int value, int vmin, int vmax,
                            int width, const std::string& fch, const std::string& ech)
{

    if (vmax <= vmin) vmax = vmin + 1;
    if (width < 1) width = 1;

    long filled = (long)((double)(value - vmin) / (vmax - vmin) * width + 0.5);

    if (filled < 0) filled = 0;
    if (filled > width) filled = width;

    std::string bar;

    for (int i = 0; i < width; ++i) bar += (i < filled) ? fch : ech;

    if (label.empty()) return bar;

    // The game centres the whole string, so blank empty cells would push the label off centre.
    std::string text = label + " " + bar;

    text.erase(text.find_last_not_of(' ') + 1);

    return text;
}

// Step a slider value on confirm: +step, wrapping max -> min.
int slider_next(int value, int vmin, int vmax, int step) {

    int v = value + step;

    if (v > vmax) v = vmin;
    if (v < vmin) v = vmin;

    return v;
}

void set_bar_layout(SlotBinding* s, const Button& b) {

    s->bar_label = b.label;
    s->bar_width = b.bar_width;
    s->bar_filled = b.bar_filled;
    s->bar_empty = b.bar_empty;
}

// Value for a click at fraction f across a text slider's "LABEL |||..." line; -1 when the click is on the label text.
// Measured at the minimum value: exact when the filled and empty glyph strings have the same length (the defaults do).
int slider_value_at_click(const std::string& label, int vmin, int vmax, int step, int width,
                          const std::string& fch, const std::string& ech, float f)
{

    const std::string text = slider_bar_text(label, vmin, vmin, vmax, width, fch, ech);
    const float start = label.empty() ? 0.0f : (float)label.size() + 0.5f;
    const float pos = f * (float)text.size();

    if (pos < start || (float)text.size() <= start || vmax <= vmin) return -1;

    const float t = std::clamp((pos - start) / ((float)text.size() - start), 0.0f, 1.0f);
    const int q = step > 0 ? step : 1;
    const int v = vmin + (int)std::lround(t * (float)(vmax - vmin) / (float)q) * q;

    return std::clamp(v, vmin, vmax);
}

// Last tap/click on a menu entry (rs64_menu_slider_click); consumed by the next slider confirm.
std::atomic<int> s_click_key{-1};
std::atomic<float> s_click_frac{0.0f};
std::atomic<uint32_t> s_click_ms{0};

int confirmed_slider_value(int key, int value, int vmin, int vmax, int step, const std::string& label, int width,
                           const std::string& fch, const std::string& ech)
{

    const float f = s_click_frac.load();
    const bool fresh = SDL_GetTicks() - s_click_ms.load() < 1000;
    const int v = (s_click_key.exchange(-1) == key && fresh) ? slider_value_at_click(label, vmin, vmax, step, width, fch, ech, f) : -1;

    return v >= 0 ? v : slider_next(value, vmin, vmax, step);
}

// One `requires` condition: built-in names first, then a mod config option (set = not its first choice, or non-zero), then the host API's registered conditions.
// front_end is false while the pause menu is built.
bool condition_holds(uint8_t* rdram, const std::string& c, bool pause)
{

    if (!c.empty() && c[0] == '!') return !condition_holds(rdram, c.substr(1), pause);

    std::string mod, opt;

    if (config_key(c, mod, opt))
    {
        const recomp::mods::ConfigValueVariant v = recomp::mods::get_mod_config_value(mod, opt);

        if (const uint32_t* u = std::get_if<uint32_t>(&v)) return *u != 0;
        if (const double* d = std::get_if<double>(&v)) return *d != 0.0;
        if (const std::string* s = std::get_if<std::string>(&v)) return !s->empty();

        return false;
    }

#ifdef __ANDROID__
    if (c == "desktop") return false;
#else
    if (c == "desktop") return true;
#endif

    if (c == "front_end") return !pause;
    if (c == "ray_query") return rs64::video::ray_query_supported();
    if (c == "expansion_pak") return rd_b(rdram, 0x130B63) != 0;
    if (c == "frame_interpolation") return rs64::video::frame_interpolation_on();
    if (c == "widescreen") return rs64::video::display_value(rs64::video::Display::Widescreen) == 1;

    return rs64::host::condition(c.c_str()) > 0;
}

// A behavior key some button names in `requires` (frame_interpolation): flipping it changes which lines a menu shows.
bool used_as_condition(const std::string& key)
{

    for (const auto& b : config().buttons)
        for (const auto& c : b.requires_conds)
            if (c == key || (c.size() == key.size() + 1 && c[0] == '!' && c.compare(1, std::string::npos, key) == 0)) return true;

    return false;
}

bool button_visible(uint8_t* rdram, const Button& b, bool pause)
{

    for (const auto& c : b.requires_conds)
        if (!condition_holds(rdram, c, pause)) return false;

    return true;
}

// One item in a menu's merged entry list (native or synthesized mod button).
struct Item {
    double order;
    Slot slot;
    bool is_native;
    bool is_spacer;
    SlotBinding binding;
    int index_seq = -1;
    int index = 0;
    std::string id;
};

// Move each indexed mod item (index_seq = its config order) to its `index` among the visible items.
void apply_indices(std::vector<Item>& items)
{

    for (int k = 0; ; ++k)
    {
        auto it = std::find_if(items.begin(), items.end(), [k](const Item& i) { return i.index_seq == k; });

        if (it == items.end()) return;

        const int to = it->index;

        rs64::menu::move_to_index(items, (size_t)(it - items.begin()), to);
    }
}

bool alias_matches(const Slot& s, const NativeAlias& a) {
    return s.sub == a.sub && (!a.match_param || s.param == a.param);
}

// Find the order of a native/button anchor by name, for before/after resolution.
bool anchor_order(const std::vector<Item>& items, uint8_t menu_id,
                  const std::string& name, double& out)
{

    const NativeAlias* na = native_alias_of(menu_id, name);

    // A mod button anchors by its id; its behavior key is still accepted for older menu files.
    for (const auto& it : items)
    {
        if (it.is_native && na && alias_matches(it.slot, *na))                 { out = it.order; return true; }
        if (!it.is_native && (it.id == name || it.binding.key == name))        { out = it.order; return true; }
    }

    return false;
}

// A title override for this menu or page, written after the game (or the page builder) set its own.
void apply_title(uint8_t* rdram, const std::string& menu)
{

    const auto it = config().titles.find(menu);

    if (it == config().titles.end()) return;

    if (!it->second.text.empty()) wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, it->second.text));
    if (it->second.has_x) wr_h(rdram, GCMD_OFF + 0x50, (int16_t)it->second.x);
    if (it->second.has_y) wr_h(rdram, GCMD_OFF + 0x52, (int16_t)it->second.y);
}

// Rebuild the current front-end menu's entry list: native entries merged with the mod buttons by order/anchors, dropping hidden/replaced natives and spacers to fit MAX_ENTRIES.
void rebuild_frontend_menu(uint8_t* rdram, uint8_t menu_id)
{

    const char* mname = frontend_menu_name(menu_id);

    if (!mname) return;

    // Re-detected below.
    if (menu_id == MENU_GAME_SETTINGS) s_inplace_tgls.clear();

    const uint8_t n = rd_b(rdram, GCMD_OFF + 0x95);
    int32_t ref_scaler = n > 0 ? read_slot(rdram, 0).scaler : 0x3F800000;

    std::vector<Item> items;

    for (uint8_t i = 0; i < n && i < MAX_ENTRIES; ++i) {
        Slot s = read_slot(rdram, i);
        Item it;
        it.order = i * 10.0;
        it.slot = s;
        it.is_native = true;
        it.is_spacer = label_is_empty(rdram, (uint32_t)s.label);
        items.push_back(it);
    }

    // hides for this menu
    auto hide_it = config().hides.find(mname);

    if (hide_it != config().hides.end()) {
        for (const auto& alias : hide_it->second) {
            const NativeAlias* na = native_alias_of(menu_id, alias);

            if (!na) continue;

            for (auto vi = items.begin(); vi != items.end(); )
            {
                if (vi->is_native && alias_matches(vi->slot, *na)) vi = items.erase(vi);
                else ++vi;
            }
        }
    }

    int index_seq = 0;

    // merge mod buttons for this menu
    for (const auto& b : config().buttons)
    {
        if (frontend_menu_id(b.menu) != menu_id) continue;
        if (!button_visible(rdram, b)) continue;

        // Sound Settings sliders are placed natively by sound_settings_install.
        if (menu_id == MENU_SOUND_SETTINGS && b.type == BType::Slider) continue;

        // replace: drop the matching native and take its order
        double ord;
        bool have_ord = false;

        if (!b.replace.empty()) {
            const NativeAlias* na = native_alias_of(menu_id, b.replace);

            if (na) {
                for (auto vi = items.begin(); vi != items.end(); ) {
                    if (vi->is_native && alias_matches(vi->slot, *na)) {
                        ord = vi->order;
                        have_ord = true;
                        vi = items.erase(vi);
                    } else ++vi;
                }
            }
        }

        if (!have_ord && !b.after.empty()  && anchor_order(items, menu_id, b.after,  ord)) { ord += 5;  have_ord = true; }
        if (!have_ord && !b.before.empty() && anchor_order(items, menu_id, b.before, ord)) { ord -= 5;  have_ord = true; }
        if (!have_ord && b.has_order) { ord = b.order; have_ord = true; }
        if (!have_ord) { double mx = 0; for (auto& it : items) mx = std::max(mx, it.order); ord = mx + 10; }

        // Submenu opener: sub-type 1 to the page id; the binding carries the page.
        if (b.type == BType::Submenu)
        {
            uint32_t lbl = alloc_str(rdram, b.label);

            if (!lbl) continue;

            Item it;
            it.order = ord;
            it.is_native = false;
            it.is_spacer = false;
            it.slot = Slot{ (int32_t)lbl, SUBTYPE_SUBMENU, (int32_t)page_id(b.page),
                            (int16_t)b.x, (int16_t)b.y, ref_scaler };
            it.binding = { true, BType::Submenu, b.page };
            it.id = b.id;
            it.binding.open_action = b.behavior;
            it.binding.sound = b.sound;
            it.index_seq = b.has_index ? index_seq++ : -1;
            it.index = b.index;
            items.push_back(it);
            continue;
        }

        // synthesize the slot
        BehaviorFns fns = resolve_behavior(b.mod_id, b.type, b.behavior);
        std::string text;

        if (b.type == BType::Toggle)
            text = toggle_text(rdram, b.behavior, fns.toggle_get, b.label, b.label_on, b.label_off);
        else if (b.type == BType::Slider)
            text = slider_bar_text(b.label, slider_value(rdram, b.behavior, fns.toggle_get),
                                   b.smin, b.smax, b.bar_width, b.bar_filled, b.bar_empty);
        else
            text = b.label;

        uint32_t lbl = alloc_str(rdram, text);

        if (!lbl) continue;

        // Toggle/Slider + confirm-action = self-targeting sub-type 1 (rebuild reflects the new state / rides the YES-NO transition); plain action = no-op host.
        // In native game_settings a toggle is emitted as native sub-type 5 (param = game-settings toggle index) so the game's own handler re-renders it in place.
        uint8_t sub;
        int32_t param;
        int ip_param;
        unsigned ip_off, ip_on;

        if (b.type == BType::Toggle && menu_id == MENU_GAME_SETTINGS
            && !b.replace.empty() && inplace_slot(b.replace, ip_param, ip_off, ip_on))
        {
            // Native in-place re-render on the replaced slot.
            sub = 5;
            param = ip_param;
            s_inplace_tgls.push_back({ ip_param, ip_off, ip_on, b.label_on, b.label_off,
                                       b.behavior, fns.toggle_get });
        } else {
            bool selfsub = (b.type == BType::Slider) || !b.confirm.empty() || b.type == BType::Toggle;
            sub = selfsub ? SUBTYPE_SUBMENU : SUBTYPE_HOST;
            param = selfsub ? (int32_t)menu_id : 0;

            if (b.type == BType::Action && b.confirm.empty() && b.transition == "account")
            {
                sub = SUBTYPE_SUBMENU;
                param = MENU_ACCOUNT;
            }

            if (b.type == BType::Action && b.confirm.empty() && b.transition == "language")
            {
                sub = SUBTYPE_SUBMENU;
                param = MENU_LANGUAGE;
            }

            if (b.type == BType::Action && b.confirm.empty() && b.transition.rfind("language:", 0) == 0)
            {
                sub = SUBTYPE_LANGUAGE;
                param = std::atoi(b.transition.c_str() + 9);
            }
        }

        Slot s{ (int32_t)lbl, sub, param, (int16_t)b.x, (int16_t)b.y, ref_scaler };
        Item it;
        it.order = ord;
        it.slot = s;
        it.is_native = false;
        it.is_spacer = false;
        it.binding = { true, b.type, b.behavior, b.confirm, fns.action, fns.toggle_get, fns.toggle_set,
                       b.smin, b.smax, b.sstep, b.label_on, b.label_off };
        set_bar_layout(&it.binding, b);
        it.binding.sound = b.sound;
        it.id = b.id;
        it.index_seq = b.has_index ? index_seq++ : -1;
        it.index = b.index;

        if (b.transition == "account") {
            it.binding.pilot_page = b.page;
            it.binding.exit_when = b.exit_when;
        }

        items.push_back(it);
    }

    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b){ return a.order < b.order; });

    // trim to MAX_ENTRIES: drop spacers first, then trailing
    auto count_kept = [&]{ return (int)items.size(); };

    for (auto vi = items.begin(); vi != items.end() && count_kept() > MAX_ENTRIES; )
    {
        if (!vi->is_spacer)
        {
            ++vi;
            continue;
        }

        // The game pulls the natives below a spacer up to shorten its gap (OPTIONS: y -10); with the spacer row gone that overlaps the entry above, so undo it.
        auto above = std::find_if(std::make_reverse_iterator(vi), items.rend(), [](const Item& i) { return i.is_native; });
        auto below = std::find_if(std::next(vi), items.end(), [](const Item& i) { return i.is_native; });
        const int lift = (below != items.end() && above != items.rend()) ? below->slot.y - above->slot.y : 0;

        vi = items.erase(vi);

        for (auto t = vi; t != items.end(); ++t)
            if (t->is_native) t->slot.y = (int16_t)(t->slot.y - lift);
    }

    apply_indices(items);

    if ((int)items.size() > MAX_ENTRIES)
    {
        std::string dropped;

        for (size_t i = MAX_ENTRIES; i < items.size(); ++i)
        {
            const Item& it = items[i];
            const std::string& name = it.id.empty() ? it.binding.key : it.id;

            dropped += " " + (it.is_native ? std::string("(the game's)") : name);
        }

        fprintf(stderr, "[menu] %s holds %d entries; dropped:%s\n", mname, MAX_ENTRIES, dropped.c_str());
        items.resize(MAX_ENTRIES);
    }

    // The game opens OPTIONS' gap with a y offset on every native below it; a mod entry there with no y of its own (a replacement, an appended one) takes the offset of the native above, or it lands a row up on that native.
    int16_t native_y = 0;

    for (auto& it : items) {
        if (it.is_native) native_y = it.slot.y;
        else if (it.slot.y == 0) it.slot.y = native_y;
    }

    // write back
    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    // The main menu's list grows down from under the logo and fits three entries: each one past that lifts it a line (36) so the last stays on screen.
    const int lift = (menu_id == MENU_MAIN && (int)items.size() > 3) ? 36 * ((int)std::min<size_t>(items.size(), MAX_ENTRIES) - 3) : 0;
    int j = 0;

    for (auto& it : items) {
        if (j >= MAX_ENTRIES) break;

        it.slot.y = (int16_t)(it.slot.y - lift);
        write_slot(rdram, j, it.slot);

        if (!it.is_native) s_slot[j] = it.binding;

        ++j;
    }

    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)j);
    apply_title(rdram, mname);
}

// Sound Settings native slider: a mod slider on `sound_settings` with a `replace` anchor becomes a volume-slider entry (sub-type 21) whose channel index is redirected to an unused settings byte,
// so the value is independent of audio; endpoint labels and the value (synced to the mod's @export) are overridden.


// The native sliders of the menu last built (Sound Settings or a mod page); the slider edit state works on any menu's highlighted row.
std::vector<SndSlider> s_snd_sliders;
uint8_t s_snd_menu = MENU_NONE;

bool snd_sliders_here(uint8_t* rdram) {
    return !s_snd_sliders.empty() && rd_b(rdram, GCMD_OFF + 0x04) == s_snd_menu;
}

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

// `top` is the slider's full scale: 0x80 in the front end, 0x7F in the pause menu.
int byte_from_value(int v, int lo, int hi, int top) {

    if (hi <= lo) return 0;

    long b = (long)(v - lo) * top / (hi - lo);

    return b < 0 ? 0 : (b > top ? top : (int)b);
}
int value_from_byte(int b, int lo, int hi, int top) {
    return hi <= lo ? lo : lo + (int)((long)std::min(b, top) * (hi - lo) / top);
}

// A mod slider at `slot` as a native volume slider on the next free channel byte, seeded from its value; false when the channels are used up.
bool add_snd_slider(uint8_t* rdram, const Button& b, int slot)
{

    const int ch = SND_CH_FIRST + (int)s_snd_sliders.size();

    if (ch > SND_CH_LAST) return false;

    const BehaviorFns fns = resolve_behavior(b.mod_id, b.type, b.behavior);
    SndSlider ss;

    ss.slot = slot;
    ss.channel = ch;
    ss.smin = b.smin;
    ss.smax = b.smax;
    ss.off_label = b.label_off.empty() ? "OFF" : b.label_off;
    ss.on_label  = b.label_on.empty()  ? "MAX" : b.label_on;
    ss.key = b.behavior;
    ss.get_fn = fns.toggle_get;
    ss.set_fn = fns.toggle_set;
    wr_b(rdram, VOL_BYTES + ch, (uint8_t)byte_from_value(slider_value(rdram, b.behavior, fns.toggle_get), b.smin, b.smax));
    s_snd_sliders.push_back(std::move(ss));

    return true;
}

// Install mod sliders into Sound Settings (called from the setupMenuData tail).
void sound_settings_install(uint8_t* rdram) {

    s_snd_sliders.clear();
    s_snd_menu = MENU_SOUND_SETTINGS;

    for (const auto& b : config().buttons)
    {
        if (b.type != BType::Slider || frontend_menu_id(b.menu) != MENU_SOUND_SETTINGS) continue;

        // Needs a replace anchor.
        const int slot = b.replace.empty() ? -1 : snd_alias_slot(rdram, b.replace);

        if (slot < 0 || !add_snd_slider(rdram, b, slot)) continue;

        Slot s = read_slot(rdram, slot);
        s.label = (int32_t)alloc_str(rdram, b.label);
        s.sub = SND_SLIDER_SUB;
        write_slot(rdram, slot, s);
    }
}

const SndSlider* snd_slider_for_slot(int slot) {

    for (const auto& ss : s_snd_sliders) if (ss.slot == slot) return &ss;

    return nullptr;
}

// Build a custom mod page (menu id 13+): title from the opening submenu button, entries from buttons whose menu == the page name, plus a Back to the parent (slot reserved, capped at MAX_ENTRIES).
int build_mod_page(uint8_t* rdram, const std::string& page, uint8_t parent)
{

    const uint8_t self_id = page_id(page);

    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    s_snd_sliders.clear();
    s_snd_menu = self_id;

    const int32_t sc = 0x3F800000;

    std::string title = page;

    for (const auto& b : config().buttons)
        if (b.page == page && b.menu != page) {
            title = b.title.empty() ? b.label : b.title;

            if (b.title_y != 0) wr_h(rdram, GCMD_OFF + 0x52, (int16_t)b.title_y);
            break;
        }

    wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, title));
    apply_title(rdram, page);

    std::vector<const Button*> lines;

    s_note.reset();
    s_note_menu = self_id;

    for (const auto& b : config().buttons)
        if (b.menu == page && button_visible(rdram, b)) {
            if (!b.note) lines.push_back(&b);
            else if (!s_note) s_note = b;
        }

    for (const auto& b : config().buttons)
        if (b.menu == page && b.has_index && button_visible(rdram, b))
        {
            const size_t from = (size_t)(std::find(lines.begin(), lines.end(), &b) - lines.begin());

            rs64::menu::move_to_index(lines, from, b.index);
        }

    // Reserve a slot for Back.
    if ((int)lines.size() > MAX_ENTRIES - 1)
    {
        std::string dropped;

        for (size_t i = MAX_ENTRIES - 1; i < lines.size(); ++i)
        {
            dropped += " " + (lines[i]->id.empty() ? lines[i]->label : lines[i]->id);
        }

        fprintf(stderr, "[menu] page %s holds %d entries and BACK; dropped:%s\n", page.c_str(), MAX_ENTRIES - 1, dropped.c_str());
        lines.resize(MAX_ENTRIES - 1);
    }

    // Selectable lines take the first slots with BACK after them (a leading non-selectable entry hangs the menu navigation); labels take the slots after BACK.
    // Every line is then moved (its y offset; slot i is drawn 36 * i down) to the place its JSON order gives it, BACK last.
    std::vector<int> slot_of(lines.size());
    int next = 0;

    for (size_t v = 0; v < lines.size(); ++v)
        if (lines[v]->type != BType::Label) slot_of[v] = next++;

    const int back_slot = next++;

    for (size_t v = 0; v < lines.size(); ++v)
        if (lines[v]->type == BType::Label) slot_of[v] = next++;

    int gap = 0;
    int line = -1, line_gap = 0;

    // The game centres a list on its entry count; gaps would push it down, so every line moves up by half of them.
    int lift = 0;

    for (const Button* b : lines) lift += b->gap_after;

    lift /= 2;
    s_page_rows = false;

    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot_line[i] = -1;

    for (size_t v = 0; v < lines.size(); ++v)
    {
        const Button& b = *lines[v];
        const int j = slot_of[v];
        const bool same_row = !b.row.empty() && v > 0 && lines[v - 1]->row == b.row;

        if (!same_row) {
            ++line;
            line_gap = gap;
        }

        s_page_rows = s_page_rows || same_row;

        if (b.type != BType::Label) s_slot_line[j] = line;

        BehaviorFns fns = resolve_behavior(b.mod_id, b.type, b.behavior);
        std::string text;

        // A slider is the game's own volume slider while a channel byte is free (three per menu), else a text bar.
        const bool native_slider = b.type == BType::Slider && b.confirm.empty() && add_snd_slider(rdram, b, j);

        if (b.type == BType::Toggle)
            text = toggle_text(rdram, b.behavior, fns.toggle_get, b.label, b.label_on, b.label_off);
        else if (b.type == BType::Slider && !native_slider)
            text = slider_bar_text(b.label, slider_value(rdram, b.behavior, fns.toggle_get),
                                   b.smin, b.smax, b.bar_width, b.bar_filled, b.bar_empty);
        else
            text = b.label;

        uint32_t live = 0;

        // A toggle, cycle or slider relabels in place each frame (rs64_menu_frame) unless its key is also a `requires` condition, whose change adds or removes lines and needs a rebuild.
        const bool value_entry = (b.type == BType::Toggle || (b.type == BType::Slider && !native_slider)) && b.confirm.empty();
        const bool in_place = value_entry && !used_as_condition(b.behavior);

        if (!b.label_src.empty())
        {
            const std::string src = source_text(b.label_src);

            if (!src.empty()) text = src;
        }

        if (!b.label_src.empty() || in_place)
        {
            live = live_buffer(rdram, b.id.empty() ? page + "#" + std::to_string(j) : b.id);

            if (live) write_live(rdram, live, text);
        }

        uint32_t lbl = live ? live : alloc_str(rdram, text.empty() ? " " : text);

        // Other value entries reload by re-targeting the page itself; a submenu entry opens its own page.
        const bool opener = b.type == BType::Submenu;
        const bool selfsub = (value_entry && !in_place) || !b.confirm.empty();
        const bool language_pick = b.type == BType::Action && b.transition.rfind("language:", 0) == 0;
        const uint8_t sub = native_slider ? SND_SLIDER_SUB : language_pick ? SUBTYPE_LANGUAGE : b.type == BType::Label ? SUBTYPE_LABEL : (selfsub || opener) ? SUBTYPE_SUBMENU : SUBTYPE_HOST;
        const uint8_t native = frontend_menu_id(b.page);
        const int32_t target = native != MENU_NONE ? (int32_t)native : (int32_t)page_id(b.page);
        const int32_t param = language_pick ? std::atoi(b.transition.c_str() + 9) : opener ? target : (b.type != BType::Label && selfsub) ? (int32_t)self_id : 0;
        int32_t scale_bits = sc;

        if (b.scale != 1.0f) std::memcpy(&scale_bits, &b.scale, 4);

        const int y = 36 * (line - j) + line_gap + b.y - lift;
        write_slot(rdram, j, Slot{ (int32_t)lbl, sub, param, (int16_t)b.x, (int16_t)y, scale_bits });
        gap += b.gap_after;
        s_slot[j] = { true, b.type, b.behavior, b.confirm, fns.action, fns.toggle_get, fns.toggle_set,
                      b.smin, b.smax, b.sstep, b.label_on, b.label_off };
        set_bar_layout(&s_slot[j], b);
        s_slot[j].sound = b.sound;
        s_slot[j].left_action = b.left_action;
        s_slot[j].right_action = b.right_action;

        // The game edits a native slider itself; the confirm intercept must not step it.
        if (native_slider) s_slot[j] = {};

        if (opener) {
            s_slot[j].key = b.page;
            s_slot[j].open_action = b.behavior;
        }

        if (live) {
            s_slot[j].label_src = b.label_src;
            s_slot[j].live_text = text;
            s_slot[j].live_buf = live;
        }
    }

    s_slot_line[back_slot] = line + 1;
    const int back_y = 36 * (line + 1 - back_slot) + gap - lift;
    write_slot(rdram, back_slot, Slot{ (int32_t)alloc_str(rdram, back_text()), SUBTYPE_SUBMENU, parent, 0, (int16_t)back_y, sc });
    s_slot[back_slot].sound = kSfxBack;
    wr_b(rdram, GCMD_OFF + 0x95, (uint8_t)(lines.size() + 1));

    // The B button returns to gCurrentMenuData+0x05; a page goes back where BACK does.
    wr_b(rdram, GCMD_OFF + 0x05, parent);

    return (int)lines.size() + 1;
}

// ---- LANGUAGE page with more languages than it has rows: the language slots scroll through every language ----

constexpr int kLangRows = PAGE_ROWS - 1;

struct LangScreen {
    bool shown = false;
    bool dirty = false;
    std::vector<std::pair<std::string, int>> rows;
    rs64::menu::ScrollPos pos{ 0, 0 };
    uint32_t bufs[kLangRows] = {};
};

LangScreen s_lang;

bool langs_on_screen(uint8_t* rdram) {
    return s_lang.shown && rd_b(rdram, GCMD_OFF + 0x04) == page_id(kLanguagesPage);
}

// Visible row i ends in a blank, or '^' / '#' on the top / bottom row with languages above / below (ensure_menu_period's arrows).
std::string lang_slot_text(int i)
{

    std::string t = s_lang.rows[s_lang.pos.top + i].first + " `";
    const bool more_above = i == 0 && s_lang.pos.top > 0;
    const bool more_below = i == kLangRows - 1 && s_lang.pos.top + kLangRows < (int)s_lang.rows.size();

    if (more_above || more_below) t.back() = more_above ? '^' : '#';

    return t;
}

// The page as build_mod_page lays it out (the first kLangRows languages, then BACK); with more languages its slots get live labels the pass rewrites, each slot's param being its language's index.
int languages_install(uint8_t* rdram, uint8_t parent)
{

    const int count = build_mod_page(rdram, kLanguagesPage, parent);

    s_lang = LangScreen{};

    for (const auto& b : config().buttons)
        if (b.menu == kLanguagesPage && b.transition.rfind("language:", 0) == 0 && button_visible(rdram, b))
            s_lang.rows.emplace_back(b.label, std::atoi(b.transition.c_str() + 9));

    if ((int)s_lang.rows.size() <= kLangRows) return count;

    // A mod's own entry on the page would sit among the slots: the page then lists the first rows only.
    for (int i = 0; i < kLangRows; ++i)
    {
        s_lang.bufs[i] = live_buffer(rdram, "languages#" + std::to_string(i));

        if (!s_lang.bufs[i] || read_slot(rdram, i).sub != SUBTYPE_LANGUAGE) return count;
    }

    for (int i = 0; i < kLangRows; ++i)
    {
        Slot s = read_slot(rdram, i);

        write_live(rdram, s_lang.bufs[i], lang_slot_text(i));
        s.label = (int32_t)s_lang.bufs[i];
        write_slot(rdram, i, s);
    }

    s_lang.shown = true;

    fprintf(stderr, "[lang] LANGUAGE page: %zu languages in %d rows\n", s_lang.rows.size(), kLangRows);

    return count;
}

// Each front-end pass before the menu reads the pad: up / down (and the wheel) step the cursor, scrolling the rows at their edges; BACK sits below them.
void languages_pass(uint8_t* rdram, recomp_context* ctx)
{

    if (!langs_on_screen(rdram)) return;

    uint32_t p = (uint32_t)rd_w(rdram, PAD_PRESSED);
    const int wheel = s_ctl_wheel.exchange(0);

    if (wheel) p |= wheel > 0 ? kNavUp : kNavDown;

    if (!(p & (kNavUp | kNavDown))) return;

    wr_w(rdram, PAD_PRESSED, (int32_t)(p & ~(kNavUp | kNavDown)));

    // A mouse or touch hover may have moved the highlight since the last step.
    const int n = (int)s_lang.rows.size(), slot = rd_b(rdram, GCMD_OFF + 0x94), top = s_lang.pos.top;

    const int dir = (p & kNavUp) ? -1 : 1;

    s_lang.pos.sel = slot >= kLangRows ? n : top + slot;

    // The list stops at its ends: wrapping from BACK would jump a long list back to the top.
    if ((dir > 0 && s_lang.pos.sel == n) || (dir < 0 && s_lang.pos.sel == 0)) return;

    s_lang.pos = rs64::menu::scroll_step(s_lang.pos, dir, n, kLangRows);

    if (s_lang.pos.top != top)
    {
        for (int i = 0; i < kLangRows; ++i)
        {
            write_live(rdram, s_lang.bufs[i], lang_slot_text(i));
            wr_w(rdram, GCMD_OFF + kRowParam + i * 4, s_lang.rows[s_lang.pos.top + i].second);
        }

        s_lang.dirty = true;
    }

    wr_b(rdram, GCMD_OFF + 0x94, (uint8_t)(s_lang.pos.sel == n ? kLangRows : s_lang.pos.sel - s_lang.pos.top));
    rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, {0x17u, 0x7Fu, 0x40u});
}

// Each front-end frame: rows the pass rewrote are relaid out in place.
void languages_frame(uint8_t* rdram, recomp_context* ctx)
{

    if (!langs_on_screen(rdram) || !s_lang.dirty) return;

    rs64::mips::mips_call(rdram, ctx, 0x800C4C8Cu, {GCMD, (1u << kLangRows) - 1, 0u});
    s_lang.dirty = false;
}

} // namespace rs64::menucfg

using namespace rs64::menucfg;

// ---- Front-end hook entry points ----

extern "C" void rs64_menu_config_init(void) { (void)config(); }

extern "C" void rs64_menu_text_language(int lang) {
    s_text_language.store(lang, std::memory_order_relaxed);
}

// Returns the entry count when the built menu is a mod page (the build hook passes it to the game's active-mask loop), else -1.
extern "C" int rs64_menu_install_main(uint8_t* rdram)
{

    static const bool off = menu_buttons_off();

    if (off) return -1;

    s_rdram = rdram;
    s_in_pause = false;

    for (int i = 0; i < MAX_ENTRIES; ++i) s_slot[i] = {};

    const uint8_t menu = rd_b(rdram, GCMD_OFF + 0x04);

    // YES/NO confirm screen in place of the menu the entry is on, riding the engine's transitions; YES and NO lead back to that menu.
    if (s_confirm_state == ConfirmState::Pending && menu == s_confirm_menu && !s_confirm_shown)
    {
        const bool page = page_name(menu) != nullptr;
        const int16_t x = menu == MENU_MAIN ? YESNO_X : 0;
        const int32_t sc = 0x3F800000;

        wr_w(rdram, GCMD_OFF + 0x00, (int32_t)alloc_str(rdram, s_confirm_prompt));
        Slot yes{ (int32_t)alloc_str(rdram, "YES"), SUBTYPE_SUBMENU, menu, x, 0, page ? sc : rd_w(rdram, GCMD_OFF + 0x54) };
        Slot no { (int32_t)alloc_str(rdram, "NO"),  SUBTYPE_SUBMENU, menu, x, 0, page ? sc : rd_w(rdram, GCMD_OFF + 0x54 + 4) };
        write_slot(rdram, 0, yes);
        write_slot(rdram, 1, no);
        s_slot[1].sound = kSfxBack;
        wr_b(rdram, GCMD_OFF + 0x95, 2);

        // B on a page's prompt returns to the page, as NO does.
        if (page) wr_b(rdram, GCMD_OFF + 0x05, menu);

        s_confirm_shown = true;
        s_page_rows = false;
        s_note.reset();
        s_snd_sliders.clear();

        return page ? 2 : -1;
    }

    // Any other build while a prompt is pending (B left it): the prompt is over.
    s_confirm_state = ConfirmState::None;

    // Back at the main menu (B from the profile select or the page): a pilot-first page is no longer armed.
    if (menu == MENU_MAIN) {
        s_pilot_page.clear();
        s_pilot_exit.clear();
        rs64_hook(RS64_HOOK_MAIN_MENU, rdram, nullptr);
    }

    // Each page left (the last one shown and its ancestors, unless the new menu is inside them) runs its opener's leave_action.
    const std::vector<uint8_t> now = page_chain(menu);

    for (uint8_t left : page_chain(s_shown_page))
    {
        if (std::find(now.begin(), now.end(), left) != now.end()) continue;

        for (const auto& b : config().buttons)
            if (b.page == *page_name(left) && !b.leave_action.empty()) { fire_action(b.leave_action); break; }
    }

    s_shown_page = page_name(menu) ? menu : 0xFF;

    if (const std::string* page = page_name(menu))
    {
        if (*page == kModsPage) return mods_install(rdram, page_parent(menu));
        if (*page == kLanguagesPage) return languages_install(rdram, page_parent(menu));

        return build_mod_page(rdram, *page, page_parent(menu));
    }

    if (menu == MENU_CONTROLLER_SETTINGS && controls_enabled()) return controls_install(rdram);
    if (frontend_menu_name(menu)) rebuild_frontend_menu(rdram, menu);
    if (menu == MENU_SOUND_SETTINGS) sound_settings_install(rdram);

    return -1;
}

// Presses the game delivered to a mod entry; the nav sequencer's menu: target waits on this (some presses are dropped).
static std::atomic<uint32_t> s_confirms{0};
extern "C" uint32_t rs64_menu_confirms() { return s_confirms.load(std::memory_order_relaxed); }

// A confirmed entry's sound, for the game's next 0x80087EE8 call; game thread only.
MenuSfx s_sfx_swap;
bool s_sfx_swap_on = false;

// On A over a mod entry with a sound: sub-types 1, 5, 24 and 25 play theirs first thing in their confirm arm, so that call is swapped; the others play none, so it plays here.
extern "C" void rs64_menu_confirm_sound(uint8_t* rdram, recomp_context* ctx)
{

    static const bool off = menu_buttons_off();

    if (off) return;

    const uint8_t e = rd_b(rdram, GCMD_OFF + 0x94);

    if (e >= MAX_ENTRIES || s_slot[e].sound.id == -1) return;

    const MenuSfx s = s_slot[e].sound;
    const uint8_t sub = rd_b(rdram, GCMD_OFF + kRowType + e);

    if (sub == SUBTYPE_SUBMENU || sub == SUBTYPE_TOGGLE || sub == SUBTYPE_LANGUAGE || sub == SUBTYPE_BACK)
    {
        s_sfx_swap = s;
        s_sfx_swap_on = true;
        return;
    }

    if (s.id >= 0) rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, { (uint32_t)s.id, (uint32_t)s.vol, 0x40u });
}

// Entry of 0x80087EE8 (id $a0, volume $a1, pan $a2); 1 skips the call.
extern "C" int rs64_menu_sfx_swap(recomp_context* ctx)
{

    if (!s_sfx_swap_on) return 0;

    s_sfx_swap_on = false;

    if (s_sfx_swap.id < 0) return 1;

    ctx->r4 = (uint32_t)s_sfx_swap.id;
    ctx->r5 = (uint32_t)s_sfx_swap.vol;

    return 0;
}

extern "C" void rs64_menu_confirm_main(uint8_t* rdram)
{

    static const bool off = menu_buttons_off();

    if (off) return;

    uint8_t e = rd_b(rdram, GCMD_OFF + 0x94);

    if (s_confirm_state == ConfirmState::Pending)
    {
        if (rd_b(rdram, GCMD_OFF + 0x04) != s_confirm_menu || !s_confirm_shown) { s_confirm_state = ConfirmState::None; return; }

        if (e == 0)
        {
            if (s_confirm_fn) call_export(rdram, s_confirm_fn); else fire_action(s_confirm_action);

            s_confirm_state = ConfirmState::None;
        } else if (e == 1) { s_confirm_state = ConfirmState::None; }

        return;
    }

    if (controls_confirm(rdram, e)) return;
    if (mods_confirm(rdram, e)) return;

    if (e < MAX_ENTRIES && s_slot[e].set) s_confirms.fetch_add(1, std::memory_order_relaxed);
    if (e >= MAX_ENTRIES || !s_slot[e].set) return;

    const SlotBinding& b = s_slot[e];

    if (b.type == BType::Submenu)
    {
        // Remember the menu it opens from (Back's target); the entry's sub-type-1 transition then builds the page.
        s_page_parent_of[page_id(b.key)] = rd_b(rdram, GCMD_OFF + 0x04);

        if (!b.open_action.empty()) fire_action(b.open_action);
    } else if (b.type == BType::Toggle) {
        if (b.toggle_set_fn) call_export(rdram, b.toggle_set_fn); else toggle_flip(b.key);

        // In-place toggles are emitted as native sub-type 5, so the game re-renders
        // them; reload toggles ride the sub-type-1 self-target. Either way, just flip.
    } else if (b.type == BType::Slider) {
        int v = slider_value(rdram, b.key, b.toggle_get_fn);
        slider_apply(rdram, b.key, b.toggle_set_fn, confirmed_slider_value(e, v, b.smin, b.smax, b.sstep, b.bar_label,
                                                                           b.bar_width, b.bar_filled, b.bar_empty));
    } else if (!b.confirm.empty()) {
        s_confirm_state = ConfirmState::Pending;
        s_confirm_menu = rd_b(rdram, GCMD_OFF + 0x04);
        s_confirm_shown = false;
        s_confirm_action = b.key;
        s_confirm_prompt = b.confirm;
        s_confirm_fn = b.action_fn;
    } else {
        if (b.action_fn) call_export(rdram, b.action_fn); else if (!b.key.empty()) fire_action(b.key);

        if (!b.pilot_page.empty()) {
            s_pilot_page = b.pilot_page;
            s_pilot_exit = b.exit_when;
        }
    }
}

// The pilot select's carousel and medals, hidden while a pilot-first page is up (the chosen-pilot fade, state 0xA, that would clear them is skipped); re-entering SELECT GAME resets it (readAccountForSelectionScreen).
// Carousel slots 0x800CF180 (stride 0x48): load fade +0x18, transition fade +0x1C drive element alpha and visibility; the medals (font 8 slots 1-3) copy alpha from the front slot only while it is visible.
static void hide_pilot_carousel(uint8_t* rdram, recomp_context* ctx, bool medals) {

    for (uint32_t i = 0; i < 3; ++i) {
        wr_w(rdram, 0x000CF180u + i * 0x48 + 0x18, 0);
        wr_w(rdram, 0x000CF180u + i * 0x48 + 0x1C, 0);
    }

    if (medals)
    {
        for (uint32_t s = 1; s <= 3; ++s) rs64::mips::mips_call(rdram, ctx, 0x80061C74u, {8u, s, 0u});
    }
}

// Exit conditions for pilot-first pages, registered through the host API (multiplayer's mp_ready).
static bool exit_condition(const std::string& key) { return rs64::host::condition(key.c_str()) > 0; }

// menuControllerInput 0x800B5428: a pilot was chosen and loaded (s6 = 0xA, the exit fade is next). With a pilot-first page armed, take the native sub-type-1 path to it instead
// ($s6 = 1 fade, target menu at sp+0xBF, sp+0x80 = 0 so the fade does not leave the menus).
extern "C" void rs64_menu_pilot_chosen(uint8_t* rdram, recomp_context* ctx)
{

    if (s_pilot_page.empty()) return;

    const uint32_t spv = (uint32_t)ctx->r29;
    const uint32_t sp = spv - 0x80000000u;
    wr_b(rdram, sp + 0xBF, page_id(s_pilot_page));
    wr_b(rdram, sp + 0x80, 0);

    // As the sub-type-1 confirm (0x800B5268-0x800B5334): the fade waits for the menu wipe 0x800B3F24 starts, named by sprintf(sp+0x20, 0x800A6644, picture + 1), unless 0x800CF046 skips it.
    const bool skip_wipe = rd_b(rdram, 0x000CF046u) != 0;

    if (!skip_wipe) wr_b(rdram, sp + 0xA7, (uint8_t)((rd_b(rdram, sp + 0xA7) + 1) % 7));

    rs64::mips::mips_call(rdram, ctx, 0x80033CC4u, {spv + 0x20, 0x800A6644u, (uint32_t)rd_b(rdram, sp + 0xA7) + 1});

    if (rd_b(rdram, sp + 0x7F) & 2) {
        wr_w(rdram, sp + 0x20, rd_w(rdram, 0x000A664Cu));
        wr_h(rdram, sp + 0x24, rd_h(rdram, 0x000A6650u));
    }

    if (!skip_wipe) rs64::mips::mips_call(rdram, ctx, 0x800B3F24u, {(uint32_t)rd_w(rdram, 0x000CF10Cu), spv + 0x20});

    hide_pilot_carousel(rdram, ctx, true);
    ctx->r22 = 1;
    s_page_parent_of[page_id(s_pilot_page)] = MENU_MAIN;

    fprintf(stderr, "[menu] pilot chosen: opening page %s\n", s_pilot_page.c_str());
}

// A page with a shared row: the cursor moves by line (wrapping) and left/right within a line; the native list order would step through a row's entries one by one.
// Runs before the menu's own input reads the pressed word (0x8013A960, port 0), and takes the presses it handles.
static void page_row_nav(uint8_t* rdram)
{

    if (!s_page_rows) return;

    constexpr uint32_t kUp = 0x00200800u, kDown = 0x00100400u, kLeft = 0x00800200u, kRight = 0x00400100u;
    const uint32_t p = (uint32_t)rd_w(rdram, 0x0013A960u);

    if (!(p & (kUp | kDown | kLeft | kRight))) return;

    const int sel = rd_b(rdram, GCMD_OFF + 0x94);

    if (sel >= MAX_ENTRIES || s_slot_line[sel] < 0) return;

    const int line = s_slot_line[sel];
    int first = sel;

    while (first > 0 && s_slot_line[first - 1] == line) --first;

    int last_line = 0;

    for (int i = 0; i < MAX_ENTRIES; ++i) last_line = std::max(last_line, s_slot_line[i]);

    const bool in_row = (first + 1 < MAX_ENTRIES && s_slot_line[first + 1] == line);

    // Left/right on a line of its own stays the entry's (a slider's).
    if (!in_row && (p & (kUp | kDown)) == 0) return;

    int target = sel;

    if (in_row && (p & (kLeft | kRight)))
    {
        const int to = sel + ((p & kLeft) ? -1 : 1);

        if (to >= 0 && to < MAX_ENTRIES && s_slot_line[to] == line) target = to;
    }

    // Entering a row picks the column last used in a row (clamped to its last entry).
    static int s_col = 0;

    if (in_row) s_col = sel - first;

    if (target == sel && (p & (kUp | kDown)))
    {
        const int want = (p & kUp) ? (line == 0 ? last_line : line - 1) : (line == last_line ? 0 : line + 1);

        for (int i = 0, col = 0; i < MAX_ENTRIES; ++i) {
            if (s_slot_line[i] != want) continue;

            target = i;

            if (col++ == s_col) break;
        }
    } else if (target != sel) {
        s_col = target - first;
    }

    wr_w(rdram, 0x0013A960u, (int32_t)(p & ~(kUp | kDown | kLeft | kRight)));
    wr_b(rdram, GCMD_OFF + 0x94, (uint8_t)target);
}

// Left / right on an entry with left_action / right_action fires that action and takes the press (the multiplayer list's pager).
static void page_side_actions(uint8_t* rdram)
{

    constexpr uint32_t kLeft = 0x00800200u, kRight = 0x00400100u;
    const uint32_t p = (uint32_t)rd_w(rdram, 0x0013A960u);
    const int sel = rd_b(rdram, GCMD_OFF + 0x94);

    if (!(p & (kLeft | kRight)) || sel >= MAX_ENTRIES || !s_slot[sel].set)
    {
        return;
    }

    const std::string& action = (p & kLeft) ? s_slot[sel].left_action : s_slot[sel].right_action;

    if (action.empty())
    {
        return;
    }

    fire_action(action);
    wr_w(rdram, 0x0013A960u, (int32_t)(p & ~(kLeft | kRight)));
}

// applyLevelScreenAndCameraSettings entry: a HIGH RESOLUTION change made in the pause menu takes effect here, before the screen setup reads it.
extern "C" void rs64_menu_hires_apply(uint8_t* rdram)
{

    if (s_hires_pending < 0) return;

    wr_b(rdram, HIRES_BYTE, (uint8_t)((rd_b(rdram, HIRES_BYTE) & ~1u) | (unsigned)s_hires_pending));
    s_hires_pending = -1;
    s_hires_save = true;
}

// Each front-end loop pass, after the pad poll: the page's own input, then a pilot-first page whose exit condition holds leaves the way a chosen pilot does
// ($s6 = 0xA, sp+0x80 = 1 and sp+0x9F = 1, the fade from 0x800A6760 at sp+0x118): fade, return from the menus, mission select with the pilot loaded.
extern "C" int rs64_menu_page_shown(void) {
    return s_page_shown.load() ? 1 : 0;
}

extern "C" void rs64_menu_pass(uint8_t* rdram, recomp_context* ctx)
{

    s_rdram = rdram;

    // A HIGH RESOLUTION change is saved like the native toggle saves it (serializeSettingsToSaveAndPersist).
    if (s_hires_save) {
        s_hires_save = false;
        rs64::mips::mips_call(rdram, ctx, 0x8006E1BCu, {});
    }

    const uint8_t menu = rd_b(rdram, GCMD_OFF + 0x04);
    const bool page_shown = page_name(menu) != nullptr;
    s_page_shown.store(page_shown);

    // Handlers that take the pad (the multiplayer address editor) run before the page's row navigation reads it.
    rs64::host::set_flag("menu_page_shown", page_shown ? 1 : 0);
    rs64::host::dispatch(RS64_HOOK_MENU_PAD, rdram, ctx);

    if (page_shown) {
        page_side_actions(rdram);
        page_row_nav(rdram);
    }

    controls_pass(rdram, ctx);
    mods_pass(rdram, ctx);
    languages_pass(rdram, ctx);

    // The pilot-first page or a page opened from it (multiplayer's JOIN GAME list): the exit condition takes either on to mission select.
    const std::vector<uint8_t> chain = page_chain(menu);

    if (s_pilot_page.empty() || std::find(chain.begin(), chain.end(), page_id(s_pilot_page)) == chain.end()) return;

    // A carousel slot still loading ramps its fade back up: keep it down while the page is shown.
    hide_pilot_carousel(rdram, ctx, false);

    if ((uint32_t)ctx->r22 != 0 || !exit_condition(s_pilot_exit)) return;

    const uint32_t sp = (uint32_t)ctx->r29 - 0x80000000u;
    ctx->r22 = 0xA;
    wr_b(rdram, sp + 0x80, 1);
    wr_b(rdram, sp + 0x9F, 1);
    wr_w(rdram, sp + 0x118, rd_w(rdram, 0x000A6760u));

    fprintf(stderr, "[menu] page %s done: on to mission select\n", s_pilot_page.c_str());
    s_shown_page = 0xFF;
    s_pilot_page.clear();
    s_pilot_exit.clear();
}

// Sound Settings slider hooks: menuControllerInput 0x800b5920 takes a channel index at sp+0xEF, redirected to the mod slider's unused channel; getGameOrFrontText 0xB7/0xB8 (OFF/MAX endpoint text) is overridden per slider;
// updateMenuPerFrame syncs the byte to the mod @export.

// Redirect the channel index for a mod slider (hook: 0x800b5920, sp = ctx->r29).
// menuControllerInput's state dispatch (0x800B4E64), every pass of the menu loop: state = $s6, 0x11 while a Sound Settings volume bar is being edited.
static std::atomic<bool> s_slider_editing{false};

// Hover moved a highlight (touch/mouse, any thread): 1 front end, 2 pause. The game thread plays the game's own cursor sound.
static std::atomic<int> s_hover_sound{0};

extern "C" void rs64_menu_hover_sound(int pause) {
    s_hover_sound.store(pause ? 2 : 1);
}

static bool take_hover_sound(int kind) {

    int want = kind;

    return s_hover_sound.compare_exchange_strong(want, 0);
}

extern "C" void rs64_menu_input_state(uint8_t* rdram, recomp_context* ctx)
{

    s_slider_editing.store(ctx->r22 == 0x11 && (rd_b(rdram, GCMD_OFF + 0x04) == MENU_SOUND_SETTINGS || snd_sliders_here(rdram)));
    typing_pass();

    // As menuControllerInput does on a move (0x800B5108): sfx 0x17, then restart the highlight pulse.
    if (take_hover_sound(1)) {
        rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, { 0x17, 0x7F, 0x40 });
        wr_w(rdram, GCMD_OFF + 0x9C, 0);
        wr_w(rdram, GCMD_OFF + 0xA0, rd_w(rdram, 0x800A66F0u - 0x80000000u));
    }
}

// Pause HUD frame entry: the pause menu's move sound (0x800C28F0, quieter than the front end's).
extern "C" void rs64_pause_hover_sound(uint8_t* rdram, recomp_context* ctx) {

    if (take_hover_sound(2)) {
        rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, { 0x17, 0x5F, 0x40 });
    }
}

extern "C" int rs64_menu_slider_editing(void) {
    return s_slider_editing.load() ? 1 : 0;
}

extern "C" void rs64_slider_redirect(uint8_t* rdram, uint32_t sp)
{

    if (!snd_sliders_here(rdram) || sp < 0x80000000u) return;

    const SndSlider* ss = snd_slider_for_slot(rd_b(rdram, GCMD_OFF + 0x94));

    if (ss) wr_b(rdram, (sp - 0x80000000u) + 0xEF, (uint8_t)ss->channel);
}

// A tap/click at fraction f across a front-end entry's text (key = slot) or a pause line (key = record nextMenu); if that entry is a text
// slider and the point is on its bar, its next confirm sets the value there instead of stepping. Any thread.
extern "C" void rs64_menu_slider_click(int key, float f) {

    s_click_frac.store(f);
    s_click_ms.store(SDL_GetTicks());
    s_click_key.store(key);
}

// Volume channel byte of a mod Sound Settings slider at `slot` (the entry's sub-type says music); -1 for a native entry.
extern "C" int rs64_menu_slider_channel(int slot) {

    const SndSlider* ss = snd_slider_for_slot(slot);

    return ss ? ss->channel : -1;
}

// Override a textId's returned string (hook: getGameOrFrontText jr-ra 0x800558c4):
// mod slider OFF/MAX endpoints, and in-place game-settings toggle labels.
extern "C" unsigned rs64_gof_override(uint8_t* rdram, unsigned textId)
{
    // In-place game_settings toggles: the replaced native toggle's label textIds show
    // the mod's labels, reflecting the mod's own state.
    for (const auto& t : s_inplace_tgls) {
        if (textId == t.off_tid || textId == t.on_tid) {
            bool on = button_toggle_state(rdram, t.key, t.get);

            return alloc_str(rdram, on ? t.on : t.off);
        }
    }

    if (!snd_sliders_here(rdram)) return 0;

    const SndSlider* ss = snd_slider_for_slot(rd_b(rdram, GCMD_OFF + 0x94));

    if (!ss) return 0;
    if (textId == 0xB7) return alloc_str(rdram, ss->off_label);
    if (textId == 0xB8) return alloc_str(rdram, ss->on_label);

    return 0;
}

// The note label while its page is up: relaid out when its text changes, cleared on leaving, and its alpha set each frame after the pilot-first page hides the medals in the same slot.
static void note_frame(uint8_t* rdram, recomp_context* ctx)
{

    const bool here = s_note.has_value() && rd_b(rdram, GCMD_OFF + 0x04) == s_note_menu;
    std::string t;

    if (here)
    {
        for (const char c : s_note->label_src.empty() ? s_note->label : source_text(s_note->label_src))
            if (c == ' ' || font_glyph(rdram, kModsDescFont, c) >= 0) t += c;
    }

    if (t != s_note_text)
    {
        s_note_text = t;
        mods_draw_desc(rdram, ctx, t, here && s_note->y ? s_note->y : kModsTopY);
    }

    if (!t.empty()) rs64::mips::mips_call(rdram, ctx, 0x80061C74u, {kModsDescFont, kModsDescFontSlot, 0xFEu});
}

// Every front-end frame (hook: updateMenuPerFrame): live page labels, the page's note, and each mod slider's channel byte synced to its @export value.
extern "C" void rs64_menu_frame(uint8_t* rdram, recomp_context* ctx)
{

    ensure_menu_period(rdram);
    rs64::lang::flush(rdram);
    controls_frame(rdram, ctx);
    mods_frame(rdram, ctx);
    languages_frame(rdram, ctx);
    note_frame(rdram, ctx);

    bool lock_shown = false;

    // A changed live label is rewritten in its buffer and relaid out in place (0x800C4C8C(menu, entry mask, 0), as the name entry does).
    if (page_name(rd_b(rdram, GCMD_OFF + 0x04)))
    {
        uint32_t mask = 0;

        for (int i = 0; i < MAX_ENTRIES; ++i)
        {
            SlotBinding& b = s_slot[i];

            if (!b.set || !b.live_buf || (uint32_t)rd_w(rdram, GCMD_OFF + 0x08 + i * 4) != b.live_buf) continue;

            std::string t;

            if (!b.label_src.empty())
                t = source_text(b.label_src);
            else if (b.type == BType::Toggle)
                t = toggle_text(rdram, b.key, b.toggle_get_fn, b.bar_label, b.label_on, b.label_off);
            else if (b.type == BType::Slider)
                t = slider_bar_text(b.bar_label, slider_value(rdram, b.key, b.toggle_get_fn), b.smin, b.smax, b.bar_width, b.bar_filled, b.bar_empty);

            lock_shown = lock_shown || t.find('$') != std::string::npos;

            if (t.empty() || t == b.live_text) continue;

            write_live(rdram, b.live_buf, t);
            b.live_text = t;
            mask |= 1u << i;
        }

        if (mask) rs64::mips::mips_call(rdram, ctx, 0x800C4C8Cu, {GCMD, mask, 0u});
    }

    show_menu_lock(rdram, lock_shown);

    if (!snd_sliders_here(rdram)) return;

    // Only a moved slider is applied: setters save their settings file.
    for (auto& ss : s_snd_sliders)
    {
        const int byte = rd_b(rdram, VOL_BYTES + ss.channel);

        if (byte == ss.last_byte) continue;

        if (ss.last_byte >= 0) slider_apply(rdram, ss.key, ss.set_fn, value_from_byte(byte, ss.smin, ss.smax));

        ss.last_byte = byte;
    }
}
