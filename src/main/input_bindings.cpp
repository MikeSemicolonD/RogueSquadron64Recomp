#include "input_bindings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "json/json.hpp"

namespace rs64::input {

// N64 button bitmasks (mirrors main.cpp).
namespace {
constexpr uint16_t A      = 0x8000;
constexpr uint16_t B      = 0x4000;
constexpr uint16_t Z      = 0x2000;
constexpr uint16_t START  = 0x1000;
constexpr uint16_t JU     = 0x0800;
constexpr uint16_t JD     = 0x0400;
constexpr uint16_t JL     = 0x0200;
constexpr uint16_t JR     = 0x0100;
constexpr uint16_t LT     = 0x0020;
constexpr uint16_t RT     = 0x0010;
constexpr uint16_t CU     = 0x0008;
constexpr uint16_t CD     = 0x0004;
constexpr uint16_t CL     = 0x0002;
constexpr uint16_t CR     = 0x0001;

// Button targets -> N64 mask. Stick targets carry no mask (handled as axes).
uint16_t target_mask(Target t) {
    switch (t) {
        case Target::A:        return A;
        case Target::B:        return B;
        case Target::Z:        return Z;
        case Target::Start:    return START;
        case Target::DpadUp:   return JU;
        case Target::DpadDown: return JD;
        case Target::DpadLeft: return JL;
        case Target::DpadRight:return JR;
        case Target::LTrig:    return LT;
        case Target::RTrig:    return RT;
        case Target::CUp:      return CU;
        case Target::CDown:    return CD;
        case Target::CLeft:    return CL;
        case Target::CRight:   return CR;
        default:               return 0;
    }
}

constexpr float PAD_DEADZONE = 0.15f;

// Targets that end up on the N64 stick, so gamepad/joystick axes on them get the stick-range scale.
bool is_stick_target(int t) {
    return (t >= (int)Target::StickUp && t <= (int)Target::StickRight) || t == (int)Target::RollLeft || t == (int)Target::RollRight;
}

// Axis sources sum within a direction (stick X + twist rudder); keys and buttons take the max.
bool is_axis_source(SourceKind k) {
    return k == SourceKind::PadAxis || k == SourceKind::JoyAxis || k == SourceKind::MouseAxis;
}

_SDL_Joystick* joy_of(const Source& s, const RawState& st) {
    if (s.dev < 0 || s.dev >= st.joys_len || !st.joys) return nullptr;
    return st.joys[s.dev];
}

// Deflection [0,1] contributed by one source toward its target's direction.
float source_value(const Source& s, const RawState& st, const Bindings& b) {
    switch (s.kind) {
        case SourceKind::Key:
            if (st.keys && s.code >= 0 && s.code < st.keys_len)
                return st.keys[s.code] ? 1.0f : 0.0f;
            return 0.0f;
        case SourceKind::PadButton:
            if (st.pad && SDL_GameControllerGetButton(st.pad, (SDL_GameControllerButton)s.code))
                return 1.0f;
            return 0.0f;
        case SourceKind::PadAxis: {
            if (!st.pad) return 0.0f;
            float v = SDL_GameControllerGetAxis(st.pad, (SDL_GameControllerAxis)s.code) / 32767.0f;
            v *= (float)s.dir;                 // select requested half
            if (v <= PAD_DEADZONE) return 0.0f;
            return std::min(1.0f, (v - PAD_DEADZONE) / (1.0f - PAD_DEADZONE));
        }
        case SourceKind::MouseButton:
            return (st.mouse_buttons & SDL_BUTTON((uint32_t)s.code)) ? 1.0f : 0.0f;
        case SourceKind::MouseAxis: {
            if (!st.mouse_active) return 0.0f;
            float d = (s.code == 0) ? st.mouse_dx : st.mouse_dy;
            d *= (float)s.dir;                 // select requested half
            if (d <= 0.0f) return 0.0f;
            float m = std::min(1.0f, d);
            return st.mouse_curve != 1.0f ? std::pow(m, st.mouse_curve) : m;
        }
        case SourceKind::JoyButton: {
            _SDL_Joystick* j = b.joystick_enabled ? joy_of(s, st) : nullptr;
            return (j && SDL_JoystickGetButton(j, s.code)) ? 1.0f : 0.0f;
        }
        case SourceKind::JoyHat: {
            _SDL_Joystick* j = b.joystick_enabled ? joy_of(s, st) : nullptr;
            return (j && (SDL_JoystickGetHat(j, s.code) & (uint8_t)s.dir)) ? 1.0f : 0.0f;
        }
        case SourceKind::JoyAxis: {
            if (!b.joystick_enabled || s.dev < 0 || s.dev >= (int)b.joy_devices.size()) return 0.0f;
            float v = joy_axis(b, st, s.dev, s.code) * (float)s.dir;
            float dz = std::clamp(b.joy_devices[s.dev].deadzone, 0.0f, 0.95f);
            if (v <= dz) return 0.0f;
            return std::min(1.0f, (v - dz) / (1.0f - dz));
        }
        default:
            return 0.0f;
    }
}
} // namespace

float joy_axis(const Bindings& b, const RawState& st, int dev, int axis) {
    if (dev < 0 || dev >= (int)b.joy_devices.size() || dev >= st.joys_len || !st.joys || !st.joys[dev]) return 0.0f;
    float v = std::max(-1.0f, SDL_JoystickGetAxis(st.joys[dev], axis) / 32767.0f);
    if (axis < 32 && (b.joy_devices[dev].invert_axes >> axis) & 1u) v = -v;
    return v;
}

float throttle_position(const Bindings& b, const RawState& st) {
    if (!b.joystick_enabled) return -1.0f;
    for (const Source& s : b.targets[(int)Target::Throttle]) {
        if (s.kind == SourceKind::JoyAxis && joy_of(s, st) && s.dev < (int)b.joy_devices.size()) {
            return std::clamp((joy_axis(b, st, s.dev, s.code) * (float)s.dir + 1.0f) * 0.5f, 0.0f, 1.0f);
        }
        if (s.kind == SourceKind::PadAxis && st.pad) {
            float v = SDL_GameControllerGetAxis(st.pad, (SDL_GameControllerAxis)s.code) / 32767.0f;
            return std::clamp((v * (float)s.dir + 1.0f) * 0.5f, 0.0f, 1.0f);
        }
    }
    return -1.0f;
}

float throttle_speed(float p, float cruise_at, float min_speed, float cruise_speed, float max_speed) {
    p = std::clamp(p, 0.0f, 1.0f);
    const float c = std::clamp(cruise_at, 0.05f, 0.95f);
    if (!(cruise_speed >= min_speed && cruise_speed <= max_speed)) cruise_speed = 0.5f * (min_speed + max_speed);
    if (p <= c) return min_speed + (cruise_speed - min_speed) * (p / c);
    return cruise_speed + (max_speed - cruise_speed) * ((p - c) / (1.0f - c));
}

int find_or_add_joy_device(Bindings& b, const std::string& guid, int ordinal, const std::string& name, bool* added) {
    for (size_t i = 0; i < b.joy_devices.size(); ++i) {
        if (b.joy_devices[i].guid == guid && b.joy_devices[i].ordinal == ordinal) {
            if (added) *added = false;
            if (!name.empty()) b.joy_devices[i].name = name;
            return (int)i;
        }
    }
    if (b.joy_devices.size() >= 127) {
        if (added) *added = false;
        return -1;
    }
    JoyDevice d;
    d.guid = guid;
    d.ordinal = ordinal;
    d.name = name;
    b.joy_devices.push_back(d);
    if (added) *added = true;
    return (int)b.joy_devices.size() - 1;
}

void add_joystick_defaults(Bindings& b, int dev, int num_axes, int num_buttons, int num_hats) {
    if (dev < 0 || dev >= (int)b.joy_devices.size()) return;
    auto add = [&](Target t, SourceKind k, int code, int dir) { b.targets[(int)t].push_back(Source{ k, code, (int8_t)dir, (int8_t)dev }); };
    std::string lname = b.joy_devices[dev].name;
    std::transform(lname.begin(), lname.end(), lname.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const bool throttle_unit = lname.find("throttle") != std::string::npos;
    // Combined stick+throttle units in DirectInput order (X, Y, Z throttle, Rx, Ry, Rz twist, slider).
    const bool x52 = lname.find("x52") != std::string::npos;

    // Throttle lever: forward is negative on most devices.
    int throttle_axis = throttle_unit ? 0 : (num_axes == 3 ? 2 : -1);
    int twist_axis = -1;
    if (x52 && num_axes >= 6) {
        throttle_axis = 2;
        twist_axis = 5;
    }
    if (throttle_axis >= 0 && throttle_axis < num_axes) {
        add(Target::Throttle, SourceKind::JoyAxis, throttle_axis, -1);
    }
    if (twist_axis >= 0 && twist_axis < num_axes) {
        add(Target::RollLeft,  SourceKind::JoyAxis, twist_axis, -1);
        add(Target::RollRight, SourceKind::JoyAxis, twist_axis, +1);
    }
    if (!throttle_unit && num_axes >= 2) {
        add(Target::StickLeft,  SourceKind::JoyAxis, 0, -1);
        add(Target::StickRight, SourceKind::JoyAxis, 0, +1);
        add(Target::StickUp,    SourceKind::JoyAxis, 1, -1);
        add(Target::StickDown,  SourceKind::JoyAxis, 1, +1);
    }
    if (num_hats > 0) {
        add(Target::LookUp,    SourceKind::JoyHat, 0, 0x01);
        add(Target::LookRight, SourceKind::JoyHat, 0, 0x02);
        add(Target::LookDown,  SourceKind::JoyHat, 0, 0x04);
        add(Target::LookLeft,  SourceKind::JoyHat, 0, 0x08);
    }
    // Luke preset actions: B fire, C-Left secondary, A thrust, Z brake, C-Right special, C-Down fire mode, R roll, L switch view.
    const Target btn_targets[] = { Target::B, Target::CLeft, Target::A, Target::Z, Target::CRight, Target::CDown, Target::RTrig, Target::LTrig };
    if (throttle_unit) return;
    for (int i = 0; i < num_buttons && i < (int)(sizeof(btn_targets) / sizeof(btn_targets[0])); ++i) {
        add(btn_targets[i], SourceKind::JoyButton, i, 0);
    }
}

Bindings default_bindings() {
    Bindings b;
    auto add = [&](Target t, Source s) { b.targets[(int)t].push_back(s); };
    auto key = [](int sc) { return Source{ SourceKind::Key, sc, 0 }; };
    auto pb  = [](int c)  { return Source{ SourceKind::PadButton, c, 0 }; };
    auto pax = [](int c, int d) { return Source{ SourceKind::PadAxis, c, (int8_t)d }; };
    auto mb  = [](int c)  { return Source{ SourceKind::MouseButton, c, 0 }; };
    auto max = [](int c, int d) { return Source{ SourceKind::MouseAxis, c, (int8_t)d }; };

    // --- Keyboard ---
    // PC-port (Rogue Squadron 3D) layout, mapped through the game's default
    // "Luke" controller preset (ROM table 0x9EA18).
    add(Target::StickUp,    key(SDL_SCANCODE_UP));
    add(Target::StickDown,  key(SDL_SCANCODE_DOWN));
    add(Target::StickLeft,  key(SDL_SCANCODE_LEFT));  add(Target::StickLeft,  key(SDL_SCANCODE_A));
    add(Target::StickRight, key(SDL_SCANCODE_RIGHT)); add(Target::StickRight, key(SDL_SCANCODE_D));
    add(Target::A,     key(SDL_SCANCODE_W));        // thrust
    add(Target::A,     key(SDL_SCANCODE_RETURN));   // menu confirm
    add(Target::B,     key(SDL_SCANCODE_SPACE));    // fire blasters
    add(Target::B,     key(SDL_SCANCODE_BACKSPACE));// menu back
    add(Target::Z,     key(SDL_SCANCODE_S));        // brake
    add(Target::RTrig, key(SDL_SCANCODE_E));        // roll
    add(Target::CLeft, key(SDL_SCANCODE_LALT));     // fire secondary
    add(Target::CLeft, key(SDL_SCANCODE_RALT));
    add(Target::CDown, key(SDL_SCANCODE_X));        // fire mode
    add(Target::CRight,key(SDL_SCANCODE_F));        // special
    add(Target::Start, key(SDL_SCANCODE_ESCAPE));   // pause
    add(Target::DpadUp,   key(SDL_SCANCODE_F1));    // cockpit view
    add(Target::DpadDown, key(SDL_SCANCODE_F2));    // standard view
    add(Target::DpadRight,key(SDL_SCANCODE_F3));    // close view
    add(Target::LTrig,    key(SDL_SCANCODE_F4));    // switch view
    add(Target::CUp,      key(SDL_SCANCODE_Q));     // look around (F5 = profiler HUD host hotkey)
    add(Target::DpadLeft, key(SDL_SCANCODE_Z));     // drop camera

    // --- Mouse (flight steering) ---
    add(Target::StickRight, max(0, +1)); add(Target::StickLeft, max(0, -1));
    add(Target::StickDown,  max(1, +1)); add(Target::StickUp,   max(1, -1));  // SDL y-down -> N64 up
    add(Target::B,     mb(SDL_BUTTON_LEFT));        // fire blasters
    add(Target::CLeft, mb(SDL_BUTTON_RIGHT));       // fire secondary
    add(Target::A,     mb(SDL_BUTTON_MIDDLE));      // menu confirm

    // --- Gamepad (mirrors the prior hardcoded map) ---
    add(Target::A,     pb(SDL_CONTROLLER_BUTTON_A));
    add(Target::B,     pb(SDL_CONTROLLER_BUTTON_X));
    add(Target::Start, pb(SDL_CONTROLLER_BUTTON_START));
    add(Target::DpadUp,   pb(SDL_CONTROLLER_BUTTON_DPAD_UP));
    add(Target::DpadDown, pb(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    add(Target::DpadLeft, pb(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    add(Target::DpadRight,pb(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
    add(Target::LTrig, pb(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    add(Target::RTrig, pb(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    add(Target::CUp,   pb(SDL_CONTROLLER_BUTTON_Y));
    add(Target::CDown, pb(SDL_CONTROLLER_BUTTON_B));
    add(Target::CLeft, pb(SDL_CONTROLLER_BUTTON_BACK));
    add(Target::CRight,pb(SDL_CONTROLLER_BUTTON_GUIDE));
    add(Target::Z,     pax(SDL_CONTROLLER_AXIS_TRIGGERLEFT, +1));
    add(Target::StickRight, pax(SDL_CONTROLLER_AXIS_LEFTX, +1));
    add(Target::StickLeft,  pax(SDL_CONTROLLER_AXIS_LEFTX, -1));
    add(Target::StickUp,    pax(SDL_CONTROLLER_AXIS_LEFTY, -1));
    add(Target::StickDown,  pax(SDL_CONTROLLER_AXIS_LEFTY, +1));

    return b;
}

bool resolve(const Bindings& b, const RawState& s, uint16_t* buttons, float* x, float* y) {
    bool any_joy = false;
    for (int i = 0; b.joystick_enabled && s.joys && i < s.joys_len && !any_joy; ++i) any_joy = s.joys[i] != nullptr;
    if (!b.keyboard_enabled && s.pad == nullptr && !any_joy) return false;

    RawState st = s;
    if (b.mouse_invert_x) st.mouse_dx = -st.mouse_dx;
    if (b.mouse_invert_y) st.mouse_dy = -st.mouse_dy;
    st.mouse_dx *= b.mouse_sensitivity;
    st.mouse_dy *= b.mouse_sensitivity;
    st.mouse_curve = b.mouse_curve;

    // Full gamepad/joystick deflection maps to the N64's ~80 rather than 127 (the host scales x by 127), so the throw isn't clipped.
    const float range = std::clamp(b.stick_range, 1.0f, 127.0f) / 127.0f;

    uint16_t btn = 0;
    float defl[(int)Target::Count] = {0};
    for (int t = 0; t < (int)Target::Count; ++t) {
        float dig = 0.0f, ana = 0.0f;
        for (const Source& src : b.targets[t]) {
            float v = source_value(src, st, b);
            if (!is_axis_source(src.kind)) {
                dig = std::max(dig, v);
                continue;
            }
            if (is_stick_target(t) && src.kind != SourceKind::MouseAxis) v *= range;
            ana += v;
        }
        float v = std::max(dig, std::min(1.0f, ana));
        defl[t] = v;
        uint16_t mask = target_mask((Target)t);
        if (mask && v > 0.5f) btn |= mask;
    }

    float sx = defl[(int)Target::StickRight] - defl[(int)Target::StickLeft];
    float sy = defl[(int)Target::StickUp]    - defl[(int)Target::StickDown];

    // Look-around: hold C-Up and point the stick, overriding steering while a look input is held.
    float lx = defl[(int)Target::LookRight] - defl[(int)Target::LookLeft];
    float ly = defl[(int)Target::LookUp]    - defl[(int)Target::LookDown];
    const bool looking = std::fabs(lx) > 0.5f || std::fabs(ly) > 0.5f;
    if (looking) {
        btn |= CU;
        sx = lx;
        sy = ly;
    }

    // Roll: while the Roll button (R) is held the game turns stick X into pure roll; a deflected roll axis then takes over stick X.
    const float roll = defl[(int)Target::RollRight] - defl[(int)Target::RollLeft];
    if (!looking && (btn & RT) && std::fabs(roll) > 0.1f) {
        sx = roll;
    }

    *x = std::clamp(sx, -1.0f, 1.0f);
    *y = std::clamp(sy, -1.0f, 1.0f);
    *buttons = btn;
    return true;
}

// --- Persistence (roguesq_input.json) --------------------------------------
namespace {
using nlohmann::json;

std::vector<std::pair<const char*, uint32_t*>> lightbar_colors(LightbarConfig& c) {
    return { {"menu", &c.menu}, {"cinematic", &c.cinematic}, {"mission", &c.mission}, {"damaged", &c.damaged},
             {"critical", &c.critical}, {"hit", &c.hit}, {"death", &c.death}, {"dead", &c.dead} };
}

// "#RRGGBB" or "RRGGBB"; returns fallback when malformed.
uint32_t parse_hex_color(const std::string& s, uint32_t fallback) {
    const char* p = s.c_str();
    if (*p == '#') ++p;
    if (std::strlen(p) != 6) return fallback;
    char* end = nullptr;
    const unsigned long v = std::strtoul(p, &end, 16);
    return (end && *end == '\0') ? (uint32_t)v : fallback;
}

const char* target_name(Target t) {
    switch (t) {
        case Target::A: return "A";                 case Target::B: return "B";
        case Target::Z: return "Z";                 case Target::Start: return "Start";
        case Target::DpadUp: return "DpadUp";       case Target::DpadDown: return "DpadDown";
        case Target::DpadLeft: return "DpadLeft";   case Target::DpadRight: return "DpadRight";
        case Target::LTrig: return "LTrig";         case Target::RTrig: return "RTrig";
        case Target::CUp: return "CUp";             case Target::CDown: return "CDown";
        case Target::CLeft: return "CLeft";         case Target::CRight: return "CRight";
        case Target::StickUp: return "StickUp";     case Target::StickDown: return "StickDown";
        case Target::StickLeft: return "StickLeft"; case Target::StickRight: return "StickRight";
        case Target::LookUp: return "LookUp";       case Target::LookDown: return "LookDown";
        case Target::LookLeft: return "LookLeft";   case Target::LookRight: return "LookRight";
        case Target::Throttle: return "Throttle";
        case Target::RollLeft: return "RollLeft";   case Target::RollRight: return "RollRight";
        default: return "";
    }
}

const char* hat_dir_name(int d) {
    switch (d) {
        case 0x01: return "up";   case 0x02: return "right";
        case 0x04: return "down"; case 0x08: return "left";
        default:   return "";
    }
}
int hat_dir_code(const std::string& s) {
    if (s == "up") return 0x01;   if (s == "right") return 0x02;
    if (s == "down") return 0x04; if (s == "left") return 0x08;
    return 0;
}

// "axis3" / "button12" / "hat0" -> 3 / 12 / 0, or -1 if `name` doesn't start with `prefix`.
int indexed_name(const std::string& name, const char* prefix) {
    size_t n = strlen(prefix);
    if (name.compare(0, n, prefix) != 0 || name.size() == n) return -1;
    for (size_t i = n; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return -1;
    return std::atoi(name.c_str() + n);
}

const char* mouse_button_name(int c) {
    switch (c) {
        case SDL_BUTTON_LEFT: return "left";   case SDL_BUTTON_MIDDLE: return "middle";
        case SDL_BUTTON_RIGHT: return "right"; case SDL_BUTTON_X1: return "x1";
        case SDL_BUTTON_X2: return "x2";       default: return "";
    }
}
int mouse_button_code(const std::string& s) {
    if (s == "left") return SDL_BUTTON_LEFT;   if (s == "middle") return SDL_BUTTON_MIDDLE;
    if (s == "right") return SDL_BUTTON_RIGHT; if (s == "x1") return SDL_BUTTON_X1;
    if (s == "x2") return SDL_BUTTON_X2;       return 0;
}

// Serialize one source to {kind, name, dir?}. Returns false if unrepresentable.
bool source_to_json(const Source& s, json& o) {
    switch (s.kind) {
        case SourceKind::Key: {
            const char* n = SDL_GetScancodeName((SDL_Scancode)s.code);
            if (!n || !n[0]) return false;
            o = { {"kind","key"}, {"name", n} };
            return true;
        }
        case SourceKind::PadButton: {
            const char* n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)s.code);
            if (!n || !n[0]) return false;
            o = { {"kind","padbutton"}, {"name", n} };
            return true;
        }
        case SourceKind::PadAxis: {
            const char* n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)s.code);
            if (!n || !n[0]) return false;
            o = { {"kind","padaxis"}, {"name", n}, {"dir", (int)s.dir} };
            return true;
        }
        case SourceKind::MouseButton:
            o = { {"kind","mousebutton"}, {"name", mouse_button_name(s.code)} };
            return true;
        case SourceKind::MouseAxis:
            o = { {"kind","mouseaxis"}, {"name", s.code == 0 ? "x" : "y"}, {"dir", (int)s.dir} };
            return true;
        case SourceKind::JoyButton:
            o = { {"kind","joybutton"}, {"device", (int)s.dev}, {"name", "button" + std::to_string(s.code)} };
            return true;
        case SourceKind::JoyAxis:
            o = { {"kind","joyaxis"}, {"device", (int)s.dev}, {"name", "axis" + std::to_string(s.code)}, {"dir", (int)s.dir} };
            return true;
        case SourceKind::JoyHat:
            o = { {"kind","joyhat"}, {"device", (int)s.dev}, {"name", "hat" + std::to_string(s.code)}, {"dir", hat_dir_name(s.dir)} };
            return true;
        default:
            return false;
    }
}

bool source_from_json(const json& o, Source& s) {
    if (!o.is_object() || !o.contains("kind") || !o.contains("name")) return false;
    if (!o["kind"].is_string() || !o["name"].is_string()) return false;
    std::string kind = o["kind"].get<std::string>();
    std::string name = o["name"].get<std::string>();
    if (kind == "joybutton" || kind == "joyaxis" || kind == "joyhat") {
        int dev = o.value("device", -1);
        if (dev < 0 || dev > 126) return false;
        if (kind == "joybutton") {
            int i = indexed_name(name, "button");
            if (i < 0) return false;
            s = { SourceKind::JoyButton, i, 0, (int8_t)dev };
        } else if (kind == "joyaxis") {
            int i = indexed_name(name, "axis");
            int d = o.value("dir", 0);
            if (i < 0 || (d != 1 && d != -1)) return false;
            s = { SourceKind::JoyAxis, i, (int8_t)d, (int8_t)dev };
        } else {
            int i = indexed_name(name, "hat");
            int d = (o.contains("dir") && o["dir"].is_string()) ? hat_dir_code(o["dir"].get<std::string>()) : 0;
            if (i < 0 || !d) return false;
            s = { SourceKind::JoyHat, i, (int8_t)d, (int8_t)dev };
        }
        return true;
    }
    int dir = o.value("dir", 0);
    if (kind == "key") {
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        s = { SourceKind::Key, (int)sc, 0 };
    } else if (kind == "padbutton") {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name.c_str());
        if (b == SDL_CONTROLLER_BUTTON_INVALID) return false;
        s = { SourceKind::PadButton, (int)b, 0 };
    } else if (kind == "padaxis") {
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name.c_str());
        if (a == SDL_CONTROLLER_AXIS_INVALID) return false;
        s = { SourceKind::PadAxis, (int)a, (int8_t)dir };
    } else if (kind == "mousebutton") {
        int c = mouse_button_code(name);
        if (!c) return false;
        s = { SourceKind::MouseButton, c, 0 };
    } else if (kind == "mouseaxis") {
        s = { SourceKind::MouseAxis, (name == "y") ? 1 : 0, (int8_t)dir };
    } else {
        return false;
    }
    return true;
}
} // namespace

int target_count() { return (int)Target::Count; }

const char* target_label(Target t) { return target_name(t); }

std::string source_label(const Source& s) {
    switch (s.kind) {
        case SourceKind::Key: {
            const char* n = SDL_GetScancodeName((SDL_Scancode)s.code);
            return (n && n[0]) ? std::string(n) : std::string("key?");
        }
        case SourceKind::PadButton: {
            const char* n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)s.code);
            return std::string("pad:") + ((n && n[0]) ? n : "?");
        }
        case SourceKind::PadAxis: {
            const char* n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)s.code);
            return std::string("pad:") + ((n && n[0]) ? n : "?") + (s.dir >= 0 ? "+" : "-");
        }
        case SourceKind::MouseButton:
            return std::string("mouse:") + mouse_button_name(s.code);
        case SourceKind::MouseAxis:
            return std::string("mouse") + (s.code == 0 ? "X" : "Y") + (s.dir >= 0 ? "+" : "-");
        case SourceKind::JoyButton:
            return "J" + std::to_string(s.dev + 1) + " btn" + std::to_string(s.code);
        case SourceKind::JoyAxis:
            return "J" + std::to_string(s.dev + 1) + " axis" + std::to_string(s.code) + (s.dir >= 0 ? "+" : "-");
        case SourceKind::JoyHat:
            return "J" + std::to_string(s.dev + 1) + " hat" + std::to_string(s.code) + " " + hat_dir_name(s.dir);
        default:
            return "?";
    }
}

std::string default_config_path() {
    std::string dir;
    char* base = SDL_GetBasePath();
    if (base) { dir = base; SDL_free(base); }
    return dir + "roguesq_input.json";
}

bool load_bindings(Bindings& b, const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    json j;
    try { f >> j; } catch (...) { return false; }

    if (j.contains("mouse") && j["mouse"].is_object()) {
        const json& m = j["mouse"];
        b.mouse_sensitivity = m.value("sensitivity", b.mouse_sensitivity);
        b.mouse_smoothing   = m.value("smoothing", b.mouse_smoothing);
        b.mouse_curve       = m.value("curve", b.mouse_curve);
        b.mouse_invert_x    = m.value("invert_x", b.mouse_invert_x);
        b.mouse_invert_y    = m.value("invert_y", b.mouse_invert_y);
    }
    b.keyboard_enabled = j.value("keyboard_enabled", b.keyboard_enabled);
    b.joystick_enabled = j.value("joystick_enabled", b.joystick_enabled);
    b.stick_range      = j.value("stick_range", b.stick_range);
    b.throttle_cruise  = j.value("throttle_cruise", b.throttle_cruise);

    if (j.contains("rumble") && j["rumble"].is_object()) {
        const json& r = j["rumble"];
        RumbleConfig& c = b.rumble;
        c.enabled              = r.value("enabled", c.enabled);
        c.strength             = r.value("strength", c.strength);
        c.scale_hits_by_damage = r.value("scale_hits_by_damage", c.scale_hits_by_damage);
        c.sustain_death_spiral = r.value("sustain_death_spiral", c.sustain_death_spiral);
        if (r.contains("effects") && r["effects"].is_object()) {
            const json& e = r["effects"];
            c.hit              = e.value("hit", c.hit);
            c.collision        = e.value("collision", c.collision);
            c.object_collision = e.value("object_collision", c.object_collision);
            c.terrain_scrape   = e.value("terrain_scrape", c.terrain_scrape);
            c.weapons          = e.value("weapons", c.weapons);
            c.death_spiral     = e.value("death_spiral", c.death_spiral);
            c.crash            = e.value("crash", c.crash);
        }
    }

    if (j.contains("lightbar") && j["lightbar"].is_object()) {
        const json& l = j["lightbar"];
        LightbarConfig& c = b.lightbar;
        c.enabled   = l.value("enabled", c.enabled);
        c.health    = l.value("health", c.health);
        c.hit_flash = l.value("hit_flash", c.hit_flash);
        if (l.contains("colors") && l["colors"].is_object()) {
            const json& k = l["colors"];
            for (auto& [name, field] : lightbar_colors(c)) {
                if (k.contains(name) && k[name].is_string()) *field = parse_hex_color(k[name].get<std::string>(), *field);
            }
        }
    }

    if (j.contains("joysticks") && j["joysticks"].is_array()) {
        b.joy_devices.clear();
        for (const json& d : j["joysticks"]) {
            if (!d.is_object() || !d.contains("guid") || !d["guid"].is_string()) continue;
            JoyDevice jd;
            jd.guid        = d["guid"].get<std::string>();
            jd.ordinal     = d.value("ordinal", 0);
            jd.name        = d.value("name", std::string());
            jd.invert_axes = d.value("invert_axes", 0u);
            jd.deadzone    = d.value("deadzone", jd.deadzone);
            b.joy_devices.push_back(jd);
        }
    }

    if (j.contains("binds") && j["binds"].is_object()) {
        for (int t = 0; t < (int)Target::Count; ++t) {
            const char* name = target_name((Target)t);
            if (!j["binds"].contains(name)) continue;   // absent -> keep default
            b.targets[t].clear();
            for (const json& e : j["binds"][name]) {
                Source s;
                if (!source_from_json(e, s)) continue;
                if (s.dev >= (int)b.joy_devices.size()) continue;
                b.targets[t].push_back(s);
            }
        }
    }
    return true;
}

bool save_bindings(const Bindings& b, const std::string& path) {
    json j;
    j["mouse"] = { {"sensitivity", b.mouse_sensitivity},
                   {"smoothing", b.mouse_smoothing}, {"curve", b.mouse_curve},
                   {"invert_x", b.mouse_invert_x}, {"invert_y", b.mouse_invert_y} };
    j["keyboard_enabled"] = b.keyboard_enabled;
    j["joystick_enabled"] = b.joystick_enabled;
    j["stick_range"] = b.stick_range;
    j["throttle_cruise"] = b.throttle_cruise;
    const RumbleConfig& c = b.rumble;
    j["rumble"] = { {"enabled", c.enabled}, {"strength", c.strength},
                    {"scale_hits_by_damage", c.scale_hits_by_damage}, {"sustain_death_spiral", c.sustain_death_spiral},
                    {"effects", { {"hit", c.hit}, {"collision", c.collision}, {"object_collision", c.object_collision},
                                  {"terrain_scrape", c.terrain_scrape}, {"weapons", c.weapons},
                                  {"death_spiral", c.death_spiral}, {"crash", c.crash} }} };
    LightbarConfig lb = b.lightbar;
    json colors = json::object();
    for (auto& [name, field] : lightbar_colors(lb)) {
        char hex[8];
        snprintf(hex, sizeof(hex), "#%06X", (unsigned)(*field & 0xFFFFFF));
        colors[name] = hex;
    }
    j["lightbar"] = { {"enabled", lb.enabled}, {"health", lb.health}, {"hit_flash", lb.hit_flash}, {"colors", colors} };
    json joys = json::array();
    for (const JoyDevice& d : b.joy_devices) {
        joys.push_back({ {"guid", d.guid}, {"ordinal", d.ordinal}, {"name", d.name},
                         {"invert_axes", d.invert_axes}, {"deadzone", d.deadzone} });
    }
    j["joysticks"] = joys;
    json binds = json::object();
    for (int t = 0; t < (int)Target::Count; ++t) {
        json arr = json::array();
        for (const Source& s : b.targets[t]) {
            json o;
            if (source_to_json(s, o)) arr.push_back(o);
        }
        binds[target_name((Target)t)] = arr;
    }
    j["binds"] = binds;

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << j.dump(2) << "\n";
    return f.good();
}

} // namespace rs64::input
