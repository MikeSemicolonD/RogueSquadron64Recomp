#include "input/input_bindings.h"
#include "util/settings_file.h"

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

namespace {
// Button targets -> N64 mask. Stick targets carry no mask (handled as axes).
uint16_t target_mask(Target t) {

    switch (t) {
        case Target::A:        return N64_A_BUTTON;
        case Target::B:        return N64_B_BUTTON;
        case Target::Z:        return N64_Z_TRIG;
        case Target::Start:    return N64_START_BUTTON;
        case Target::DpadUp:   return N64_U_JPAD;
        case Target::DpadDown: return N64_D_JPAD;
        case Target::DpadLeft: return N64_L_JPAD;
        case Target::DpadRight:return N64_R_JPAD;
        case Target::LTrig:    return N64_L_TRIG;
        case Target::RTrig:    return N64_R_TRIG;
        case Target::CUp:      return N64_U_CBUTTONS;
        case Target::CDown:    return N64_D_CBUTTONS;
        case Target::CLeft:    return N64_L_CBUTTONS;
        case Target::CRight:   return N64_R_CBUTTONS;
        default:               return 0;
    }
}

// Targets that end up on the N64 stick, so gamepad/joystick axes on them get the stick-range scale.
bool is_stick_target(int t)
{
    return (t >= (int)Target::StickUp && t <= (int)Target::StickRight) || t == (int)Target::RollLeft || t == (int)Target::RollRight;
}

// Axis sources sum within a direction (stick X + twist rudder); keys and buttons take the max.
bool is_axis_source(SourceKind k)
{
    return k == SourceKind::PadAxis || k == SourceKind::JoyAxis || k == SourceKind::MouseAxis || k == SourceKind::VrAxis;
}

// The same physical input (a combo's `with` aside).
bool same_input(const Source& p, const Source& q)
{
    return p.kind == q.kind && p.code == q.code && p.dir == q.dir && p.dev == q.dev;
}

// Deflection past a dead zone, rescaled to [0,1].
float past_deadzone(float v, float deadzone)
{

    const float dz = std::clamp(deadzone, 0.0f, 0.95f);

    return (v <= dz) ? 0.0f : std::min(1.0f, (v - dz) / (1.0f - dz));
}

_SDL_Joystick* joy_of(const Source& s, const RawState& st) {

    if (s.dev < 0 || s.dev >= st.joys_len || !st.joys) return nullptr;

    return st.joys[s.dev];
}

// Deflection [0,1] contributed by one source toward its target's direction.
float source_value(const Source& s, const RawState& st, const Bindings& b) {

    switch (s.kind)
    {
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

            const float dz = std::clamp(b.pad_deadzone, 0.0f, 0.95f);

            if (v <= dz) return 0.0f;

            return std::min(1.0f, (v - dz) / (1.0f - dz));
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
        case SourceKind::VrButton:
            return (st.vr && (st.vr_buttons >> s.code & 1u)) ? 1.0f : 0.0f;
        case SourceKind::VrAxis:
            return st.vr ? past_deadzone(((s.code == 0) ? st.vr_x : st.vr_y) * (float)s.dir, b.pad_deadzone) : 0.0f;
        default:
            return 0.0f;
    }
}
} // namespace

Source held_source(const Held& h)
{
    return Source{ h.kind, h.code, h.dir, h.dev };
}

Held as_held(const Source& s)
{
    return Held{ s.kind, s.code, s.dir, s.dev };
}

bool input_down(const Bindings& b, const RawState& s, const Source& src)
{
    return source_value(src, s, b) > 0.5f;
}

float joy_axis(const Bindings& b, const RawState& st, int dev, int axis)
{

    if (dev < 0 || dev >= (int)b.joy_devices.size() || dev >= st.joys_len || !st.joys || !st.joys[dev]) return 0.0f;

    float v = std::max(-1.0f, SDL_JoystickGetAxis(st.joys[dev], axis) / 32767.0f);

    if (axis < 32 && (b.joy_devices[dev].invert_axes >> axis) & 1u) v = -v;

    return v;
}

float throttle_position(const Bindings& b, const RawState& st) {

    if (!b.joystick_enabled) return -1.0f;

    for (const Source& s : b.targets[(int)Target::Throttle])
    {
        if (s.kind == SourceKind::JoyAxis && joy_of(s, st) && s.dev < (int)b.joy_devices.size())
        {
            return std::clamp((joy_axis(b, st, s.dev, s.code) * (float)s.dir + 1.0f) * 0.5f, 0.0f, 1.0f);
        }

        if (s.kind == SourceKind::PadAxis && st.pad)
        {
            float v = SDL_GameControllerGetAxis(st.pad, (SDL_GameControllerAxis)s.code) / 32767.0f;

            return std::clamp((v * (float)s.dir + 1.0f) * 0.5f, 0.0f, 1.0f);
        }
    }

    // No throttle lever: pad axes bound to A (boost) and Z (brake) push the lever from cruise by how far they are pulled.
    auto pull = [&](Target t) {

        float m = 0.0f;

        for (const Source& s : b.targets[(int)t])
        {
            if (s.kind == SourceKind::PadAxis) m = std::max(m, source_value(s, st, b));
        }

        return m;
    };

    const float boost = pull(Target::A);
    const float brake = pull(Target::Z);

    if (boost <= 0.0f && brake <= 0.0f) return -1.0f;

    const float c = std::clamp(b.throttle_cruise, 0.05f, 0.95f);

    return std::clamp(c + boost * (1.0f - c) - brake * c, 0.0f, 1.0f);
}

float throttle_speed(float p, float cruise_at, float min_speed, float cruise_speed, float max_speed)
{

    p = std::clamp(p, 0.0f, 1.0f);
    const float c = std::clamp(cruise_at, 0.05f, 0.95f);

    if (!(cruise_speed >= min_speed && cruise_speed <= max_speed)) cruise_speed = 0.5f * (min_speed + max_speed);
    if (p <= c) return min_speed + (cruise_speed - min_speed) * (p / c);

    return cruise_speed + (max_speed - cruise_speed) * ((p - c) / (1.0f - c));
}

int find_or_add_joy_device(Bindings& b, const std::string& guid, int ordinal, const std::string& name, bool* added)
{

    for (size_t i = 0; i < b.joy_devices.size(); ++i)
    {
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

void add_joystick_defaults(Bindings& b, int dev, int num_axes, int num_buttons, int num_hats)
{

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

    // Twist rolls while the Roll button (the 7th, R below) is held: two combos, one per twist direction.
    constexpr int kRollButton = 6;

    if (twist_axis >= 0 && twist_axis < num_axes) {
        add(Target::RollLeft,  SourceKind::JoyAxis, twist_axis, -1);
        add(Target::RollRight, SourceKind::JoyAxis, twist_axis, +1);

        if (num_buttons > kRollButton && !throttle_unit) {
            b.targets[(int)Target::RollLeft].back().with = Held{ SourceKind::JoyButton, kRollButton, 0, (int8_t)dev };
            b.targets[(int)Target::RollRight].back().with = Held{ SourceKind::JoyButton, kRollButton, 0, (int8_t)dev };
        }
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

    for (int i = 0; i < num_buttons && i < (int)(sizeof(btn_targets) / sizeof(btn_targets[0])); ++i)
    {
        add(btn_targets[i], SourceKind::JoyButton, i, 0);
    }
}

Bindings default_bindings()
{

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

    // Views: C cockpit, V standard, B close, N switch.
    add(Target::DpadUp,   key(SDL_SCANCODE_C));
    add(Target::DpadDown, key(SDL_SCANCODE_V));
    add(Target::DpadRight,key(SDL_SCANCODE_B));
    add(Target::LTrig,    key(SDL_SCANCODE_N));
    add(Target::CUp,      key(SDL_SCANCODE_Q));     // look around (F5 = profiler HUD host hotkey)
    add(Target::DpadLeft, key(SDL_SCANCODE_Z));     // drop camera

    // --- Mouse (flight steering) ---
    add(Target::StickRight, max(0, +1)); add(Target::StickLeft, max(0, -1));
    add(Target::StickDown,  max(1, +1)); add(Target::StickUp,   max(1, -1));  // SDL y-down -> N64 up
    add(Target::B,     mb(SDL_BUTTON_LEFT));        // fire blasters
    add(Target::CLeft, mb(SDL_BUTTON_RIGHT));       // fire secondary
    add(Target::A,     mb(SDL_BUTTON_MIDDLE));      // menu confirm

    // --- Gamepad (menus read pad A/B as confirm/back through menu_fixed_mask) ---
    add(Target::B,     pb(SDL_CONTROLLER_BUTTON_A));
    add(Target::CLeft, pb(SDL_CONTROLLER_BUTTON_B));
    add(Target::CDown, pb(SDL_CONTROLLER_BUTTON_Y));
    add(Target::CRight,pb(SDL_CONTROLLER_BUTTON_X));
    add(Target::A,     pax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, +1));
    add(Target::Z,     pax(SDL_CONTROLLER_AXIS_TRIGGERLEFT, +1));
    add(Target::RTrig, pb(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    add(Target::CUp,   pb(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    add(Target::LTrig, pb(SDL_CONTROLLER_BUTTON_BACK));
    add(Target::Start, pb(SDL_CONTROLLER_BUTTON_START));
    add(Target::DpadUp,   pb(SDL_CONTROLLER_BUTTON_DPAD_UP));
    add(Target::DpadDown, pb(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    add(Target::DpadLeft, pb(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    add(Target::DpadRight,pb(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
    add(Target::StickRight, pax(SDL_CONTROLLER_AXIS_LEFTX, +1));
    add(Target::StickLeft,  pax(SDL_CONTROLLER_AXIS_LEFTX, -1));
    add(Target::StickUp,    pax(SDL_CONTROLLER_AXIS_LEFTY, -1));
    add(Target::StickDown,  pax(SDL_CONTROLLER_AXIS_LEFTY, +1));

    // --- VR controllers (Luke layout; menus read A/B/Menu through menu_fixed_mask) ---
    auto vb = [](int c) { return Source{ SourceKind::VrButton, c, 0 }; };
    auto vax = [](int c, int d) { return Source{ SourceKind::VrAxis, c, (int8_t)d }; };

    add(Target::B,      vb(VrA));
    add(Target::CLeft,  vb(VrB));
    add(Target::CRight, vb(VrX));
    add(Target::Start,  vb(VrMenu));
    add(Target::LTrig,  vb(VrStickClickL));
    add(Target::CDown,  vb(VrStickClickR));
    add(Target::Z,      vb(VrTriggerL));
    add(Target::A,      vb(VrTriggerR));
    add(Target::RTrig,  Source{ SourceKind::VrButton, VrGripR, 0, -1, Held{ SourceKind::VrButton, VrGripL, 0, -1 } });
    add(Target::StickRight, vax(0, +1));
    add(Target::StickLeft,  vax(0, -1));
    add(Target::StickUp,    vax(1, +1));
    add(Target::StickDown,  vax(1, -1));

    return b;
}

bool resolve(const Bindings& b, const RawState& s, uint16_t* buttons, float* x, float* y)
{

    bool any_joy = false;

    for (int i = 0; b.joystick_enabled && s.joys && i < s.joys_len && !any_joy; ++i) any_joy = s.joys[i] != nullptr;
    if (!b.keyboard_enabled && s.pad == nullptr && !any_joy && !s.vr) return false;

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

    // Combos down now: their own inputs drive no plain binding meanwhile (Shift+E as Roll doesn't also press what E alone does).
    static thread_local std::vector<Source> combos;

    combos.clear();

    for (int t = 0; t < (int)Target::Count; ++t) {
        for (const Source& src : b.targets[t]) {
            if (src.with.kind != SourceKind::None && input_down(b, st, held_source(src.with)) && input_down(b, st, src)) combos.push_back(src);
        }
    }

    const auto hidden = [&](const Source& src) {

        return std::any_of(combos.begin(), combos.end(), [&](const Source& c) { return same_input(c, src); });
    };

    for (int t = 0; t < (int)Target::Count; ++t) {
        float dig = 0.0f, ana = 0.0f;

        for (const Source& src : b.targets[t])
        {
            const bool combo = src.with.kind != SourceKind::None;

            if ((combo && !input_down(b, st, held_source(src.with))) || (!combo && hidden(src))) continue;

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

    // Look-around: hold C-Up and point the stick, overriding steering while a look input is held. In VR the head looks around, so C-Up is dropped.
    float lx = defl[(int)Target::LookRight] - defl[(int)Target::LookLeft];
    float ly = defl[(int)Target::LookUp]    - defl[(int)Target::LookDown];
    const bool looking = !s.vr && (std::fabs(lx) > 0.5f || std::fabs(ly) > 0.5f);

    if (s.vr) {
        btn &= ~N64_U_CBUTTONS;
    }

    if (looking) {
        btn |= N64_U_CBUTTONS;
        sx = lx;
        sy = ly;
    }

    // Roll: while the Roll button (R) is held the game turns stick X into pure roll; a deflected roll axis then takes over stick X.
    const float roll = defl[(int)Target::RollRight] - defl[(int)Target::RollLeft];

    if (!looking && (btn & N64_R_TRIG) && std::fabs(roll) > 0.1f) {
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

std::vector<std::pair<const char*, uint32_t*>> lightbar_colors(LightbarConfig& c)
{

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

    switch (t)
    {
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

    switch (c)
    {
        case SDL_BUTTON_LEFT: return "left";   case SDL_BUTTON_MIDDLE: return "middle";
        case SDL_BUTTON_RIGHT: return "right"; case SDL_BUTTON_X1: return "x1";
        case SDL_BUTTON_X2: return "x2";       default: return "";
    }
}
int mouse_button_code(const std::string& s)
{

    if (s == "left") return SDL_BUTTON_LEFT;   if (s == "middle") return SDL_BUTTON_MIDDLE;
    if (s == "right") return SDL_BUTTON_RIGHT; if (s == "x1") return SDL_BUTTON_X1;
    if (s == "x2") return SDL_BUTTON_X2;       return 0;
}

// VR button names in roguesq_input.json, by VrButton.
constexpr const char* kVrButtonNames[VrButtonCount] = { "a", "b", "x", "y", "menu", "stick-click-left", "stick-click-right", "trigger-left", "trigger-right", "grip-left", "grip-right" };

int vr_button_code(const std::string& s)
{

    for (int i = 0; i < VrButtonCount; ++i) {
        if (s == kVrButtonNames[i]) return i;
    }

    return -1;
}

bool input_to_json(const Source& s, json& o);
bool input_from_json(const json& o, Source& s);

// Serialize one source to {kind, name, dir?, with?}. Returns false if unrepresentable.
bool source_to_json(const Source& s, json& o)
{

    if (!input_to_json(s, o)) return false;

    json with;

    if (s.with.kind != SourceKind::None)
    {
        if (!input_to_json(held_source(s.with), with)) return false;

        o["with"] = with;
    }

    return true;
}

bool source_from_json(const json& o, Source& s)
{

    if (!input_from_json(o, s)) return false;

    if (!o.contains("with")) return true;

    Source held;

    if (!input_from_json(o["with"], held)) return false;

    s.with = as_held(held);

    return true;
}

bool input_to_json(const Source& s, json& o) {

    switch (s.kind)
    {
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
        case SourceKind::VrButton:
            if (s.code < 0 || s.code >= VrButtonCount) return false;

            o = { {"kind","vrbutton"}, {"name", kVrButtonNames[s.code]} };

            return true;
        case SourceKind::VrAxis:
            o = { {"kind","vraxis"}, {"name", s.code == 0 ? "x" : "y"}, {"dir", (int)s.dir} };

            return true;
        default:
            return false;
    }
}

bool input_from_json(const json& o, Source& s)
{

    if (!o.is_object() || !o.contains("kind") || !o.contains("name")) return false;
    if (!o["kind"].is_string() || !o["name"].is_string()) return false;

    std::string kind = o["kind"].get<std::string>();
    std::string name = o["name"].get<std::string>();

    if (kind == "joybutton" || kind == "joyaxis" || kind == "joyhat") {
        int dev = o.value("device", -1);

        if (dev < 0 || dev > 126) return false;

        if (kind == "joybutton")
        {
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

    if (kind == "key")
    {
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
    } else if (kind == "vrbutton") {
        const int c = vr_button_code(name);

        if (c < 0) return false;

        s = { SourceKind::VrButton, c, 0 };
    } else if (kind == "vraxis") {
        if (dir != 1 && dir != -1) return false;

        s = { SourceKind::VrAxis, (name == "y") ? 1 : 0, (int8_t)dir };
    } else {
        return false;
    }

    return true;
}
} // namespace

int target_count() { return (int)Target::Count; }

const char* target_label(Target t) { return target_name(t); }

std::string source_label(const Source& s) {

    if (s.with.kind != SourceKind::None)
    {
        Source main = s;

        main.with = Held{};

        return source_label(held_source(s.with)) + "+" + source_label(main);
    }

    switch (s.kind)
    {
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
        case SourceKind::VrButton:
            return std::string("vr:") + ((s.code >= 0 && s.code < VrButtonCount) ? kVrButtonNames[s.code] : "?");
        case SourceKind::VrAxis:
            return std::string("vrStick") + (s.code == 0 ? "X" : "Y") + (s.dir >= 0 ? "+" : "-");
        default:
            return "?";
    }
}

Device device_of(const Source& s)
{

    switch (s.kind)
    {
        case SourceKind::PadButton:
        case SourceKind::PadAxis:   return Device::Gamepad;
        case SourceKind::JoyButton:
        case SourceKind::JoyAxis:
        case SourceKind::JoyHat:    return Device::Joystick;
        case SourceKind::VrButton:
        case SourceKind::VrAxis:    return Device::Vr;
        default:                    return Device::Keyboard;
    }
}

std::vector<ControlRow> control_rows(Device d)
{

    std::vector<ControlRow> r = {
        { Target::A,          "THRUST" },
        { Target::B,          "FIRE BLASTERS" },
        { Target::Z,          "BRAKE" },
        { Target::RTrig,      "ROLL" },
        { Target::LTrig,      "SWITCH VIEW" },
        { Target::CUp,        "LOOK AROUND" },
        { Target::CDown,      "FIRE MODE" },
        { Target::CLeft,      "SECONDARY" },
        { Target::CRight,     "SPECIAL" },
        { Target::DpadUp,     "COCKPIT VIEW" },
        { Target::DpadRight,  "CLOSE VIEW" },
        { Target::DpadDown,   "STANDARD VIEW" },
        { Target::DpadLeft,   "DROP CAMERA" },
        { Target::StickUp,    "PITCH DOWN" },
        { Target::StickDown,  "PITCH UP" },
        { Target::StickLeft,  "TURN LEFT" },
        { Target::StickRight, "TURN RIGHT" },
        { Target::LookUp,     "LOOK UP" },
        { Target::LookDown,   "LOOK DOWN" },
        { Target::LookLeft,   "LOOK LEFT" },
        { Target::LookRight,  "LOOK RIGHT" },
    };

    if (d == Device::Joystick) {
        r.push_back({ Target::Throttle,  "THROTTLE" });
        r.push_back({ Target::RollLeft,  "ROLL LEFT" });
        r.push_back({ Target::RollRight, "ROLL RIGHT" });
    }

    return r;
}

int shown_source(const Bindings& b, Target t, Device d)
{
    return nth_source(b, t, d, 0);
}

int nth_source(const Bindings& b, Target t, Device d, int n)
{

    const std::vector<Source>& v = b.targets[(int)t];

    for (size_t k = 0; k < v.size(); ++k) {
        if (device_of(v[k]) == d && n-- == 0) return (int)k;
    }

    return -1;
}

Target rebind(Bindings& b, Target t, Device d, const Source& got, int column)
{

    constexpr int kShownColumns = 2;

    auto same = [](const Source& p, const Source& q) {
        return same_input(p, q) && same_input(held_source(p.with), held_source(q.with));
    };

    // Column of v[k] among its row's device-d sources.
    auto column_of = [d](const std::vector<Source>& v, size_t k) {
        return (int)std::count_if(v.begin(), v.begin() + k, [d](const Source& s) { return device_of(s) == d; });
    };

    std::vector<Source>& mine = b.targets[(int)t];
    const int at = nth_source(b, t, d, column);

    if (at >= 0 && same(mine[at], got)) return Target::Count;

    const bool had = at >= 0;
    const Source old = had ? mine[at] : Source{};
    Target giver = Target::Count;
    bool found = false;

    for (int u = 0; u < (int)Target::Count && !found; ++u)
    {
        std::vector<Source>& v = b.targets[u];

        for (size_t k = 0; k < v.size(); ++k)
        {
            if (!same(v[k], got)) continue;

            found = true;

            // A binding another row shows (or this row's other column) takes the replaced one; a hidden one (B's mouse button behind two keys) is just taken.
            if (had && column_of(v, k) < kShownColumns) {
                v[k] = old;
                giver = (u == (int)t) ? Target::Count : (Target)u;
            } else {
                v.erase(v.begin() + k);
            }

            break;
        }
    }

    // The column's own index again: an erase above may have moved it.
    const int now = nth_source(b, t, d, column);

    if (now >= 0 && had) mine[now] = got;
    else mine.push_back(got);

    return giver;
}

void clear_binding(Bindings& b, Target t, Device d, int column)
{

    const int at = nth_source(b, t, d, column);

    if (at >= 0) b.targets[(int)t].erase(b.targets[(int)t].begin() + at);
}

namespace {

// Upper case; drops anything the menu font can't draw (its ';' '_' '&' '"' '/' slots are redrawn as other glyphs by menu_config).
std::string font_text(const std::string& s)
{

    std::string out;

    for (char c : s)
    {
        const char u = (char)std::toupper((unsigned char)c);

        if (std::isupper((unsigned char)u) || std::isdigit((unsigned char)u) || std::strchr(" :',!?-<>{}.", u)) out += u;
    }

    return out;
}

const char* pad_button_menu_name(int code) {

    switch (code)
    {
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "LB";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "LS";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return "RS";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "UP";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "DOWN";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "LEFT";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "RIGHT";
        default:                                  return nullptr;
    }
}

const char* pad_axis_menu_name(int code) {

    switch (code)
    {
        case SDL_CONTROLLER_AXIS_LEFTX:        return "LS X";
        case SDL_CONTROLLER_AXIS_LEFTY:        return "LS Y";
        case SDL_CONTROLLER_AXIS_RIGHTX:       return "RS X";
        case SDL_CONTROLLER_AXIS_RIGHTY:       return "RS Y";
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return "LT";
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "RT";
        default:                               return "PAD AXIS";
    }
}

} // namespace

std::string menu_source_name(const Source& s)
{

    static constexpr const char* kVrMenuNames[VrButtonCount] = { "A", "B", "X", "Y", "MENU", "LS CLICK", "RS CLICK", "L TRIG", "R TRIG", "L GRIP", "R GRIP" };

    if (s.with.kind != SourceKind::None)
    {
        Source main = s;

        main.with = Held{};

        // The menu font has no '+'.
        return menu_source_name(held_source(s.with)) + " - " + menu_source_name(main);
    }

    const std::string arrow = s.dir >= 0 ? " >" : " <";
    const std::string joy = "J" + std::to_string(s.dev + 1) + " ";

    switch (s.kind)
    {
        case SourceKind::Key: {
            static const std::pair<SDL_Scancode, const char*> kPunct[] = {
                { SDL_SCANCODE_SEMICOLON, "SEMICOLON" }, { SDL_SCANCODE_SLASH, "SLASH" }, { SDL_SCANCODE_BACKSLASH, "BACKSLASH" },
                { SDL_SCANCODE_LEFTBRACKET, "L BRACKET" }, { SDL_SCANCODE_RIGHTBRACKET, "R BRACKET" },
                { SDL_SCANCODE_EQUALS, "EQUALS" }, { SDL_SCANCODE_GRAVE, "GRAVE" }, { SDL_SCANCODE_APOSTROPHE, "APOSTROPHE" },
            };

            for (const auto& [sc, name] : kPunct) {
                if (s.code == sc) return name;
            }

            std::string n = font_text(SDL_GetScancodeName((SDL_Scancode)s.code));

            // Short like the pad names so the binding column stays narrow: "LEFT SHIFT" -> "L SHIFT".
            for (const auto& [from, to] : { std::pair{ "LEFT ", "L " }, std::pair{ "RIGHT ", "R " }, std::pair{ "KEYPAD ", "NUM " } }) {
                if (n.rfind(from, 0) == 0) n = to + n.substr(std::strlen(from));
            }

            return n.empty() ? "KEY " + std::to_string(s.code) : n;
        }
        case SourceKind::PadButton: {
            const char* m = pad_button_menu_name(s.code);
            const char* n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)s.code);

            return "PAD " + (m ? std::string(m) : font_text(n ? n : "?"));
        }
        case SourceKind::PadAxis: {
            const bool trigger = s.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT || s.code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;

            return pad_axis_menu_name(s.code) + (trigger ? std::string() : arrow);
        }
        case SourceKind::MouseButton: return "MOUSE " + std::to_string(s.code);
        case SourceKind::MouseAxis:   return std::string("MOUSE ") + (s.code == 0 ? "X" : "Y") + arrow;
        case SourceKind::JoyButton:   return joy + "BTN " + std::to_string(s.code);
        case SourceKind::JoyAxis:     return joy + "AX " + std::to_string(s.code) + arrow;
        case SourceKind::JoyHat:      return joy + "HAT " + font_text(hat_dir_name(s.dir));
        case SourceKind::VrButton:    return (s.code >= 0 && s.code < VrButtonCount) ? kVrMenuNames[s.code] : "--";
        case SourceKind::VrAxis:      return std::string("STICK ") + (s.code == 0 ? "X" : "Y") + arrow;
        default:                      return "--";
    }
}

uint16_t menu_fixed_mask(const RawState& s, bool esc_is_start)
{

    static const std::pair<int, uint16_t> keys[] = {
        { SDL_SCANCODE_RETURN, N64_A_BUTTON }, { SDL_SCANCODE_KP_ENTER, N64_A_BUTTON }, { SDL_SCANCODE_SPACE, N64_A_BUTTON }, { SDL_SCANCODE_BACKSPACE, N64_B_BUTTON },
        { SDL_SCANCODE_UP, N64_U_JPAD }, { SDL_SCANCODE_DOWN, N64_D_JPAD }, { SDL_SCANCODE_LEFT, N64_L_JPAD }, { SDL_SCANCODE_RIGHT, N64_R_JPAD },
    };
    static const std::pair<SDL_GameControllerButton, uint16_t> pad[] = {
        { SDL_CONTROLLER_BUTTON_A, N64_A_BUTTON }, { SDL_CONTROLLER_BUTTON_B, N64_B_BUTTON }, { SDL_CONTROLLER_BUTTON_START, N64_START_BUTTON },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, N64_U_JPAD }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, N64_D_JPAD },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, N64_L_JPAD }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, N64_R_JPAD },
    };

    uint16_t m = 0;

    for (const auto& [sc, bit] : keys) {
        if (s.keys && sc < s.keys_len && s.keys[sc]) m |= bit;
    }

    if (s.keys && SDL_SCANCODE_ESCAPE < s.keys_len && s.keys[SDL_SCANCODE_ESCAPE]) m |= esc_is_start ? N64_START_BUTTON : N64_B_BUTTON;

    for (const auto& [b, bit] : pad) {
        if (s.pad && SDL_GameControllerGetButton(s.pad, b)) m |= bit;
    }

    static const std::pair<int, uint16_t> vr[] = { { VrA, N64_A_BUTTON }, { VrB, N64_B_BUTTON }, { VrMenu, N64_START_BUTTON } };

    for (const auto& [b, bit] : vr) {
        if (s.vr && (s.vr_buttons >> b & 1u)) m |= bit;
    }

    return m;
}

uint16_t menu_buttons(uint16_t bound, uint16_t fixed)
{

    constexpr uint16_t kMenuBits = N64_A_BUTTON | N64_B_BUTTON | N64_START_BUTTON | N64_U_JPAD | N64_D_JPAD | N64_L_JPAD | N64_R_JPAD;

    return fixed ? (uint16_t)((bound & ~kMenuBits) | fixed) : bound;
}

bool capture_accepts(const CaptureFilter& f, const Source& s)
{

    if (s.kind == SourceKind::None || s.kind == SourceKind::MouseAxis) return false;

    // A combo's held input comes from the same device.
    if ((s.with.kind != SourceKind::None) && (s.with.kind == SourceKind::MouseAxis || device_of(held_source(s.with)) != device_of(s))) return false;

    return f.any || device_of(s) == f.device;
}

Bindings default_device_bindings(const Bindings& cur, Device d)
{

    const Bindings def = default_bindings();
    Bindings out = cur;

    for (int t = 0; t < (int)Target::Count; ++t)
    {
        std::vector<Source>& v = out.targets[t];

        v.erase(std::remove_if(v.begin(), v.end(), [d](const Source& s) { return device_of(s) == d; }), v.end());

        for (const Source& s : def.targets[t]) {
            if (device_of(s) == d) v.push_back(s);
        }
    }

    return out;
}

// In the working directory with saves and mods: the exe's folder can be read-only (AppImage, Flatpak).
std::string default_config_path() {
    return "roguesq_input.json";
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
    b.pad_deadzone     = j.value("pad_deadzone", b.pad_deadzone);
    b.throttle_cruise  = j.value("throttle_cruise", b.throttle_cruise);
    b.trigger_resistance = std::clamp(j.value("trigger_resistance", b.trigger_resistance), 0.0f, 1.0f);

    if (j.contains("rumble") && j["rumble"].is_object())
    {
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

            for (auto& [name, field] : lightbar_colors(c))
            {
                if (k.contains(name) && k[name].is_string()) *field = parse_hex_color(k[name].get<std::string>(), *field);
            }
        }
    }

    if (j.contains("joysticks") && j["joysticks"].is_array()) {
        b.joy_devices.clear();

        for (const json& d : j["joysticks"])
        {
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

bool save_bindings(const Bindings& b, const std::string& path)
{

    json j;
    j["mouse"] = { {"sensitivity", b.mouse_sensitivity},
                   {"smoothing", b.mouse_smoothing}, {"curve", b.mouse_curve},
                   {"invert_x", b.mouse_invert_x}, {"invert_y", b.mouse_invert_y} };
    j["keyboard_enabled"] = b.keyboard_enabled;
    j["joystick_enabled"] = b.joystick_enabled;
    j["stick_range"] = b.stick_range;
    j["pad_deadzone"] = b.pad_deadzone;
    j["throttle_cruise"] = b.throttle_cruise;
    j["trigger_resistance"] = b.trigger_resistance;
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

    for (const JoyDevice& d : b.joy_devices)
    {
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

    return rs64::settings::write_file(path, j.dump(2) + "\n");
}
} // namespace rs64::input
