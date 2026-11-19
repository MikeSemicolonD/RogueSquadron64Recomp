#include "input/touch_config.h"

#include <cstdio>
#include <fstream>
#include "json/json.hpp"
#include "util/settings_file.h"

using nlohmann::json;

namespace rs64::touch {
    // In the working directory with saves and mods: the exe's folder can be read-only (AppImage, Flatpak).
    std::string config_path() {
        return "roguesq_touch.json";
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

        json j;

        if (!rs64::settings::parse_file(f, path, j)) {
            fprintf(stderr, "[touch] %s is not valid JSON; using defaults\n", path.c_str());

            return c;
        }

        using rs64::settings::value_or;

        c.enabled = value_or(j, "enabled", c.enabled);
        c.opacity = value_or(j, "opacity", c.opacity);
        c.button_scale = value_or(j, "button_scale", c.button_scale);
        c.stick_radius = value_or(j, "stick_radius", c.stick_radius);
        c.stick_curve = value_or(j, "stick_curve", c.stick_curve);
        c.gyro = value_or(j, "gyro", c.gyro);
        c.debug_hitboxes = value_or(j, "debug_hitboxes", c.debug_hitboxes);
        c.stick_split = value_or(j, "stick_split", c.stick_split);
        c.steer_dead_deg = value_or(j, "steer_dead_deg", c.steer_dead_deg);
        c.steer_full_deg = value_or(j, "steer_full_deg", c.steer_full_deg);
        c.steer_curve = value_or(j, "steer_curve", c.steer_curve);
        c.rate_assist = value_or(j, "rate_assist", c.rate_assist);
        c.pitch_gain = value_or(j, "pitch_gain", c.pitch_gain);
        c.pitch_min = value_or(j, "pitch_min", c.pitch_min);
        c.thumb_bridge = value_or(j, "thumb_bridge", c.thumb_bridge);

        if (j.contains("layout") && j["layout"].is_object()) {
            for (const auto& [id, p] : j["layout"].items())
            {
                c.layout[id] = { value_or(p, "x", 0.5f), value_or(p, "y", 0.5f), value_or(p, "scale", 1.0f) };
            }
        }

        return c;
    }

    bool save_config(const Config& c, const std::string& path)
    {

        json j = { { "enabled", c.enabled }, { "opacity", c.opacity }, { "button_scale", c.button_scale },
                   { "stick_radius", c.stick_radius }, { "stick_curve", c.stick_curve }, { "gyro", c.gyro },
                   { "debug_hitboxes", c.debug_hitboxes }, { "stick_split", c.stick_split },
                   { "steer_dead_deg", c.steer_dead_deg }, { "steer_full_deg", c.steer_full_deg },
                   { "steer_curve", c.steer_curve }, { "rate_assist", c.rate_assist },
                   { "pitch_gain", c.pitch_gain }, { "pitch_min", c.pitch_min }, { "thumb_bridge", c.thumb_bridge } };
        json layout = json::object();

        for (const auto& [id, p] : c.layout) {
            layout[id] = { { "x", p.cx }, { "y", p.cy }, { "scale", p.scale } };
        }

        j["layout"] = layout;

        return rs64::settings::write_file(path, j.dump(2) + "\n");
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
