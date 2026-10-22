#include "main/ui_panels.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "os_compat.h"
#include "imgui/imgui.h"
#include "rhi/rt64_render_hooks.h"
#include "gui/rt64_file_dialog.h"
#include "librecomp/mods.hpp"
#include "input/input_host.h"
#include "input/rumble.h"
#include "input/touch_config.h"
#include "game/game_state.h"
#include "util/debug_logs.h"
#include "host_api/host_api.h"
#include "main/main.h"
#include "video/rt64_render_context.h"
#include "video/video_config.h"

using recomp::dbg::env_on;
using recomp::dbg::env_int;

namespace rs64::app {
static void draw_controls_ui()
{

    if (!g_show_controls.load()) return;

    namespace ri = rs64::input;

    static int capture_target = -1;                 // Target awaiting a new bind
    static bool capture_append = false;             // Add (keep existing binds) vs Rebind (replace)
    static bool capture_snap = false;               // joystick baseline not taken yet
    static uint8_t prev_keys[SDL_NUM_SCANCODES] = {0};
    static uint32_t prev_pad = 0, prev_mouse = 0;

    // Joystick state when the rebind started; a button/hat press or an axis moved >50% from here is captured.
    struct JoySnap { SDL_JoystickID id; std::vector<float> axes; std::vector<uint8_t> buttons, hats; };
    static std::vector<JoySnap> joy_snap;

    ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);
    bool open = true;

    if (ImGui::Begin("Controls", &open))
    {
        // Action row at the top so it stays visible even if the window is taller
        // than the game viewport.
        if (ImGui::Button("Save"))
        {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            rs64::input::save_bindings(g_bindings, rs64::input::default_config_path());
        }

        ImGui::SameLine();

        if (ImGui::Button("Restore defaults"))
        {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            g_bindings = rs64::input::default_bindings();
            rebind_open_joysticks_locked();
            rs64::input::save_bindings(g_bindings, rs64::input::default_config_path());
        }

        ImGui::SameLine();

        if (ImGui::Button("Close")) g_show_controls.store(false);

        ImGui::Separator();
        ImGui::TextWrapped("Click Rebind (replace) or Add, then press a key, gamepad button, mouse button, joystick button or hat, or move a joystick axis. Esc cancels.");

        std::unique_lock<std::mutex> mouse_lock(g_bindings_mtx);
        ImGui::SliderFloat("Mouse sensitivity", &g_bindings.mouse_sensitivity, 0.005f, 0.30f, "%.3f");
        ImGui::SliderFloat("Mouse smoothing", &g_bindings.mouse_smoothing, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Mouse response curve", &g_bindings.mouse_curve, 1.0f, 3.0f, "%.2f");
        ImGui::Checkbox("Invert X", &g_bindings.mouse_invert_x);
        ImGui::SameLine();
        ImGui::Checkbox("Invert Y", &g_bindings.mouse_invert_y);
        mouse_lock.unlock();

        ImGui::TextDisabled("Mouse-steering is automatic while the window is focused; F6 or Esc closes this.");
        bool fs = rs64_get_fullscreen() != 0;

        if (ImGui::Checkbox("Fullscreen", &fs)) rs64_set_fullscreen(fs);

        ImGui::SameLine();
        ImGui::TextDisabled("(Alt+Enter)");

        if (ImGui::CollapsingHeader("Joysticks / HOTAS"))
        {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            ImGui::Checkbox("Joystick input", &g_bindings.joystick_enabled);
            ImGui::SameLine();
            ImGui::TextDisabled("(untick to ignore connected joysticks)");
            ImGui::SliderFloat("Analog stick range", &g_bindings.stick_range, 60.0f, 127.0f, "%.0f");
            ImGui::TextDisabled("Full gamepad/joystick deflection. The N64 stick reaches ~80 and the game saturates there.");

            if (g_bindings.joy_devices.empty()) ImGui::TextDisabled("No joysticks seen yet.");

            for (size_t d = 0; d < g_bindings.joy_devices.size(); ++d)
            {
                ri::JoyDevice& jd = g_bindings.joy_devices[d];
                SDL_Joystick* j = d < g_joy_by_dev.size() ? g_joy_by_dev[d] : nullptr;
                ImGui::PushID((int)d);
                ImGui::Text("J%d  %s%s", (int)d + 1, jd.name.c_str(), j ? "" : "  (disconnected)");
                ImGui::SliderFloat("Deadzone", &jd.deadzone, 0.0f, 0.5f, "%.2f");
                const int axes = j ? SDL_JoystickNumAxes(j) : 0;

                for (int a = 0; a < axes && a < 32; ++a)
                {
                    ImGui::PushID(a);
                    ri::RawState rs;
                    rs.joys = g_joy_by_dev.data();
                    rs.joys_len = (int)g_joy_by_dev.size();
                    const float v = ri::joy_axis(g_bindings, rs, (int)d, a);
                    char label[32];
                    snprintf(label, sizeof(label), "axis%d  %+.2f", a, v);
                    ImGui::ProgressBar((v + 1.0f) * 0.5f, ImVec2(220, 0), label);
                    ImGui::SameLine();
                    bool inv = (jd.invert_axes >> a) & 1u;

                    if (ImGui::Checkbox("Invert", &inv)) jd.invert_axes = inv ? (jd.invert_axes | (1u << a)) : (jd.invert_axes & ~(1u << a));

                    ImGui::PopID();
                }

                ImGui::PopID();
            }

            const float tp = g_throttle.load();

            if (tp >= 0.0f) ImGui::ProgressBar(tp, ImVec2(220, 0), "Throttle");
            else ImGui::TextDisabled("Throttle: not bound (pull the lever back, click Rebind on Throttle, then push it forward).");

            ImGui::SliderFloat("Cruise at", &g_bindings.throttle_cruise, 0.05f, 0.95f, "%.2f");
            ImGui::TextDisabled("Lever back = slowest, forward = fastest; the \"Cruise at\" position gives the craft's normal speed.");
        }

        if (ImGui::CollapsingHeader("Rumble"))
        {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            ri::RumbleConfig& r = g_bindings.rumble;
            static bool s_late_pak = false;

            if (ImGui::Checkbox("Rumble", &r.enabled) && r.enabled && !g_report_rumble_pak.exchange(true))
            {
                s_late_pak = true;

                fprintf(stderr, "[rumble] Rumble Pak now reported; effects start with the next mission\n");
            }

            if (r.enabled && s_late_pak) {
                ImGui::SameLine();
                ImGui::TextDisabled("(starts with the next mission)");
            }

            ImGui::SliderFloat("Strength", &r.strength, 0.0f, 2.0f, "%.2f");
            ImGui::Checkbox("Scale hits by damage", &r.scale_hits_by_damage);
            ImGui::Checkbox("Sustain through death spiral", &r.sustain_death_spiral);
            ImGui::Checkbox("Hits", &r.hit);
            ImGui::SameLine();
            ImGui::Checkbox("Collisions", &r.collision);
            ImGui::SameLine();
            ImGui::Checkbox("Object collisions", &r.object_collision);
            ImGui::Checkbox("Terrain scrape", &r.terrain_scrape);
            ImGui::SameLine();
            ImGui::Checkbox("Weapons", &r.weapons);
            ImGui::SameLine();
            ImGui::Checkbox("Death spiral", &r.death_spiral);
            ImGui::SameLine();
            ImGui::Checkbox("Crash", &r.crash);
        }

        if (ImGui::CollapsingHeader("Lightbar (DualShock 4 / DualSense)"))
        {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            ri::LightbarConfig& l = g_bindings.lightbar;
            ImGui::Checkbox("Lightbar", &l.enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Follow health", &l.health);
            ImGui::SameLine();
            ImGui::Checkbox("Flash on hits", &l.hit_flash);

            auto color = [](const char* label, uint32_t& c) {

                float f[3] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f };

                if (ImGui::ColorEdit3(label, f, ImGuiColorEditFlags_NoInputs))
                {
                    c = ((uint32_t)std::lround(f[0] * 255.0f) << 16) | ((uint32_t)std::lround(f[1] * 255.0f) << 8) | (uint32_t)std::lround(f[2] * 255.0f);
                }
            };

            color("Menus", l.menu);
            ImGui::SameLine();
            color("Cutscenes / demos", l.cinematic);
            color("Full health", l.mission);
            ImGui::SameLine();
            color("Damaged", l.damaged);
            ImGui::SameLine();
            color("Critical", l.critical);
            color("Hit flash", l.hit);
            ImGui::SameLine();
            color("Death spiral", l.death);
            ImGui::SameLine();
            color("Destroyed", l.dead);

            if (ImGui::Button("Default colors")) {
                const ri::LightbarConfig d;
                const bool en = l.enabled, hp = l.health, hf = l.hit_flash;
                l = d;
                l.enabled = en;
                l.health = hp;
                l.hit_flash = hf;
            }

            bool any_led = false;

            for (SDL_GameController* p : g_pads) any_led = any_led || SDL_GameControllerHasLED(p);
            if (!any_led) ImGui::TextDisabled("No connected pad has a controllable lightbar (Steam Input or DS4Windows hide it behind a virtual pad).");
        }

        ImGui::Separator();

        // Edge-detect a captured input while a rebind is pending.
        if (capture_target >= 0 && capture_snap) {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            joy_snap.clear();

            for (const OpenJoy& oj : g_open_joys)
            {
                JoySnap s{ oj.id, {}, {}, {} };

                for (int a = 0; a < SDL_JoystickNumAxes(oj.joy); ++a) s.axes.push_back(SDL_JoystickGetAxis(oj.joy, a) / 32767.0f);
                for (int b = 0; b < SDL_JoystickNumButtons(oj.joy); ++b) s.buttons.push_back(SDL_JoystickGetButton(oj.joy, b));
                for (int h = 0; h < SDL_JoystickNumHats(oj.joy); ++h) s.hats.push_back(SDL_JoystickGetHat(oj.joy, h));

                joy_snap.push_back(std::move(s));
            }

            capture_snap = false;
        } else if (capture_target >= 0) {
            int nk = 0;
            const uint8_t* ks = SDL_GetKeyboardState(&nk);

            if (ks[SDL_SCANCODE_ESCAPE])
            {
                capture_target = -1;
            } else {
                ri::Source got;
                bool have = false;

                for (int sc = 0; sc < nk && !have; ++sc)
                    if (sc != SDL_SCANCODE_ESCAPE && ks[sc] && !prev_keys[sc]) { got = { ri::SourceKind::Key, sc, 0 }; have = true; }

                if (!have && controller)
                    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && !have; ++b)
                    {
                        bool now = SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b);

                        if (now && !((prev_pad >> b) & 1)) { got = { ri::SourceKind::PadButton, b, 0 }; have = true; }
                    }

                if (!have)
                {
                    uint32_t mb = SDL_GetMouseState(nullptr, nullptr);

                    for (int b = 1; b <= 5 && !have; ++b)
                        if ((mb & SDL_BUTTON(b)) && !(prev_mouse & SDL_BUTTON(b))) { got = { ri::SourceKind::MouseButton, b, 0 }; have = true; }
                }

                std::lock_guard<std::mutex> lk(g_bindings_mtx);

                for (const OpenJoy& oj : g_open_joys) {
                    if (have) break;

                    const int dev = joy_dev_locked(oj);
                    const JoySnap* s = nullptr;

                    for (const JoySnap& js : joy_snap) {
                        if (js.id == oj.id) s = &js;
                    }

                    if (dev < 0 || !s) continue;

                    for (int b = 0; b < (int)s->buttons.size() && !have; ++b)
                    {
                        if (SDL_JoystickGetButton(oj.joy, b) && !s->buttons[b]) { got = { ri::SourceKind::JoyButton, b, 0, (int8_t)dev }; have = true; }
                    }

                    for (int h = 0; h < (int)s->hats.size() && !have; ++h)
                    {
                        const uint8_t now_bits = SDL_JoystickGetHat(oj.joy, h) & (uint8_t)~s->hats[h];

                        for (int bit = 0; bit < 4 && !have; ++bit)
                        {
                            if (now_bits & (1u << bit)) { got = { ri::SourceKind::JoyHat, h, (int8_t)(1 << bit), (int8_t)dev }; have = true; }
                        }
                    }

                    for (int a = 0; a < (int)s->axes.size() && !have; ++a)
                    {
                        const float delta = SDL_JoystickGetAxis(oj.joy, a) / 32767.0f - s->axes[a];

                        if (std::fabs(delta) < 0.5f) continue;

                        int dir = delta > 0.0f ? 1 : -1;

                        if (a < 32 && ((g_bindings.joy_devices[dev].invert_axes >> a) & 1u)) dir = -dir;

                        got = { ri::SourceKind::JoyAxis, a, (int8_t)dir, (int8_t)dev };
                        have = true;
                    }
                }

                if (have) {
                    if (!capture_append) g_bindings.targets[capture_target].clear();

                    g_bindings.targets[capture_target].push_back(got);
                    capture_target = -1;
                }
            }
        }

        int nk = 0;
        const uint8_t* ks = SDL_GetKeyboardState(&nk);
        int n = nk < (int)SDL_NUM_SCANCODES ? nk : (int)SDL_NUM_SCANCODES;
        memcpy(prev_keys, ks, n);
        prev_pad = 0;

        if (controller) for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b)) prev_pad |= (1u << b);

        prev_mouse = SDL_GetMouseState(nullptr, nullptr);

        ImGui::BeginChild("binds", ImVec2(0, 0), true);
        std::unique_lock<std::mutex> binds_lock(g_bindings_mtx);

        for (int t = 0; t < ri::target_count(); ++t) {
            ImGui::PushID(t);
            ImGui::Text("%s", ri::target_label((ri::Target)t));
            ImGui::SameLine(120);

            if (capture_target == t)
            {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "[press an input...]");
            } else {
                std::string b;

                for (const auto& s : g_bindings.targets[t]) { if (!b.empty()) b += ", "; b += ri::source_label(s); }

                ImGui::TextUnformatted(b.empty() ? "(unbound)" : b.c_str());
            }

            ImGui::SameLine(330);

            if (ImGui::SmallButton("Rebind")) {
                capture_target = t;
                capture_append = false;
                capture_snap = true;
            }

            ImGui::SameLine();

            if (ImGui::SmallButton("Add")) {
                capture_target = t;
                capture_append = true;
                capture_snap = true;
            }

            ImGui::SameLine();

            if (ImGui::SmallButton("Clear")) g_bindings.targets[t].clear();

            ImGui::PopID();
        }

        binds_lock.unlock();
        ImGui::EndChild();
    }

    ImGui::End();

    if (!open) g_show_controls.store(false);
}

extern "C" void rs64_menu_config_mark_dirty(void);   // menu_config.cpp
extern "C" void rs64_menu_config_init(void);         // menu_config.cpp

// Render one mod config-schema option as an ImGui widget and persist edits.
static void draw_mod_config_option(const std::string& mod_id, const recomp::mods::ConfigOption& opt)
{

    using namespace recomp::mods;
    ConfigValueVariant v = get_mod_config_value(mod_id, opt.id);
    bool changed = false;

    switch (opt.type) {
        case ConfigOptionType::Enum: {
            const auto& e = std::get<ConfigOptionEnum>(opt.variant);
            int cur = std::holds_alternative<uint32_t>(v) ? (int)std::get<uint32_t>(v) : (int)e.default_value;
            std::vector<const char*> items;
            items.reserve(e.options.size());

            for (const auto& s : e.options) items.push_back(s.c_str());

            if (ImGui::Combo(opt.name.c_str(), &cur, items.data(), (int)items.size())) {
                set_mod_config_value(mod_id, opt.id, (uint32_t)cur);
                changed = true;
            }
            break;
        }
        case ConfigOptionType::Number: {
            const auto& n = std::get<ConfigOptionNumber>(opt.variant);
            double dv = std::holds_alternative<double>(v) ? std::get<double>(v) : n.default_value;
            float f = (float)dv;

            if (ImGui::SliderFloat(opt.name.c_str(), &f, (float)n.min, (float)n.max)) {
                set_mod_config_value(mod_id, opt.id, (double)f);
                changed = true;
            }
            break;
        }
        case ConfigOptionType::String: {
            const auto& s = std::get<ConfigOptionString>(opt.variant);
            std::string cur = std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : s.default_value;
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s", cur.c_str());

            if (ImGui::InputText(opt.name.c_str(), buf, sizeof buf)) {
                set_mod_config_value(mod_id, opt.id, std::string(buf));
                changed = true;
            }
            break;
        }
        default: break;
    }

    if (!opt.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opt.description.c_str());
    if (changed) rs64_menu_config_mark_dirty();
}

// A "Mods" panel: lists loaded mods, an enable toggle, and each mod's
// config-schema options (rendered from librecomp's mod system).
static void draw_mods_ui() {

    if (!g_show_controls.load()) return;

    ImGui::SetNextWindowSize(ImVec2(460, 380), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Mods"))
    {
        auto mods = recomp::mods::get_all_mod_details("rs64");

        if (mods.empty()) ImGui::TextDisabled("No mods loaded. Put mods in the 'mods' folder.");

        // Code mods load once at game start; toggling one is saved but only applies on the next launch.
        static std::unordered_map<std::string, bool> s_loaded_state;

        for (const auto& d : mods)
        {
            ImGui::PushID(d.mod_id.c_str());
            bool enabled = recomp::mods::is_mod_enabled(d.mod_id);

            if (!d.runtime_toggleable) {
                s_loaded_state.try_emplace(d.mod_id, enabled);
            }

            if (ImGui::Checkbox("##enabled", &enabled)) {
                recomp::mods::enable_mod(d.mod_id, enabled);
                rs64_menu_config_mark_dirty();
            }

            ImGui::SameLine();
            const char* title = d.display_name.empty() ? d.mod_id.c_str() : d.display_name.c_str();
            auto loaded = s_loaded_state.find(d.mod_id);

            if (loaded != s_loaded_state.end() && loaded->second != enabled)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(restart to apply)");
                ImGui::SameLine();
            }

            if (ImGui::CollapsingHeader(title))
            {
                if (!d.description.empty()) ImGui::TextWrapped("%s", d.description.c_str());

                const auto& schema = recomp::mods::get_mod_config_schema(d.mod_id);

                if (schema.options.empty()) ImGui::TextDisabled("(no options)");

                for (const auto& opt : schema.options) {
                    ImGui::PushID(opt.id.c_str());
                    draw_mod_config_option(d.mod_id, opt);
                    ImGui::PopID();
                }
            }

            ImGui::PopID();
        }
    }

    ImGui::End();
}

static void draw_touch_text(ImDrawList* dl, const char* s, ImVec2 c, float size, ImU32 col) {

    ImFont* f = ImGui::GetFont();
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
    dl->AddText(f, size, { c.x - ts.x / 2, c.y - ts.y / 2 }, col, s);
}

// N64 controller glyphs (not action icons: controller presets remap what each button does).
static void draw_touch_glyph(ImDrawList* dl, const rs64::touch::ButtonDef& b, float W, float H, float op, bool pressed)
{

    const std::string_view id = b.id ? b.id : "";
    const ImVec2 c{ b.cx * W, b.cy * H };
    const float r = b.r * H;
    const int a = (int)(op * (pressed ? 255 : 150));
    const ImU32 ink = IM_COL32(255, 255, 255, (int)(op * 255));
    const ImU32 ring = IM_COL32(255, 255, 255, (int)(op * (pressed ? 255 : 170)));

    auto color = [&](int rr, int gg, int bb) {

        const float k = pressed ? 1.35f : 1.0f;

        return IM_COL32(std::min(255, (int)(rr * k)), std::min(255, (int)(gg * k)), std::min(255, (int)(bb * k)), a);
    };

    if (id == "R") {
        const ImVec2 p0{ c.x - r * 1.3f, c.y - r * 0.7f };
        const ImVec2 p1{ c.x + r * 1.3f, c.y + r * 0.7f };
        dl->AddRectFilled(p0, p1, color(120, 120, 128), r * 0.7f);
        dl->AddRect(p0, p1, ring, r * 0.7f, 0, 2.0f);
        draw_touch_text(dl, "R", c, r * 1.1f, ink);

        return;
    }

    if (id == "CU" || id == "CD" || id == "CL" || id == "CR") {
        dl->AddCircleFilled(c, r, color(235, 190, 30), 32);
        dl->AddCircle(c, r, ring, 32, 2.0f);
        const float t = r * 0.55f;
        const ImU32 arrow = IM_COL32(60, 40, 0, (int)(op * 255));

        if (id == "CU")
        {
            dl->AddTriangleFilled({ c.x, c.y - t }, { c.x - t, c.y + t * 0.6f }, { c.x + t, c.y + t * 0.6f }, arrow);
        } else if (id == "CD") {
            dl->AddTriangleFilled({ c.x, c.y + t }, { c.x + t, c.y - t * 0.6f }, { c.x - t, c.y - t * 0.6f }, arrow);
        } else if (id == "CL") {
            dl->AddTriangleFilled({ c.x - t, c.y }, { c.x + t * 0.6f, c.y + t }, { c.x + t * 0.6f, c.y - t }, arrow);
        } else {
            dl->AddTriangleFilled({ c.x + t, c.y }, { c.x - t * 0.6f, c.y - t }, { c.x - t * 0.6f, c.y + t }, arrow);
        }

        return;
    }

    if (id == "GEAR") {
        dl->AddCircleFilled(c, r, IM_COL32(70, 70, 80, a), 32);

        for (int i = 0; i < 8; ++i)
        {
            const float ang = i * 3.14159265f / 4.0f;
            const ImVec2 d{ std::cos(ang), std::sin(ang) };
            dl->AddLine({ c.x + d.x * r * 0.35f, c.y + d.y * r * 0.35f }, { c.x + d.x * r * 0.75f, c.y + d.y * r * 0.75f }, ink, r * 0.22f);
        }

        dl->AddCircleFilled(c, r * 0.5f, ink, 24);
        dl->AddCircleFilled(c, r * 0.22f, IM_COL32(70, 70, 80, 255), 16);
        dl->AddCircle(c, r, ring, 32, 2.0f);

        return;
    }

    ImU32 fill = color(130, 130, 138);

    if (id == "A") {
        fill = color(50, 90, 220);
    } else if (id == "B" || id == "MENU_B") {
        fill = color(40, 160, 70);
    } else if (id == "START" || id == "MENU_START") {
        fill = color(200, 45, 45);
    }

    dl->AddCircleFilled(c, r, fill, 32);
    dl->AddCircle(c, r, ring, 32, 2.0f);
    const bool word = std::string_view(b.label).size() > 1;
    draw_touch_text(dl, b.label, c, word ? r * 0.55f : r * 1.1f, ink);
}

// Touch controls over the game (or the layout editor); with debug_hitboxes, the menu entries' tap boxes.
static void draw_touch_overlay()
{

    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x;
    const float H = io.DisplaySize.y;
    const rs64::touch::Context ctx = touch_context();
    rs64::touch::OverlayState ds;
    rs64::touch::Config cfg;
    std::unique_lock<std::mutex> touch_lock(g_touch_mtx);
    ds = g_touch.draw_state(ctx);
    cfg = g_touch.config();
    touch_lock.unlock();

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Pause-menu calibration: log the HUD's line elements (at most once a second, only when they change).
    if (cfg.debug_hitboxes && ctx == rs64::touch::Context::PauseMenu) {
        static std::string s_last_pause;
        static uint32_t s_last_ms = 0;
        uint8_t* r = touch_rdram();
        const uint32_t now = SDL_GetTicks();

        if (r && now - s_last_ms > 1000) {
            std::string d = rs64::touch::describe_pause(r);

            if (d != s_last_pause) {
                s_last_pause = d;
                s_last_ms = now;

                fprintf(stderr, "[touch] pause %s win=%.0fx%.0f\n", d.c_str(), W, H);
            }
        }

        std::vector<rs64::touch::PauseLine> lines;

        if (r && rs64::touch::read_pause_lines(r, W, H, &lines))
        {
            for (const rs64::touch::PauseRow& row : rs64::touch::pause_rows(lines))
            {
                dl->AddRect({ row.x0 * W, row.y0 * H }, { row.x1 * W, row.y1 * H }, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
            }
        }
    }

    if (cfg.debug_hitboxes && touch_menu_hits_allowed()) {
        std::vector<rs64::touch::Box> boxes;
        rs64::touch::MenuSnapshot snap{};
        uint8_t* r = touch_rdram();

        if (r) {
            static std::string s_last;
            std::string d = rs64::touch::describe_menu(r);

            if (d != s_last) {
                s_last = d;

                fprintf(stderr, "[touch] menu %s win=%.0fx%.0f\n", d.c_str(), W, H);
            }
        }

        if (r && rs64::touch::read_menu(r, &snap, &boxes, W, H)) {
            for (const rs64::touch::Box& b : boxes)
            {
                dl->AddRect({ b.x0 * W, b.y0 * H }, { b.x1 * W, b.y1 * H }, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
            }
        }
    }

    if (!ds.visible)
    {
        return;
    }

    const float op = ds.editing ? 1.0f : cfg.opacity;
    const ImU32 edge = IM_COL32(255, 255, 255, (int)(op * 255));

    if (ds.editing)
    {
        dl->AddRectFilled({ 0, 0 }, { W, H }, IM_COL32(0, 0, 0, 150));
        const float sx = ds.stick_split * W;

        for (float y = 0; y < H; y += 24.0f)
        {
            dl->AddLine({ sx, y }, { sx, y + 12.0f }, IM_COL32(255, 255, 255, 200), 3.0f);
        }

        draw_touch_text(dl, "STICK", { sx * 0.5f, H * 0.5f }, H * 0.05f, IM_COL32(255, 255, 255, 160));
        draw_touch_text(dl, "TOUCH LAYOUT: drag to move, pinch or +/- to resize", { W * 0.5f, H * 0.17f }, H * 0.035f, edge);
    }

    for (const rs64::touch::ButtonDef& b : ds.buttons) {
        draw_touch_glyph(dl, b, W, H, op, (ds.pressed & b.mask) != 0);

        if (ds.editing && b.mask == ds.selected)
        {
            dl->AddCircle({ b.cx * W, b.cy * H }, b.r * H + 6.0f, IM_COL32(255, 220, 40, 255), 48, 4.0f);
        }
    }

    for (const rs64::touch::ButtonDef& t : ds.tools)
    {
        const ImVec2 c{ t.cx * W, t.cy * H };
        const float r = t.r * H;
        const float hw = std::string_view(t.label).size() > 1 ? r * 1.9f : r;
        dl->AddRectFilled({ c.x - hw, c.y - r }, { c.x + hw, c.y + r }, IM_COL32(40, 60, 110, 230), r);
        dl->AddRect({ c.x - hw, c.y - r }, { c.x + hw, c.y + r }, edge, r, 0, 2.0f);
        draw_touch_text(dl, t.label, c, r * 0.9f, edge);
    }

    if (ds.stick_down)
    {
        dl->AddCircleFilled({ ds.base_x * W, ds.base_y * H }, ds.stick_r * H, IM_COL32(40, 40, 48, (int)(op * 110)), 48);
        dl->AddCircle({ ds.base_x * W, ds.base_y * H }, ds.stick_r * H, edge, 48, 2.0f);
        dl->AddCircleFilled({ ds.knob_x * W, ds.knob_y * H }, ds.stick_r * H * 0.4f, IM_COL32(90, 90, 100, (int)(op * 230)), 32);
        dl->AddCircle({ ds.knob_x * W, ds.knob_y * H }, ds.stick_r * H * 0.4f, edge, 32, 2.0f);
    }
}

// Bottom-right "Compiling shaders" panel while draws wait for their shader (D3D11/D3D10 have no ubershader to draw them meanwhile).
static void draw_shader_compile_indicator()
{

    uint32_t pending = 0;
    uint32_t compiled = 0;
    bool draws_wait = false;
    RT64::GetShaderCompileStatus(pending, compiled, draws_wait);
    static bool s_active = false;
    static uint32_t s_batch_start = 0;

    if (!draws_wait || (pending == 0)) {
        s_active = false;

        return;
    }

    if (!s_active) {
        s_active = true;
        s_batch_start = compiled;
    }

    const uint32_t done = compiled - s_batch_start;
    const uint32_t total = done + pending;
    const ImGuiIO& io = ImGui::GetIO();
    const float margin = 12.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##shader_compile", nullptr, flags))
    {
        ImGui::Text("Compiling shaders  %u / %u", done, total);
        ImGui::ProgressBar(float(done) / float(total), ImVec2(220.0f, 0.0f), "");
    }

    ImGui::End();
}

// The single ImGui render hook draws the touch overlay, both dev panels and the shader compile indicator.
void draw_dev_ui()
{

    draw_touch_overlay();
    draw_controls_ui();
    draw_mods_ui();
    draw_shader_compile_indicator();

    // Text input is off by default (see create_gfx); an ImGui text box needs it while focused.
    static bool s_imgui_text = false;
    const bool want_text = ImGui::GetIO().WantTextInput;

    if (want_text != s_imgui_text) {
        s_imgui_text = want_text;

        if (want_text) {
            SDL_StartTextInput();
        } else {
            SDL_StopTextInput();
        }
    }
}
}
