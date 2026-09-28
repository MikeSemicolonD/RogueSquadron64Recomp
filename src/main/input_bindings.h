#ifndef RS64_INPUT_BINDINGS_H
#define RS64_INPUT_BINDINGS_H

#include <cstdint>
#include <string>
#include <vector>

struct _SDL_GameController;
struct _SDL_Joystick;

namespace rs64::input {

// N64 controller outputs, as rebindable targets. The analog stick is modeled as
// four directional targets so keys, gamepad axes, and mouse motion all bind the
// same way; x/y are reconstructed from opposing deflections in resolve().
// Look* hold C-Up and point the stick while pressed (the game's look-around), for hats.
// Throttle is a positional axis (full travel, dir picks which end is forward) that sets flight speed directly.
// Roll* (a twist or rudder axis) drive stick X while the Roll button (R) is held, the game's pure roll; pitch stays on the stick.
enum class Target : int {
    A, B, Z, Start,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    LTrig, RTrig,
    CUp, CDown, CLeft, CRight,
    StickUp, StickDown, StickLeft, StickRight,
    LookUp, LookDown, LookLeft, LookRight,
    Throttle,
    RollLeft, RollRight,
    Count
};

enum class SourceKind : uint8_t { None, Key, PadButton, PadAxis, MouseButton, MouseAxis, JoyButton, JoyAxis, JoyHat };

// One physical input bound to a target.
//   Key:         code = SDL_Scancode
//   PadButton:   code = SDL_GameControllerButton
//   PadAxis:     code = SDL_GameControllerAxis, dir = +1/-1 selects the half-axis
//   MouseButton: code = SDL mouse button index (SDL_BUTTON_LEFT ...)
//   MouseAxis:   code = 0 (x) or 1 (y),         dir = +1/-1 selects the half-axis
//   JoyButton:   code = button index,           dev = Bindings::joy_devices index
//   JoyAxis:     code = axis index, dir = +1/-1, dev
//   JoyHat:      code = hat index, dir = SDL_HAT_UP/RIGHT/DOWN/LEFT bit, dev
struct Source {
    SourceKind kind = SourceKind::None;
    int32_t    code = 0;
    int8_t     dir  = 0;
    int8_t     dev  = -1;
};

// A raw (non-gamepad) joystick such as a flight stick or HOTAS throttle, identified by GUID
// plus its ordinal among connected devices with the same GUID.
struct JoyDevice {
    std::string guid;
    int         ordinal     = 0;
    std::string name;
    uint32_t    invert_axes = 0;      // bit n flips axis n
    float       deadzone    = 0.05f;  // fraction of each half-axis
};

// Rumble. The game drives its own 13 Rumble Pak effects; these gate and shape the output.
struct RumbleConfig {
    bool  enabled              = true;
    float strength             = 1.0f;
    bool  scale_hits_by_damage = true;
    bool  sustain_death_spiral = true;
    bool  hit                  = true;   // effect 0
    bool  collision            = true;   // 1-3
    bool  object_collision     = true;   // 4-6
    bool  terrain_scrape       = true;   // 7
    bool  weapons              = true;   // 8-10
    bool  death_spiral         = true;   // 11
    bool  crash                = true;   // 12
};

// DualShock 4 / DualSense lightbar colors (0xRRGGBB) by game state. In a mission the color follows
// health: full -> damaged (half) -> critical (empty), flashing on hits and strobing during the death spiral.
struct LightbarConfig {
    bool     enabled   = true;
    bool     health    = true;
    bool     hit_flash = true;
    uint32_t menu      = 0x193792;  // #193792
    uint32_t cinematic = 0xC05A08;  // #C05A08
    uint32_t mission   = 0x00E050;  // #00E050
    uint32_t damaged   = 0xFFB000;  // #FFB000
    uint32_t critical  = 0xFF1A00;  // #FF1A00
    uint32_t hit       = 0xFFFFFF;  // #FFFFFF
    uint32_t death     = 0xFF0000;  // #FF0000
    uint32_t dead      = 0x200000;  // #200000
};

struct Bindings {
    std::vector<Source> targets[(int)Target::Count];
    std::vector<JoyDevice> joy_devices;
    float mouse_sensitivity = 0.05f;  // relative-pixel -> stick deflection
    float mouse_smoothing   = 0.3f;   // 0 = raw per-frame delta, higher = softer onset/recenter (time constant, ~ms/100)
    float mouse_curve       = 1.0f;   // response exponent on [0,1] deflection; >1 eases small movements
    bool  mouse_invert_x    = false;
    bool  mouse_invert_y    = false;
    bool  keyboard_enabled  = true;   // report a controller and read the keyboard
    bool  joystick_enabled  = true;   // read connected raw joysticks
    float stick_range       = 80.0f;  // raw N64 stick value at full gamepad/joystick deflection (hardware ~80; the game saturates at 75/80)
    float throttle_cruise   = 0.5f;   // throttle position [0,1] that gives the craft's cruise speed
    RumbleConfig rumble;
    LightbarConfig lightbar;
};

// Snapshot of live device state handed to resolve() each poll.
struct RawState {
    const uint8_t*          keys         = nullptr;  // SDL_GetKeyboardState array (by scancode)
    int                     keys_len     = 0;
    _SDL_GameController*     pad          = nullptr;  // null if no gamepad
    _SDL_Joystick* const*   joys         = nullptr;  // indexed like Bindings::joy_devices; null entries are disconnected
    int                     joys_len     = 0;
    float                   mouse_dx     = 0.0f;      // relative motion this frame (already smoothed by caller)
    float                   mouse_dy     = 0.0f;
    float                   mouse_curve  = 1.0f;       // response exponent applied to mouse-axis deflection
    uint32_t                mouse_buttons= 0;          // SDL_GetMouseState button mask
    bool                    mouse_active = false;      // relative capture engaged
};

Bindings default_bindings();

// UI helpers.
int         target_count();                    // == (int)Target::Count
const char* target_label(Target t);            // "A", "StickLeft", ...
std::string source_label(const Source& s);     // "Space", "pad:a", "mouse:left", "mouseX+", "J1 axis2-"

// Resolve device state into an N64 button mask + analog x/y in [-1,1].
// Returns true if the profile is an active input source (keyboard_enabled, a
// gamepad, or a connected joystick), false to signal "no controller".
bool resolve(const Bindings& b, const RawState& s, uint16_t* buttons, float* x, float* y);

// Throttle lever position in [0,1] (0 = back, 1 = forward) from the first connected Throttle bind, or -1 if none.
float throttle_position(const Bindings& b, const RawState& s);

// Target speed for throttle position p: back = min, p == cruise_at = cruise, forward = max (piecewise linear).
float throttle_speed(float p, float cruise_at, float min_speed, float cruise_speed, float max_speed);

// Joystick axis in [-1,1] with the device's invert applied (no deadzone). 0 if disconnected.
float joy_axis(const Bindings& b, const RawState& s, int dev, int axis);

// Index of the device with this GUID + ordinal, adding it if new (*added = true).
int find_or_add_joy_device(Bindings& b, const std::string& guid, int ordinal, const std::string& name, bool* added);

// Bind the PC version's (Rogue Squadron 3D) joystick layout for a newly seen device: X/Y steer,
// hat looks, buttons fire/secondary/thrust/brake/special/fire mode/roll/view. A device named
// "throttle", or a stick with exactly three axes (X, Y, throttle), gets its throttle axis bound
// to Throttle (positional speed). Other axes are left for the Controls window.
void add_joystick_defaults(Bindings& b, int dev, int num_axes, int num_buttons, int num_hats);

// roguesq_input.json next to the executable.
std::string default_config_path();

// Parse a bindings JSON file into `b`. Returns false if the file is missing or
// unparseable (b is left untouched); malformed individual sources are skipped.
bool load_bindings(Bindings& b, const std::string& path);

// Write `b` to `path` (human-readable source names). Returns false on I/O error.
bool save_bindings(const Bindings& b, const std::string& path);

} // namespace rs64::input

#endif
