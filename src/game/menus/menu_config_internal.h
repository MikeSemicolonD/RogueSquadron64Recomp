#pragma once
// Menu button config internals shared by the front-end menus (menu_config.cpp), their host-built screens (controls_menu.cpp, mods_menu.cpp), the font and typing helpers, and the pause menu (pause_menu.cpp).
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "recomp.h"
#include "json/json.hpp"
#include "input/input_bindings.h"
#include "game/menus/menu_layout.h"

namespace rs64::menucfg {

using rs64::menu::kRowLabel;
using rs64::menu::kRowType;
using rs64::menu::kRowPos;
using rs64::menu::kRowScale;
using rs64::menu::kRowParam;

// A menu SFX for 0x80087EE8 (id, volume); id -1 keeps the game's sound, -2 plays none.
struct MenuSfx {
    int id = -1;
    int vol = 0x64;
};

// Label: a page line the cursor skips (sub-type 13), e.g. a live status line.
enum class BType { Action, Toggle, Slider, Submenu, Label };

struct Button {
    std::string id;

    // named location (main_menu, game_settings, pause, ...)
    std::string menu;
    BType type = BType::Action;

    // action or toggle registry key (or @export in 1b)
    std::string behavior;

    // Action: non-empty => YES/NO prompt
    std::string confirm;

    // Action label
    std::string label;

    // Toggle labels
    std::string label_on;
    std::string label_off;

    // Submenu: the page it opens; page entries use menu==page
    std::string page;

    // Submenu: the opened page's title (default = label)
    std::string title;

    // Submenu: the page title's y offset (gCurrentMenuData+0x52, title slot 9)
    int title_y = 0;

    // Submenu: action fired when its page is left (its "action" fires on open)
    std::string leave_action;

    // Action entry: actions fired by left / right while it is selected (a pager line).
    std::string left_action, right_action;

    // Page entry: live text source; the drawn label follows it every frame
    std::string label_src;

    // Page label: small white text at y (default under the title), not an entry; one a page.
    bool note = false;

    // Front-end action: also run the native transition to this menu ("account" = the profile select, like START; "language" = the game's LANGUAGE page)
    std::string transition;

    // With transition "account": once a pilot is chosen, show this page instead of leaving for mission select, and leave when the exit_when condition holds.
    std::string exit_when;

    // placement
    bool has_order = false;
    int order = 0;

    // anchor to a native alias / button id
    std::string before, after, replace;

    // index: final visible position, negative counts from the end (wins over anchors). requires_conds: conditions that must all hold for the button to show.
    bool has_index = false;
    int index = 0;
    std::vector<std::string> requires_conds;

    int x = 0, y = 0;

    // Page entries: extra space below this line (pixels) and the text scale.
    int gap_after = 0;
    float scale = 1.0f;

    // Page entries with the same row share one line (left to right in file order, placed by x); left/right moves between them.
    std::string row;

    // pause entry flag word (0x4000 style + 0x0001 selectable)
    int flags = 0x4001;

    // owning mod (for @export resolution)
    std::string mod_id;

    // Slider (value control rendered as a font-glyph bar in the label).
    int smin = 0, smax = 100, sstep = 10;
    int bar_width = 10;
    std::string bar_filled = "|";
    std::string bar_empty  = " ";

    // Cycle: per-choice label overrides (strings or language maps) and the text for a value matching no choice.
    std::vector<std::string> value_labels;
    std::string label_other;

    MenuSfx sound;
};

// A menu's title override (top-level "titles": menu name -> { label, x, y }); gCurrentMenuData +0x00 text, +0x50 / +0x52 offsets.
struct MenuTitle {
    std::string text;
    int x = 0, y = 0;
    bool has_x = false, has_y = false;
};

struct MenuConfig {
    std::vector<Button> buttons;

    // menu name -> native aliases to hide
    std::unordered_map<std::string, std::vector<std::string>> hides;

    // menu name -> title override; a later mod's replaces an earlier one's
    std::unordered_map<std::string, MenuTitle> titles;
};

// Physical-offset RDRAM accessors.
inline int32_t rd_w(uint8_t* r, uint32_t off)           { return *(int32_t*)(r + off); }
inline int16_t rd_h(uint8_t* r, uint32_t off)           { return *(int16_t*)(r + (off ^ 2)); }
inline uint8_t rd_b(uint8_t* r, uint32_t off)           { return r[off ^ 3]; }
inline void    wr_w(uint8_t* r, uint32_t off, int32_t v){ *(int32_t*)(r + off) = v; }
inline void    wr_h(uint8_t* r, uint32_t off, int16_t v){ *(int16_t*)(r + (off ^ 2)) = v; }
inline void    wr_b(uint8_t* r, uint32_t off, uint8_t v){ r[off ^ 3] = v; }

constexpr uint8_t FIRST_PAGE_ID = 13;

// channel volume bytes (0..N)
constexpr uint32_t VOL_BYTES = 0x80130B60u - 0x80000000u;

// native slider range
constexpr int      VOL_MAX   = 0x80;

// music-volume sub-type
constexpr uint8_t  SND_SLIDER_SUB = 21;

// Free settings bytes past the three volumes: +0x23 is the Expansion Pak / time-limit flag and +0x24 the high-score sort's, so mod sliders start at +0x25.
constexpr int      SND_CH_FIRST = 5, SND_CH_LAST = 7;

struct SndSlider {
    int slot = -1, channel = 0, smin = 0, smax = 100, last_byte = -1;
    std::string off_label, on_label, key;
    recomp_func_t* get_fn = nullptr, * set_fn = nullptr;
};

struct BehaviorFns { recomp_func_t* action = nullptr, * toggle_get = nullptr, * toggle_set = nullptr; };

extern uint8_t* s_rdram;

// Set while the in-mission pause menu is open (rs64_pause_install).
extern bool s_in_pause;

// The native sliders of the menu last built (Sound Settings or a mod page).
extern std::vector<SndSlider> s_snd_sliders;

bool env_disabled(const char* name);
const MenuConfig& config();
uint32_t alloc_str(uint8_t* rdram, const std::string& s);
void call_export(uint8_t* rdram, recomp_func_t* fn);
void fire_action(const std::string& key);
void toggle_flip(const std::string& key);
nlohmann::json setting_rpc(const nlohmann::json& p);
void input_set(const std::function<void(rs64::input::Bindings&)>& set);
void reset_controls();
BehaviorFns resolve_behavior(const std::string& mod_id, BType type, const std::string& behavior);
std::string toggle_text(uint8_t* rdram, const std::string& key, recomp_func_t* get_fn, const std::string& label, const std::string& on, const std::string& off);
int slider_value(uint8_t* rdram, const std::string& key, recomp_func_t* get_fn);
void slider_apply(uint8_t* rdram, const std::string& key, recomp_func_t* set_fn, int v);
std::string slider_bar_text(const std::string& label, int value, int vmin, int vmax, int width, const std::string& fch, const std::string& ech);
int confirmed_slider_value(int key, int value, int vmin, int vmax, int step, const std::string& label, int width, const std::string& fch, const std::string& ech);
bool button_visible(uint8_t* rdram, const Button& b, bool pause = false);
uint8_t page_id(const std::string& page);
const std::string* page_name(uint8_t id);

// Pause menu id of a mod page (0xFF if none), as its root and GAME SETTINGS entries link to it.
uint8_t pause_page_id(const std::string& page);
int byte_from_value(int v, int lo, int hi, int top = VOL_MAX);
int value_from_byte(int b, int lo, int hi, int top = VOL_MAX);

// ---- The front-end menu (gCurrentMenuData) and its entry slots ----

constexpr uint32_t GCMD = 0x800CE730u;
constexpr uint32_t GCMD_OFF = GCMD - 0x80000000u;
constexpr int MAX_ENTRIES = rs64::menu::kMenuRows;

// Rows a scrolling list page (controls, languages, mods) shows at once.
constexpr int PAGE_ROWS = 8;

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
    MENU_LANGUAGE            = 9,
    // Ids 0-12 are native (setupMenuData range-checks < 13); an id >= 13 bails to the shared tail with 0 entries, so our install hook fully builds it as a custom page.
    MENU_MOD_PAGE            = 13,
    MENU_NONE                = 0xFF,
};

// Entry sub-types. 1 = submenu transition (self-targeting = re-run setupMenuData, the only way to re-commit labels); 4 = in-range no-op on confirm (intercept owns the action).
constexpr uint8_t SUBTYPE_SUBMENU = 1;
constexpr uint8_t SUBTYPE_HOST    = 4;

// The sound menuControllerInput plays for BACK, at its volume.
constexpr MenuSfx kSfxBack{ 0x6D, 0x64 };

// The pressed-buttons word of port 0 and its direction bits (stick and d-pad).
constexpr uint32_t PAD_PRESSED = 0x0013A960u;
constexpr uint32_t kNavUp = 0x00200800u, kNavDown = 0x00100400u, kNavLeft = 0x00800200u, kNavRight = 0x00400100u;

struct Slot { int32_t label; uint8_t sub; int32_t param; int16_t x, y; int32_t scaler; };

Slot read_slot(uint8_t* r, int i);
void write_slot(uint8_t* r, int i, const Slot& s);

// Per-slot binding, read by the confirm intercept.
struct SlotBinding {
    bool set = false;
    BType type = BType::Action;
    std::string key, confirm;

    // toggle_get_fn/toggle_set_fn double as the slider getter/setter @exports.
    recomp_func_t* action_fn = nullptr, * toggle_get_fn = nullptr, * toggle_set_fn = nullptr;

    // Slider range.
    int smin = 0, smax = 100, sstep = 10;

    // Toggle labels, for the in-place relabel.
    std::string label_on, label_off;

    // Submenu opener: action fired when the page opens.
    std::string open_action;

    // Left / right actions of an action entry.
    std::string left_action, right_action;

    // Action with transition "account": the page shown after the pilot is chosen, and its exit condition.
    std::string pilot_page, exit_when;

    // Live label: the entry points at its own RDRAM buffer (never an alloc_str cache entry, which other labels share).
    std::string label_src, live_text;
    uint32_t live_buf = 0;

    // Text slider bar layout, for click-to-position.
    std::string bar_label, bar_filled, bar_empty;
    int bar_width = 10;

    MenuSfx sound;
};

extern SlotBinding s_slot[MAX_ENTRIES];

// Whether the page last built has entries sharing a line (page_row_nav steers by line then).
extern bool s_page_rows;

// One RDRAM text buffer per id, kept for the session, and its rewrite.
uint32_t live_buffer(uint8_t* rdram, const std::string& id);
void write_live(uint8_t* rdram, uint32_t vaddr, const std::string& s);

void apply_title(uint8_t* rdram, const std::string& menu);
const char* back_text();

// A host page's string in the shown language: English, German and French built in, and a text key ("mods.<key>") language mods and menu_text.json can translate.
std::string mods_text(const char* key, const char* en, const char* de, const char* fr);

// Last tap/click on a menu entry (rs64_menu_slider_click).
extern std::atomic<int> s_click_key;
extern std::atomic<float> s_click_frac;
extern std::atomic<uint32_t> s_click_ms;

// ---- Host-built list screens ----

// Row layout the Controls list and the MODS page share; rows end at the end stop with the font's blank glyph so they have one width.
constexpr int kCtlPitch = 36, kCtlBackGap = 10, kCtlX = 15;
constexpr const char* kCtlRowEnd = "\t`";

// Mouse wheel notches from the SDL thread (up positive), taken by the list screens' passes as up/down steps.
extern std::atomic<int> s_ctl_wheel;

bool controls_enabled();
bool controls_on_screen(uint8_t* rdram);
int controls_install(uint8_t* rdram);
bool controls_confirm(uint8_t* rdram, int e);
void controls_pass(uint8_t* rdram, recomp_context* ctx);
void controls_frame(uint8_t* rdram, recomp_context* ctx);

constexpr const char* kModsPage = "mods";
constexpr int kModsTopY = -136;

// The small white text line under a page title (the MODS description, a page's note): hatten13, text group 8, slot 1.
constexpr uint32_t kModsDescFont = 8, kModsDescFontSlot = 1;

bool mods_on_screen(uint8_t* rdram);
int mods_install(uint8_t* rdram, uint8_t parent);
bool mods_confirm(uint8_t* rdram, int e);
void mods_pass(uint8_t* rdram, recomp_context* ctx);
void mods_frame(uint8_t* rdram, recomp_context* ctx);
void mods_draw_desc(uint8_t* rdram, recomp_context* ctx, const std::string& text, int y = kModsTopY);

bool langs_on_screen(uint8_t* rdram);

// ---- menu_font.cpp ----

int font_glyph(uint8_t* rdram, uint32_t font, char c);
int font_width(uint8_t* rdram, uint32_t font, const std::string& s, int gap);
void ensure_menu_period(uint8_t* rdram);
void show_menu_lock(uint8_t* rdram, bool lock);

// ---- menu_typing.cpp ----

// Each menu pass: text input follows whether a letter wheel ran on the previous pass.
void typing_pass();

}
