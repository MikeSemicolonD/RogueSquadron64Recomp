#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "touch_input.h"

// Menu awareness for touch: context from the game-state classifier, and tap-to-select on the
// front-end menu (gCurrentMenuData @ 0x800CE730, layout in docs/adding-menus-and-buttons.md).
namespace rs64::touch {
    // Context from the loaded overlay (g_active_overlay: 0 mission, 1 menu, 2 cinematic, -1 none), refined by
    // the classifier id on menu screens, the pause master word, and the attract-demo bit.
    Context classify(int overlay, const char* state_id, bool paused, bool demo);
    // Pause gate: D_8010CA20 == 5 while in a mission.
    bool read_paused(const uint8_t* rdram);
    // Attract demo: gGameSettings+0x10 bit 0x20.
    bool read_demo(const uint8_t* rdram);

    // Entry i sits at game y = list_base(+0x96) + 36*i - 18*n + y_off[i], centered on game x = x_off[i]
    // (setupMenuData tail, 0x800C4CF0). These map game units to the 320x240 screen space, fit to device screenshots.
    struct MenuLayout {
        float box_aspect;    // width/height of the presented game image (VI pixel aspect, not 4:3)
        float center_x;      // screen x of game x 0
        float x_scale;       // screen units per game x unit (320/512)
        float y_origin;      // screen y (text top) of game y 0
        float y_scale;       // screen units per game y unit
        float glyph_w;       // advance per character at scaler 1.0
        float line_h;        // text height at scaler 1.0
    };
    extern MenuLayout g_menu_layout;

    struct Box { int entry; float x0, y0, x1, y1; };
    struct MenuSnapshot { uint32_t menu_ptr; uint8_t menu_id; uint8_t count; uint8_t current; uint16_t active; };

    // Reads the front-end menu into boxes (normalized window coords); false if a bounds check fails.
    bool read_menu(const uint8_t* rdram, MenuSnapshot* snap, std::vector<Box>* boxes, float win_w, float win_h);
    // 320x240 game-screen point -> normalized window coords (same presentation box as the hit boxes).
    void n64_to_window(float px, float py, float win_w, float win_h, float* nx, float* ny);

    // SELECT GAME (menu.account.enter_name): a tap on a side save slot steps toward it.
    TapAction select_game_tap(float x, float y, float win_w, float win_h);
    // SELECT LEVEL: the ◁/▷ arrows in the bottom corners step levels; the bottom "PRESS C FOR LEVEL DESCRIPTION" line
    // presses C. This screen fills the window, so window coords.
    TapAction level_select_tap(float x, float y);

    // PASSCODES letter wheel band (window y 0.50..0.65); taps outside it go to ENTER CODE / BACK.
    bool on_wheel(float y);

    // Edge-to-edge front-end carousels (Concert Hall, Showroom): the ◁◁◁ / ▷▷▷ arrows in the bottom corners step.
    TapAction carousel_arrow_tap(float x, float y);

    // SELECT YOUR CRAFT: the top "PRESS C FOR CRAFT DESCRIPTION" line presses C; the screen edges step crafts (window coords).
    TapAction craft_select_tap(float x, float y);

    // Pilot-flow screens, from the menu overlay's screen u16 @0x800CFF50 (2 menu tree, 1 SELECT LEVEL, 0 SELECT YOUR CRAFT);
    // SELECT GAME is the menu tree with menu id 1 and entry 0 typed as a save slot (0x800CE758 == 3). Menu overlay only.
    enum class AccountScreen { NotAccount, SelectGame, Levels, Craft };
    AccountScreen account_screen(const uint8_t* rdram);

    // SOUND SETTINGS volume bar under a music/sfx/speech entry (sub-types 21/22/23), shown while that slider is being edited.
    struct SliderBar {
        int channel;     // 0 music, 1 sfx, 2 speech (byte at 0x80130B60 + channel)
        float x0, x1;    // window x of value 0 and value 128
        float y0, y1;    // touch band around the bar
    };
    bool sound_slider(const uint8_t* rdram, int entry, float win_w, float win_h, SliderBar* bar);
    int slider_value_at(const SliderBar& bar, float x);
    uint8_t read_volume(const uint8_t* rdram, int channel);
    void write_volume(uint8_t* rdram, int channel, uint8_t value);

    // Pause HUD (mission overlay): slot id u16 @0x8010BFD0 -> slot table *(0x80130BB0) (8-byte entries, +0 handler
    // handleHUD 0x800C0084, +4 HUD). Returns the HUD address or 0.
    uint32_t find_pause_hud(const uint8_t* rdram);
    // Pause-menu lines (HUD menu_elements, stride 0x30, x/y floats at +0x18/+0x1C, same game->screen transform as the
    // front end; lines are centered). `selectable` is the index among 0x0001 records the line belongs to, or -1.
    struct PauseLine {
        int element;
        int selectable;
        float x0, x1, y0, y1;
        bool bar = false;    // a volume bar's backing (full value range 0..127 across x0..x1)
        int channel = -1;    // volume channel of the slider row this line belongs to
    };
    // Horizontal extent of the presented game picture in window fractions: the mission image, the front-end menu tree's
    // image, or the full width on front-end screens drawn edge to edge (level/craft select, media hub).
    void picture_extent(const uint8_t* rdram, int overlay, float win_w, float win_h, float* x0, float* x1);
    // Game 320x240 point -> window, for the mission's presented image.
    void pause_to_window(float px, float py, float win_w, float win_h, float* nx, float* ny);
    // Volume (0..127) for a finger at window x on a pause volume bar.
    int pause_slider_value(const PauseLine& bar, float x);
    // Highlights pause row k (current_entry, HUD+0xD68). False if the pause HUD is not found.
    bool pause_set_entry(uint8_t* rdram, int k);
    bool read_pause_lines(const uint8_t* rdram, float win_w, float win_h, std::vector<PauseLine>* lines);
    // One tap zone per selectable pause row: its pieces merged, widened 10%, neighbours split at the vertical midpoint.
    struct PauseRow { int selectable; float x0, x1, y0, y1; };
    std::vector<PauseRow> pause_rows(const std::vector<PauseLine>& lines);
    // Hit-tests a tap against the pause rows; on a hit writes current_entry (HUD+0xD68). True on a hit.
    bool pause_tap_select(uint8_t* rdram, float x, float y, float win_w, float win_h);
    // Highlights the pause row under (x, y) and returns its selectable index; -1 when the point is on no row.
    int pause_hover(uint8_t* rdram, float x, float y, float win_w, float win_h);
    // Calibration log of the pause HUD: phase, current menu/entry, and each drawn line element (x, y, width, scale, color).
    std::string describe_pause(const uint8_t* rdram);

    // One line describing the resident menu's raw entry fields (id, count, per entry: len, x/y offset, scaler), for calibration logs.
    std::string describe_menu(const uint8_t* rdram);
    // Hit-tests a tap; on a hit writes current_menu_entry, but only if the menu is unchanged since `at_down`.
    bool tap_select(uint8_t* rdram, const MenuSnapshot& at_down, float x, float y, float win_w, float win_h);
}
