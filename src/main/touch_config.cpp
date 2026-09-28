#include "touch_config.h"

#include <cstdio>
#include <fstream>
#include "json/json.hpp"
#ifndef RS64_TOUCH_NO_SDL
#include "SDL.h"
#endif

using nlohmann::json;

namespace rs64::touch {
    std::string config_path() {
        std::string dir;
#ifndef RS64_TOUCH_NO_SDL
        if (char* base = SDL_GetBasePath()) {
            dir = base;
            SDL_free(base);
        }
#endif
        return dir + "roguesq_touch.json";
    }

    Config load_config(const std::string& path) {
        Config c;
#ifdef __ANDROID__
        c.enabled = true;
#endif
        std::ifstream f(path);
        if (!f.is_open()) {
            save_config(c, path);
            return c;
        }
        try {
            json j = json::parse(f);
            c.enabled = j.value("enabled", c.enabled);
            c.opacity = j.value("opacity", c.opacity);
            c.button_scale = j.value("button_scale", c.button_scale);
            c.stick_radius = j.value("stick_radius", c.stick_radius);
            c.stick_curve = j.value("stick_curve", c.stick_curve);
            c.gyro = j.value("gyro", c.gyro);
            c.debug_hitboxes = j.value("debug_hitboxes", c.debug_hitboxes);
            c.stick_split = j.value("stick_split", c.stick_split);
            c.steer_dead_deg = j.value("steer_dead_deg", c.steer_dead_deg);
            c.steer_full_deg = j.value("steer_full_deg", c.steer_full_deg);
            c.steer_curve = j.value("steer_curve", c.steer_curve);
            c.rate_assist = j.value("rate_assist", c.rate_assist);
            c.pitch_gain = j.value("pitch_gain", c.pitch_gain);
            c.pitch_min = j.value("pitch_min", c.pitch_min);
            if (j.contains("layout") && j["layout"].is_object()) {
                for (const auto& [id, p] : j["layout"].items()) {
                    c.layout[id] = { p.value("x", 0.5f), p.value("y", 0.5f), p.value("scale", 1.0f) };
                }
            }
        } catch (...) {
            fprintf(stderr, "[touch] %s is not valid JSON; using defaults\n", path.c_str());
        }
        return c;
    }

    bool save_config(const Config& c, const std::string& path) {
        json j = { { "enabled", c.enabled }, { "opacity", c.opacity }, { "button_scale", c.button_scale },
                   { "stick_radius", c.stick_radius }, { "stick_curve", c.stick_curve }, { "gyro", c.gyro },
                   { "debug_hitboxes", c.debug_hitboxes }, { "stick_split", c.stick_split },
                   { "steer_dead_deg", c.steer_dead_deg }, { "steer_full_deg", c.steer_full_deg },
                   { "steer_curve", c.steer_curve }, { "rate_assist", c.rate_assist },
                   { "pitch_gain", c.pitch_gain }, { "pitch_min", c.pitch_min } };
        json layout = json::object();
        for (const auto& [id, p] : c.layout) {
            layout[id] = { { "x", p.cx }, { "y", p.cy }, { "scale", p.scale } };
        }
        j["layout"] = layout;
        std::ofstream f(path);
        if (!f.is_open()) {
            return false;
        }
        f << j.dump(2) << "\n";
        return true;
    }

    Config& shared_config() {
        static Config s = load_config(config_path());
        return s;
    }

    bool gyro_enabled() {
        return shared_config().gyro;
    }

    void set_gyro_enabled(bool on) {
        shared_config().gyro = on;
        save_config(shared_config(), config_path());
    }
}
