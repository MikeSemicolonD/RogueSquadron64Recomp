#include "input/touch_config.h"

#include <cstdio>
#include <fstream>
#include "json/json.hpp"
#include "util/settings_file.h"

using nlohmann::json;

namespace rs64::touch {
    template <class C, class F>
    void fields(C& c, F&& f)
    {

        f("enabled", c.enabled);
        f("opacity", c.opacity);
        f("button_scale", c.button_scale);
        f("stick_radius", c.stick_radius);
        f("stick_curve", c.stick_curve);
        f("gyro", c.gyro);
        f("debug_hitboxes", c.debug_hitboxes);
        f("stick_split", c.stick_split);
        f("steer_dead_deg", c.steer_dead_deg);
        f("steer_full_deg", c.steer_full_deg);
        f("steer_curve", c.steer_curve);
        f("rate_assist", c.rate_assist);
        f("pitch_gain", c.pitch_gain);
        f("pitch_min", c.pitch_min);
        f("thumb_bridge", c.thumb_bridge);
    }

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

        fields(c, rs64::settings::reader(j, path));

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

        json j = json::object();

        fields(c, rs64::settings::writer(j));

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
