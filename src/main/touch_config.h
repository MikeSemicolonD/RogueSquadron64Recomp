#pragma once

#include <string>
#include "touch_input.h"

// roguesq_touch.json: touch overlay + gyro settings, written with defaults when missing.
namespace rs64::touch {
    std::string config_path();
    Config load_config(const std::string& path);
    bool save_config(const Config& c, const std::string& path);
    // Process-wide instance, loaded on first use.
    Config& shared_config();
    bool gyro_enabled();
    void set_gyro_enabled(bool on);
}
