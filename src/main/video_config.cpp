#include "video_config.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>

namespace rs64::video {

using nlohmann::json;
using UC = RT64::UserConfiguration;

namespace {
template <class T>
void adv_get(const json& a, const char* key, T& out) {
    if (a.contains(key) && !a.at(key).is_null()) out = a.at(key).get<T>();
}

std::atomic<float> g_draw_distance{1.0f};
} // namespace

float draw_distance() {
    return g_draw_distance.load(std::memory_order_relaxed);
}

void set_draw_distance(float mult) {
    g_draw_distance.store(std::clamp(mult, kDrawDistanceMin, kDrawDistanceMax), std::memory_order_relaxed);
}

void apply_friendly(UC& uc, const json& j) {
    auto has = [&](const char* k) { return j.contains(k) && !j.at(k).is_null(); };

    if (has("drawDistance") && j["drawDistance"].is_number()) {
        set_draw_distance(j["drawDistance"].get<float>());
    }

    if (has("widescreen")) {
        uc.aspectRatio = j["widescreen"].get<bool>() ? UC::AspectRatio::Expand
                                                     : UC::AspectRatio::Original;
    }
    if (has("resolutionScale")) {
        const json& r = j["resolutionScale"];
        if (r.is_string() && r.get<std::string>() == "auto") {
            uc.resolution = UC::Resolution::WindowIntegerScale;
        } else if (r.is_number()) {
            double m = r.get<double>();
            if (m == 1.0) {
                uc.resolution = UC::Resolution::Original;
            } else {
                uc.resolution = UC::Resolution::Manual;
                uc.resolutionMultiplier = m;
            }
        }
    }
    if (has("antialiasing")) {
        int a = j["antialiasing"].get<int>();
        uc.antialiasing = (a >= 8) ? UC::Antialiasing::MSAA8X
                        : (a >= 4) ? UC::Antialiasing::MSAA4X
                        : (a >= 2) ? UC::Antialiasing::MSAA2X
                                   : UC::Antialiasing::None;
    }
    if (has("supersampling")) uc.downsampleMultiplier = j["supersampling"].get<int>();
    if (has("smoothUpscale")) {
        uc.filtering = j["smoothUpscale"].get<bool>() ? UC::Filtering::AntiAliasedPixelScaling
                                                      : UC::Filtering::Nearest;
    }
    if (has("smoothTextures")) uc.threePointFiltering = !j["smoothTextures"].get<bool>();
    if (has("targetFramerate")) uc.refreshRateTarget = j["targetFramerate"].get<int>();
    if (has("frameInterpolation")) {
        uc.refreshRate = j["frameInterpolation"].get<bool>() ? UC::RefreshRate::Manual
                                                             : UC::RefreshRate::Original;
    }
    if (has("hdr")) {
        uc.internalColorFormat = j["hdr"].get<bool>() ? UC::InternalColorFormat::High
                                                      : UC::InternalColorFormat::Automatic;
    }
    if (has("tripleBuffering")) {
        uc.displayBuffering = j["tripleBuffering"].get<bool>() ? UC::DisplayBuffering::Triple
                                                              : UC::DisplayBuffering::Double;
    }
    if (has("graphicsAPI")) {
        std::string a = j["graphicsAPI"].get<std::string>();
        uc.graphicsAPI = (a == "vulkan") ? UC::GraphicsAPI::Vulkan
                       : (a == "d3d12")  ? UC::GraphicsAPI::D3D12
                       : (a == "metal")  ? UC::GraphicsAPI::Metal
                                         : UC::GraphicsAPI::Automatic;
    }
    if (has("developerMode")) uc.developerMode = j["developerMode"].get<bool>();

    if (j.contains("advanced") && j["advanced"].is_object()) {
        const json& a = j["advanced"];
        adv_get(a, "graphicsAPI", uc.graphicsAPI);
        adv_get(a, "resolution", uc.resolution);
        adv_get(a, "displayBuffering", uc.displayBuffering);
        adv_get(a, "antialiasing", uc.antialiasing);
        adv_get(a, "filtering", uc.filtering);
        adv_get(a, "aspectRatio", uc.aspectRatio);
        adv_get(a, "extAspectRatio", uc.extAspectRatio);
        adv_get(a, "upscale2D", uc.upscale2D);
        adv_get(a, "refreshRate", uc.refreshRate);
        adv_get(a, "internalColorFormat", uc.internalColorFormat);
        adv_get(a, "hardwareResolve", uc.hardwareResolve);
        adv_get(a, "resolutionMultiplier", uc.resolutionMultiplier);
        adv_get(a, "downsampleMultiplier", uc.downsampleMultiplier);
        adv_get(a, "aspectTarget", uc.aspectTarget);
        adv_get(a, "extAspectTarget", uc.extAspectTarget);
        adv_get(a, "refreshRateTarget", uc.refreshRateTarget);
        adv_get(a, "idleWorkActive", uc.idleWorkActive);
        adv_get(a, "developerMode", uc.developerMode);
    }

    uc.validate();
}

json to_friendly(const UC& uc) {
    json j;
    j["schema"] = 2;
    j["drawDistance"] = std::round(draw_distance() * 100.0f) / 100.0f;
    json adv = json::object();

    // widescreen <-> aspectRatio
    if (uc.aspectRatio == UC::AspectRatio::Expand) {
        j["widescreen"] = true;
    } else if (uc.aspectRatio == UC::AspectRatio::Original) {
        j["widescreen"] = false;
    } else {
        adv["aspectRatio"] = uc.aspectRatio;
        adv["aspectTarget"] = uc.aspectTarget;
    }

    // resolutionScale <-> resolution + resolutionMultiplier
    switch (uc.resolution) {
        case UC::Resolution::WindowIntegerScale: j["resolutionScale"] = "auto"; break;
        case UC::Resolution::Original:           j["resolutionScale"] = 1; break;
        default:                                 j["resolutionScale"] = uc.resolutionMultiplier; break;
    }

    switch (uc.antialiasing) {
        case UC::Antialiasing::MSAA2X: j["antialiasing"] = 2; break;
        case UC::Antialiasing::MSAA4X: j["antialiasing"] = 4; break;
        case UC::Antialiasing::MSAA8X: j["antialiasing"] = 8; break;
        default:                       j["antialiasing"] = 0; break;
    }

    j["supersampling"] = uc.downsampleMultiplier;

    if (uc.filtering == UC::Filtering::AntiAliasedPixelScaling) {
        j["smoothUpscale"] = true;
    } else if (uc.filtering == UC::Filtering::Nearest) {
        j["smoothUpscale"] = false;
    } else {
        adv["filtering"] = uc.filtering;
    }

    j["smoothTextures"] = !uc.threePointFiltering;

    if (uc.refreshRate == UC::RefreshRate::Manual) {
        j["frameInterpolation"] = true;
    } else if (uc.refreshRate == UC::RefreshRate::Original) {
        j["frameInterpolation"] = false;
    } else {
        adv["refreshRate"] = uc.refreshRate;
    }
    j["targetFramerate"] = uc.refreshRateTarget;

    if (uc.internalColorFormat == UC::InternalColorFormat::High) {
        j["hdr"] = true;
    } else if (uc.internalColorFormat == UC::InternalColorFormat::Automatic) {
        j["hdr"] = false;
    } else {
        adv["internalColorFormat"] = uc.internalColorFormat;
    }

    j["tripleBuffering"] = (uc.displayBuffering == UC::DisplayBuffering::Triple);

    switch (uc.graphicsAPI) {
        case UC::GraphicsAPI::Vulkan: j["graphicsAPI"] = "vulkan"; break;
        case UC::GraphicsAPI::D3D12:  j["graphicsAPI"] = "d3d12"; break;
        case UC::GraphicsAPI::Metal:  j["graphicsAPI"] = "metal"; break;
        default:                      j["graphicsAPI"] = "auto"; break;
    }

    j["developerMode"] = uc.developerMode;

    // Fields with no friendly key: always mirrored into advanced so a round-trip
    // against any baseline is exact and the raw knobs stay discoverable.
    adv["upscale2D"] = uc.upscale2D;
    adv["hardwareResolve"] = uc.hardwareResolve;
    adv["idleWorkActive"] = uc.idleWorkActive;
    adv["extAspectRatio"] = uc.extAspectRatio;
    adv["extAspectTarget"] = uc.extAspectTarget;

    j["advanced"] = adv;
    return j;
}

LoadResult load(UC& uc, const std::string& path) {
    LoadResult r;
    std::ifstream in(path);
    if (!in.is_open()) return r;
    json j;
    try { in >> j; } catch (...) { return r; }
    r.loaded = true;

    if (j.contains("schema") && j["schema"].is_number_integer() && j["schema"].get<int>() >= 2) {
        apply_friendly(uc, j);
        // Rewrite files that predate a friendly key so it shows up for editing.
        r.migrated = !j.contains("drawDistance");
    } else {
        // Legacy raw UserConfiguration: merge onto the current baseline, same as
        // the pre-migration loader, then flag for rewrite in friendly form.
        json base = uc;            // RT64 to_json
        base.update(j);
        try { uc = base.get<UC>(); } catch (...) { return r; }
        uc.validate();
        r.migrated = true;
    }
    return r;
}

} // namespace rs64::video
