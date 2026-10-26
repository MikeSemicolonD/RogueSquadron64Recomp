#include "video/video_config.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <mutex>
#include <utility>

namespace rs64::video {

using nlohmann::json;
using UC = RT64::UserConfiguration;

namespace {
template <class T>
void adv_get(const json& a, const char* key, T& out)
{
    if (a.contains(key) && !a.at(key).is_null()) out = a.at(key).get<T>();
}

std::atomic<float> g_draw_distance{1.0f};
std::atomic<bool> g_keep_cutscene{true};
std::atomic<bool> g_legacy_terrain{false};
std::atomic<bool> g_fb_writeback{true};
std::atomic<bool> g_d3d10{false};
std::atomic<bool> g_fullscreen{false};
std::atomic<bool> g_ray_query{false};

// TARGET FPS = DISPLAY (refreshRate Display while interpolating); remembered while interpolation is off.
std::atomic<bool> g_target_display{false};

UC::RefreshRate interp_rate()
{
    return g_target_display.load(std::memory_order_relaxed) ? UC::RefreshRate::Display : UC::RefreshRate::Manual;
}

// The menu's copy of the RT64 fields it owns: stepped on the game thread, applied to the live config by the renderer.
std::mutex g_display_mutex;
UC g_desired;
bool g_pending = false;
std::vector<int> g_refresh_rates;

struct AaPreset { UC::Antialiasing msaa; int ssaa; bool fxaa; const char* name; };

const AaPreset kAaPresets[] = {
    { UC::Antialiasing::None,   1, false, "OFF" },
    { UC::Antialiasing::None,   1, true,  "FXAA" },
    { UC::Antialiasing::MSAA2X, 1, false, "MSAA 2X" },
    { UC::Antialiasing::MSAA4X, 1, false, "MSAA 4X" },
    { UC::Antialiasing::MSAA8X, 1, false, "MSAA 8X" },
    { UC::Antialiasing::None,   2, false, "SSAA 2X" },
    { UC::Antialiasing::None,   4, false, "SSAA 4X" },
};

constexpr int kAaPresetCount = (int)(sizeof(kAaPresets) / sizeof(kAaPresets[0]));
const char* const kResolutionNames[] = { "AUTO", "1X", "2X", "3X", "4X" };
const char* const kUpscaleNames[] = { "NEAREST", "LINEAR", "AA PIXEL" };
const char* const kTextureNames[] = { "BILINEAR", "3-POINT" };
const char* const kOnOff[] = { "OFF", "ON" };

// TARGET FPS choices: 0 = DISPLAY, then the monitor's rates, with a saved rate it doesn't list kept in order.
std::vector<int> target_choices(const UC& uc)
{

    std::vector<int> v = g_refresh_rates;

    if (!g_target_display.load(std::memory_order_relaxed) && std::find(v.begin(), v.end(), uc.refreshRateTarget) == v.end())
    {
        v.insert(std::upper_bound(v.begin(), v.end(), uc.refreshRateTarget), uc.refreshRateTarget);
    }

    v.insert(v.begin(), 0);

    return v;
}

int choice_of(const UC& uc, Display d)
{

    switch (d) {
        case Display::Resolution:         return resolution_choice(uc);
        case Display::Widescreen:         return uc.aspectRatio == UC::AspectRatio::Expand ? 1 : 0;
        case Display::AntiAliasing:       return aa_preset(uc);
        case Display::FrameInterpolation: return uc.refreshRate != UC::RefreshRate::Original ? 1 : 0;
        case Display::TextureFilter:      return uc.threePointFiltering ? 1 : 0;
        case Display::UpscaleFilter:      return upscale_choice(uc);
        case Display::TargetFps:          break;
    }

    if (g_target_display.load(std::memory_order_relaxed)) return 0;

    const std::vector<int> v = target_choices(uc);

    return (int)(std::find(v.begin(), v.end(), uc.refreshRateTarget) - v.begin());
}

int count_of(const UC& uc, Display d)
{

    switch (d) {
        case Display::Resolution:    return 5;
        case Display::AntiAliasing:  return kAaPresetCount;
        case Display::UpscaleFilter: return 3;
        case Display::TargetFps:     return (int)target_choices(uc).size();
        default:                     return 2;
    }
}

void step_in(UC& uc, Display d)
{

    // A combination matching no choice (CUSTOM) steps to the first real one: FXAA for anti-aliasing, the first choice otherwise.
    const int cur = choice_of(uc, d);
    const int next = cur < 0 ? (d == Display::AntiAliasing ? 1 : 0) : (cur + 1) % count_of(uc, d);

    switch (d) {
        case Display::Resolution:
            apply_resolution_choice(uc, next);
            break;
        case Display::Widescreen:
            uc.aspectRatio = next ? UC::AspectRatio::Expand : UC::AspectRatio::Original;
            break;
        case Display::AntiAliasing:
            apply_aa_preset(uc, next);
            break;
        case Display::TextureFilter:
            uc.threePointFiltering = next != 0;
            break;
        case Display::UpscaleFilter:
            apply_upscale_choice(uc, next);
            break;
        case Display::FrameInterpolation:
            uc.refreshRate = next ? interp_rate() : UC::RefreshRate::Original;
            break;
        case Display::TargetFps:
        {
            const std::vector<int> v = target_choices(uc);

            g_target_display.store(next == 0, std::memory_order_relaxed);

            if (next != 0) uc.refreshRateTarget = v[next];
            if (uc.refreshRate != UC::RefreshRate::Original) uc.refreshRate = interp_rate();

            break;
        }
    }
}

// -1 = follow the setting; 0/1 = ROGUESQ_FULLSCREEN pins the live state until the player toggles, and the file keeps the setting.
std::atomic<int> g_fullscreen_pin{-1};

std::mutex g_language_mutex;
std::string g_language;
std::string g_language_pin;

// "rayTracing" in roguesq_video.json. The file keeps the player's setting; a set ROGUESQ_RT_* env var only pins the live switch, so test runs never rewrite it.
const std::pair<const char*, rs64lights::Feature> kRtKeys[] = {
    { "lights", rs64lights::Feature::Lights },
    { "shadows", rs64lights::Feature::Shadows },
    { "softShadows", rs64lights::Feature::SoftShadows },
    { "fogShafts", rs64lights::Feature::FogShafts },
    { "ambientOcclusion", rs64lights::Feature::AmbientOcclusion },
    { "globalIllumination", rs64lights::Feature::GlobalIllumination },
    { "reflections", rs64lights::Feature::Reflections },
};

std::atomic<bool> g_rt_setting[7] = { false, false, true, false, false, false, false };
} // namespace

bool rt_setting(rs64lights::Feature f)
{
    return g_rt_setting[static_cast<int>(f)].load(std::memory_order_relaxed);
}

void set_rt_setting(rs64lights::Feature f, bool on)
{

    g_rt_setting[static_cast<int>(f)].store(on, std::memory_order_relaxed);
    rs64lights::setFeature(f, on);
}

float draw_distance() {
    return g_draw_distance.load(std::memory_order_relaxed);
}

void set_draw_distance(float mult)
{
    g_draw_distance.store(std::clamp(mult, kDrawDistanceMin, draw_distance_max()), std::memory_order_relaxed);
}

float draw_distance_max() {
    return legacy_terrain() ? kLegacyTerrainDrawDistanceMax : kDrawDistanceMax;
}

bool keep_cutscene_draw_distance() {
    return g_keep_cutscene.load(std::memory_order_relaxed);
}

bool legacy_terrain() {
    return g_legacy_terrain.load(std::memory_order_relaxed);
}

void set_legacy_terrain(bool on)
{

    g_legacy_terrain.store(on, std::memory_order_relaxed);
    set_draw_distance(draw_distance());
}

void set_keep_cutscene_draw_distance(bool on) {
    g_keep_cutscene.store(on, std::memory_order_relaxed);
}

bool framebuffer_writeback() {
    return g_fb_writeback.load(std::memory_order_relaxed);
}

void set_framebuffer_writeback(bool on) {
    g_fb_writeback.store(on, std::memory_order_relaxed);
}

bool ray_query_supported() {
    return g_ray_query.load(std::memory_order_relaxed);
}

void set_ray_query_supported(bool on) {
    g_ray_query.store(on, std::memory_order_relaxed);
}

int aa_preset(const UC& uc)
{

    for (int i = 0; i < kAaPresetCount; ++i)
    {
        const AaPreset& p = kAaPresets[i];

        if (uc.antialiasing == p.msaa && uc.downsampleMultiplier == p.ssaa && uc.fxaa == p.fxaa) return i;
    }

    return -1;
}

void apply_aa_preset(UC& uc, int preset)
{

    const AaPreset& p = kAaPresets[std::clamp(preset, 0, kAaPresetCount - 1)];

    uc.antialiasing = p.msaa;
    uc.downsampleMultiplier = p.ssaa;
    uc.fxaa = p.fxaa;
}

int resolution_choice(const UC& uc)
{

    if (uc.resolution == UC::Resolution::WindowIntegerScale) return 0;
    if (uc.resolution == UC::Resolution::Original) return 1;

    const double m = uc.resolutionMultiplier;

    return (m == 2.0 || m == 3.0 || m == 4.0) ? (int)m : -1;
}

void apply_resolution_choice(UC& uc, int choice)
{

    if (choice <= 0) {
        uc.resolution = UC::Resolution::WindowIntegerScale;
    } else if (choice == 1) {
        uc.resolution = UC::Resolution::Original;
    } else {
        uc.resolution = UC::Resolution::Manual;
        uc.resolutionMultiplier = (double)std::min(choice, 4);
    }
}

int upscale_choice(const UC& uc)
{

    switch (uc.filtering) {
        case UC::Filtering::Nearest: return 0;
        case UC::Filtering::Linear:  return 1;
        default:                     return 2;
    }
}

void apply_upscale_choice(UC& uc, int choice)
{
    uc.filtering = choice <= 0 ? UC::Filtering::Nearest : choice == 1 ? UC::Filtering::Linear : UC::Filtering::AntiAliasedPixelScaling;
}

int display_value(Display d)
{

    std::lock_guard<std::mutex> lock(g_display_mutex);

    return choice_of(g_desired, d);
}

void step_display(Display d)
{

    std::lock_guard<std::mutex> lock(g_display_mutex);

    step_in(g_desired, d);
    g_pending = true;
}

int display_choice_count(Display d)
{

    std::lock_guard<std::mutex> lock(g_display_mutex);

    return count_of(g_desired, d);
}

std::string display_label(Display d, int choice)
{

    if (choice < 0) return "CUSTOM";

    switch (d) {
        case Display::Resolution:    return kResolutionNames[std::min(choice, 4)];
        case Display::AntiAliasing:  return kAaPresets[std::min(choice, kAaPresetCount - 1)].name;
        case Display::UpscaleFilter: return kUpscaleNames[std::min(choice, 2)];
        case Display::TextureFilter: return kTextureNames[choice ? 1 : 0];
        case Display::TargetFps:     break;
        default:                     return kOnOff[choice ? 1 : 0];
    }

    if (choice == 0) return "DISPLAY";

    std::lock_guard<std::mutex> lock(g_display_mutex);
    const std::vector<int> v = target_choices(g_desired);

    return choice < (int)v.size() ? std::to_string(v[choice]) : "DISPLAY";
}

bool frame_interpolation_on()
{

    std::lock_guard<std::mutex> lock(g_display_mutex);

    return g_desired.refreshRate != UC::RefreshRate::Original;
}

ApplyResult apply_pending(UC& uc)
{

    std::lock_guard<std::mutex> lock(g_display_mutex);
    ApplyResult r;

    // No menu change waiting: mirror the live config (the F1 inspector and env overrides edit it directly).
    if (!g_pending)
    {
        g_desired = uc;

        return r;
    }

    const UC& d = g_desired;

    r.msaa = d.antialiasing != uc.antialiasing;
    r.discard_fbs = d.resolution != uc.resolution || d.resolutionMultiplier != uc.resolutionMultiplier || d.downsampleMultiplier != uc.downsampleMultiplier;
    r.changed = true;
    uc.aspectRatio = d.aspectRatio;
    uc.resolution = d.resolution;
    uc.resolutionMultiplier = d.resolutionMultiplier;
    uc.antialiasing = d.antialiasing;
    uc.downsampleMultiplier = d.downsampleMultiplier;
    uc.fxaa = d.fxaa;
    uc.refreshRate = d.refreshRate;
    uc.refreshRateTarget = d.refreshRateTarget;
    uc.threePointFiltering = d.threePointFiltering;
    uc.filtering = d.filtering;
    g_pending = false;

    return r;
}

void set_refresh_rates(std::vector<int> hz)
{

    std::sort(hz.begin(), hz.end());
    hz.erase(std::unique(hz.begin(), hz.end()), hz.end());

    std::lock_guard<std::mutex> lock(g_display_mutex);

    g_refresh_rates = std::move(hz);
}

bool d3d10() {
    return g_d3d10.load(std::memory_order_relaxed);
}

void set_d3d10(bool on) {
    g_d3d10.store(on, std::memory_order_relaxed);
}

bool fullscreen()
{

    const int pin = g_fullscreen_pin.load(std::memory_order_relaxed);

    return pin >= 0 ? pin != 0 : g_fullscreen.load(std::memory_order_relaxed);
}

void set_fullscreen(bool on) {

    g_fullscreen_pin.store(-1, std::memory_order_relaxed);
    g_fullscreen.store(on, std::memory_order_relaxed);
}

void pin_fullscreen(bool on) {
    g_fullscreen_pin.store(on ? 1 : 0, std::memory_order_relaxed);
}

std::string language()
{

    std::lock_guard lock(g_language_mutex);

    return g_language_pin.empty() ? g_language : g_language_pin;
}

void set_language(const std::string& code)
{

    std::lock_guard lock(g_language_mutex);

    g_language_pin.clear();
    g_language = code;
}

void pin_language(const std::string& code)
{

    std::lock_guard lock(g_language_mutex);

    g_language_pin = code;
}

bool peek_fullscreen(const std::string& path)
{

    std::ifstream in(path);

    if (!in.is_open()) return false;

    json j;
    try { in >> j; } catch (...) { return false; }

    return j.is_object() && j.contains("fullscreen") && j["fullscreen"].is_boolean() && j["fullscreen"].get<bool>();
}

float effective_draw_distance(float setting, bool game_view, uint32_t level) {
    return (game_view || level == kGameViewLevel) ? 1.0f : setting;
}

void apply_friendly(UC& uc, const json& j)
{

    auto has = [&](const char* k) { return j.contains(k) && !j.at(k).is_null(); };

    if (has("drawDistance") && j["drawDistance"].is_number()) {
        set_draw_distance(j["drawDistance"].get<float>());
    }

    if (has("keepCutsceneDrawDistance") && j["keepCutsceneDrawDistance"].is_boolean())
    {
        set_keep_cutscene_draw_distance(j["keepCutsceneDrawDistance"].get<bool>());
    }

    if (has("legacyTerrain") && j["legacyTerrain"].is_boolean()) {
        set_legacy_terrain(j["legacyTerrain"].get<bool>());
    }

    if (has("framebufferWriteback") && j["framebufferWriteback"].is_boolean()) {
        set_framebuffer_writeback(j["framebufferWriteback"].get<bool>());
    }

    if (has("fullscreen") && j["fullscreen"].is_boolean())
    {
        g_fullscreen.store(j["fullscreen"].get<bool>(), std::memory_order_relaxed);
    }

    if (has("language") && j["language"].is_string())
    {
        std::lock_guard lock(g_language_mutex);

        g_language = j["language"].get<std::string>();
    }

    if (has("rayTracing") && j["rayTracing"].is_object()) {
        const json& rt = j["rayTracing"];

        for (const auto& [key, feature] : kRtKeys) {
            if (rt.contains(key) && rt.at(key).is_boolean()) {
                set_rt_setting(feature, rt.at(key).get<bool>());
            }
        }
    }

    if (has("widescreen"))
    {
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
    if (has("ssaa")) uc.downsampleMultiplier = j["ssaa"].get<int>();
    if (has("fxaa")) uc.fxaa = j["fxaa"].get<bool>();

    if (has("smoothUpscale"))
    {
        uc.filtering = j["smoothUpscale"].get<bool>() ? UC::Filtering::AntiAliasedPixelScaling
                                                      : UC::Filtering::Nearest;
    }

    if (has("smoothTextures")) uc.threePointFiltering = !j["smoothTextures"].get<bool>();
    // targetFramerate is a number or "display" (follow the monitor); it is kept while interpolation is off.
    if (has("targetFramerate"))
    {
        const json& t = j["targetFramerate"];

        if (t.is_string()) {
            g_target_display.store(t.get<std::string>() == "display", std::memory_order_relaxed);
        } else if (t.is_number()) {
            uc.refreshRateTarget = t.get<int>();
            g_target_display.store(false, std::memory_order_relaxed);
        }

        if (uc.refreshRate != UC::RefreshRate::Original) uc.refreshRate = interp_rate();
    }

    if (has("frameInterpolation")) uc.refreshRate = j["frameInterpolation"].get<bool>() ? interp_rate() : UC::RefreshRate::Original;

    if (has("hdr"))
    {
        uc.internalColorFormat = j["hdr"].get<bool>() ? UC::InternalColorFormat::High
                                                      : UC::InternalColorFormat::Automatic;
    }

    if (has("tripleBuffering"))
    {
        uc.displayBuffering = j["tripleBuffering"].get<bool>() ? UC::DisplayBuffering::Triple
                                                              : UC::DisplayBuffering::Double;
    }

    if (has("graphicsAPI")) {
        std::string a = j["graphicsAPI"].get<std::string>();
        uc.graphicsAPI = (a == "vulkan") ? UC::GraphicsAPI::Vulkan
                       : (a == "d3d12")  ? UC::GraphicsAPI::D3D12
                       : (a == "metal")  ? UC::GraphicsAPI::Metal
                       : ((a == "d3d11") || (a == "d3d10")) ? UC::GraphicsAPI::D3D11
                                         : UC::GraphicsAPI::Automatic;
        set_d3d10(a == "d3d10");
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

    if (uc.refreshRate == UC::RefreshRate::Display) g_target_display.store(true, std::memory_order_relaxed);

    uc.validate();
}

json to_friendly(const UC& uc)
{

    json j;
    j["schema"] = 2;
    j["drawDistance"] = std::round(draw_distance() * 100.0f) / 100.0f;
    j["keepCutsceneDrawDistance"] = keep_cutscene_draw_distance();
    j["legacyTerrain"] = legacy_terrain();
    j["framebufferWriteback"] = framebuffer_writeback();
    j["fullscreen"] = g_fullscreen.load(std::memory_order_relaxed);

    std::unique_lock lock(g_language_mutex);

    if (!g_language.empty()) {
        j["language"] = g_language;
    }

    lock.unlock();

    json rt = json::object();

    for (const auto& [key, feature] : kRtKeys) {
        rt[key] = rt_setting(feature);
    }

    j["rayTracing"] = rt;
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
    switch (uc.resolution)
    {
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

    j["ssaa"] = uc.downsampleMultiplier;
    j["fxaa"] = uc.fxaa;

    if (uc.filtering == UC::Filtering::AntiAliasedPixelScaling) {
        j["smoothUpscale"] = true;
    } else if (uc.filtering == UC::Filtering::Nearest) {
        j["smoothUpscale"] = false;
    } else {
        adv["filtering"] = uc.filtering;
    }

    j["smoothTextures"] = !uc.threePointFiltering;

    const bool target_display = g_target_display.load(std::memory_order_relaxed) || uc.refreshRate == UC::RefreshRate::Display;

    j["frameInterpolation"] = uc.refreshRate != UC::RefreshRate::Original;

    if (target_display) {
        j["targetFramerate"] = "display";
    } else {
        j["targetFramerate"] = uc.refreshRateTarget;
    }

    if (uc.internalColorFormat == UC::InternalColorFormat::High)
    {
        j["hdr"] = true;
    } else if (uc.internalColorFormat == UC::InternalColorFormat::Automatic) {
        j["hdr"] = false;
    } else {
        adv["internalColorFormat"] = uc.internalColorFormat;
    }

    j["tripleBuffering"] = (uc.displayBuffering == UC::DisplayBuffering::Triple);

    switch (uc.graphicsAPI)
    {
        case UC::GraphicsAPI::Vulkan: j["graphicsAPI"] = "vulkan"; break;
        case UC::GraphicsAPI::D3D12:  j["graphicsAPI"] = "d3d12"; break;
        case UC::GraphicsAPI::Metal:  j["graphicsAPI"] = "metal"; break;
        case UC::GraphicsAPI::D3D11:  j["graphicsAPI"] = d3d10() ? "d3d10" : "d3d11"; break;
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

LoadResult load(UC& uc, const std::string& path)
{

    LoadResult r;
    std::ifstream in(path);

    if (!in.is_open()) return r;

    json j;
    try { in >> j; } catch (...) { return r; }
    r.loaded = true;

    if (j.contains("schema") && j["schema"].is_number_integer() && j["schema"].get<int>() >= 2)
    {
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
