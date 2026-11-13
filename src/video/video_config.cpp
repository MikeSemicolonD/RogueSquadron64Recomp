#include "video/video_config.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include "util/debug_logs.h"

namespace rs64::video {

using nlohmann::json;
using UC = RT64::UserConfiguration;
using recomp::dbg::env_int;
using recomp::dbg::env_on;
using recomp::dbg::env_str;

namespace {
template <class T>
void adv_get(const json& a, const char* key, T& out)
{
    if (a.contains(key) && !a.at(key).is_null()) out = a.at(key).get<T>();
}

constexpr std::pair<int, int> kAspects[] = { {4, 3}, {5, 4}, {3, 2}, {16, 10}, {16, 9}, {21, 9}, {32, 9} };

json aspect_to_json(double a)
{

    for (const auto& [w, h] : kAspects) {
        if (std::fabs(a - double(w) / h) < 1e-4) {
            return std::to_string(w) + ":" + std::to_string(h);
        }
    }

    return std::round(a * 10000.0) / 10000.0;
}

// Accepts "16:9" or a plain width/height number.
void aspect_get(const json& a, const char* key, double& out)
{

    if (!a.contains(key)) return;

    const json& v = a.at(key);

    if (v.is_number()) {
        out = v.get<double>();
        return;
    }

    int w = 0;
    int h = 0;

    if (v.is_string() && std::sscanf(v.get<std::string>().c_str(), "%d:%d", &w, &h) == 2 && w > 0 && h > 0) {
        out = double(w) / h;
    }
}

std::atomic<float> g_draw_distance{1.0f};
std::atomic<bool> g_keep_cutscene{true};
std::atomic<bool> g_vr_mode{false};

std::atomic<bool> g_legacy_terrain{false};

#if defined(__ANDROID__)
std::atomic<int> g_terrain_tessellation{2};
#else
std::atomic<int> g_terrain_tessellation{4};
#endif

std::atomic<bool> g_fb_writeback{true};
std::atomic<bool> g_d3d10{false};
std::atomic<float> g_ssaa_base{1.0f};
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

// The settings read only at launch, as they were when the renderer started (first apply_pending). A new one adds a field here, in restart_snapshot and in restart_required.
struct RestartSettings
{
    UC::GraphicsAPI api{};
    bool d3d10 = false;
    bool vr = false;
};

std::optional<RestartSettings> g_started;

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

// The ANTI-ALIASING choice that renders: a saved SSAA above what the cap allows at this resolution shows as the step RT64 lowers it to.
int shown_aa_preset(const UC& uc)
{

    UC shown = uc;

    shown.downsampleMultiplier = RT64::rs64CappedSsaa(uc.downsampleMultiplier, g_ssaa_base.load(std::memory_order_relaxed));

    return aa_preset(shown);
}

const char* const kResolutionNames[] = { "AUTO", "1X", "2X", "3X", "4X" };
const char* const kUpscaleNames[] = { "NEAREST", "LINEAR", "AA PIXEL" };
const char* const kTextureNames[] = { "BILINEAR", "3-POINT" };
const char* const kOnOff[] = { "OFF", "ON" };

// RENDERER choices (Windows only); takes effect at the next start. A saved "d3d10" shows as D3D11.
const UC::GraphicsAPI kApiChoices[] = { UC::GraphicsAPI::Automatic, UC::GraphicsAPI::Vulkan, UC::GraphicsAPI::D3D12, UC::GraphicsAPI::D3D11 };
const char* const kApiNames[] = { "AUTO", "VULKAN", "D3D12", "D3D11" };
constexpr int kApiCount = 4;

int api_choice(const UC& uc)
{

    for (int i = 0; i < kApiCount; i++)
    {
        if (kApiChoices[i] == uc.graphicsAPI) return i;
    }

    return -1;
}

// TARGET FPS choices: 0 = DISPLAY, then the monitor's rates, with a saved rate it doesn't list kept in order.
std::vector<int> target_choices(const UC& uc)
{

    std::vector<int> v = g_refresh_rates;

    if (std::find(v.begin(), v.end(), 30) == v.end())
    {
        v.insert(std::upper_bound(v.begin(), v.end(), 30), 30);
    }

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
        case Display::AntiAliasing:       return shown_aa_preset(uc);
        case Display::FrameInterpolation: return uc.refreshRate != UC::RefreshRate::Original ? 1 : 0;
        case Display::TextureFilter:      return uc.threePointFiltering ? 1 : 0;
        case Display::UpscaleFilter:      return upscale_choice(uc);
        case Display::GraphicsApi:        return api_choice(uc);
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
        case Display::GraphicsApi:   return kApiCount;
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
        {

            // SSAA steps the cap would lower at this resolution are skipped, so the row never names one that doesn't render.
            const float base = g_ssaa_base.load(std::memory_order_relaxed);
            int pick = next;

            for (int i = 0; i < kAaPresetCount && RT64::rs64CappedSsaa(kAaPresets[pick].ssaa, base) != kAaPresets[pick].ssaa; ++i)
            {
                pick = (pick + 1) % kAaPresetCount;
            }

            apply_aa_preset(uc, pick);
            break;
        }
        case Display::TextureFilter:
            uc.threePointFiltering = next != 0;
            break;
        case Display::UpscaleFilter:
            apply_upscale_choice(uc, next);
            break;
        case Display::GraphicsApi:
            uc.graphicsAPI = kApiChoices[next];
            g_d3d10.store(false, std::memory_order_relaxed);
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

// "rayTracing.lightLimits": light slots per category (its default priority's share); "lasersMax" (priority -1) caps laser bolts in all.
const std::pair<const char*, int> kLightLimitKeys[] = {
    { "engines", 7 },
    { "pickups", 6 },
    { "effects", 5 },
    { "models", 3 },
    { "level", 2 },
    { "torpedoes", 1 },
    { "lasers", 0 },
    { "lasersMax", -1 },
};

std::atomic<int> g_light_limit[8] = { 12, 4, 10, 6, 4, 2, 4, 8 };

void set_light_limit(size_t i, int n)
{

    n = std::clamp(n, 0, static_cast<int>(rs64lights::MaxLights));
    g_light_limit[i].store(n, std::memory_order_relaxed);

    const int priority = kLightLimitKeys[i].second;

    if (priority >= 0) {
        rs64lights::g_priorityShare[priority] = static_cast<size_t>(n);
    } else {
        rs64lights::setMaxProjectileLights(n);
    }
}
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

float hud_placement() {
    return g_rs64_hud_placement.load(std::memory_order_relaxed);
}

void set_hud_placement(float placement) {
    g_rs64_hud_placement.store(std::clamp(placement, 0.0f, 1.0f), std::memory_order_relaxed);
}

bool legacy_terrain() {
    return g_legacy_terrain.load(std::memory_order_relaxed);
}

void set_legacy_terrain(bool on)
{

    g_legacy_terrain.store(on, std::memory_order_relaxed);
    set_draw_distance(draw_distance());
}



int terrain_tessellation() {
    return g_terrain_tessellation.load(std::memory_order_relaxed);
}

void set_terrain_tessellation(int quads)
{

    g_terrain_tessellation.store(std::clamp(quads, kTerrainTessellationMin, kTerrainTessellationMax), std::memory_order_relaxed);
}

void set_keep_cutscene_draw_distance(bool on) {
    g_keep_cutscene.store(on, std::memory_order_relaxed);
}

bool vr_mode() {
    return g_vr_mode.load(std::memory_order_relaxed);
}

void set_vr_mode(bool on) {
    g_vr_mode.store(on, std::memory_order_relaxed);
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

// One anti-aliasing method at a time, as the menu offers them: SSAA under MSAA multiplies the multisampled target (SSAA 2X + MSAA 4X ran at 2 fps where
// either alone ran at 57 and 221), so MSAA wins over SSAA and either over FXAA.
static void single_aa(UC& uc)
{

    const bool msaa = uc.antialiasing != UC::Antialiasing::None;
    const bool ssaa = uc.downsampleMultiplier > 1;

    if (msaa && ssaa) uc.downsampleMultiplier = 1;
    if ((msaa || ssaa) && uc.fxaa) uc.fxaa = false;
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
        case Display::GraphicsApi:   return kApiNames[std::min(choice, kApiCount - 1)];
        case Display::TargetFps:     break;
        default:                     return kOnOff[choice ? 1 : 0];
    }

    if (choice == 0) return "DISPLAY";

    std::lock_guard<std::mutex> lock(g_display_mutex);
    const std::vector<int> v = target_choices(g_desired);

    return choice < (int)v.size() ? std::to_string(v[choice]) : "DISPLAY";
}

int target_fps(int display_hz)
{

    std::lock_guard<std::mutex> lock(g_display_mutex);

    return g_target_display.load(std::memory_order_relaxed) ? display_hz : g_desired.refreshRateTarget;
}

bool frame_interpolation_on()
{

    std::lock_guard<std::mutex> lock(g_display_mutex);

    return g_desired.refreshRate != UC::RefreshRate::Original;
}

RestartSettings restart_snapshot(const UC& uc)
{
    return { uc.graphicsAPI, d3d10(), vr_mode() };
}

bool restart_required()
{

    std::lock_guard<std::mutex> lock(g_display_mutex);

    const RestartSettings now = restart_snapshot(g_desired);

    return g_started && (now.api != g_started->api || now.d3d10 != g_started->d3d10 || now.vr != g_started->vr);
}

ApplyResult apply_pending(UC& uc)
{

    std::lock_guard<std::mutex> lock(g_display_mutex);
    ApplyResult r;

    if (!g_started) g_started = restart_snapshot(uc);

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
    uc.graphicsAPI = d.graphicsAPI;
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

void set_ssaa_base(float scale)
{
    g_ssaa_base.store(scale, std::memory_order_relaxed);
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

std::string config_path()
{

    const char* path = env_str("ROGUESQ_VIDEO_CONFIG");

    return path ? path : "roguesq_video.json";
}

bool peek_fullscreen(const std::string& path)
{

    std::ifstream in(path);

    if (!in.is_open()) return false;

    json j;
    try { in >> j; } catch (...) { return false; }

    return j.is_object() && j.contains("fullscreen") && j["fullscreen"].is_boolean() && j["fullscreen"].get<bool>();
}

float effective_draw_distance(float setting, bool game_view) {
    return game_view ? 1.0f : setting;
}

void apply_friendly(UC& uc, const json& j)
{

    auto has = [&](const char* k) { return j.contains(k) && !j.at(k).is_null(); };

    if (has("drawDistance") && j["drawDistance"].is_number()) {
        set_draw_distance(j["drawDistance"].get<float>());
    }

    if (has("hudPlacement") && j["hudPlacement"].is_number()) {
        set_hud_placement(j["hudPlacement"].get<float>());
    }

    if (has("keepCutsceneDrawDistance") && j["keepCutsceneDrawDistance"].is_boolean())
    {
        set_keep_cutscene_draw_distance(j["keepCutsceneDrawDistance"].get<bool>());
    }

    if (has("vrMode") && j["vrMode"].is_boolean()) {
        set_vr_mode(j["vrMode"].get<bool>());
    }

    if (has("legacyTerrain") && j["legacyTerrain"].is_boolean()) {
        set_legacy_terrain(j["legacyTerrain"].get<bool>());
    }


    if (has("terrainTessellation") && j["terrainTessellation"].is_number()) {
        set_terrain_tessellation(j["terrainTessellation"].get<int>());
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

        if (rt.contains("lightLimits") && rt.at("lightLimits").is_object()) {
            const json& limits = rt.at("lightLimits");

            for (size_t i = 0; i < std::size(kLightLimitKeys); ++i) {
                const char* key = kLightLimitKeys[i].first;

                if (limits.contains(key) && limits.at(key).is_number_integer()) {
                    set_light_limit(i, limits.at(key).get<int>());
                }
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
        aspect_get(a, "aspectTarget", uc.aspectTarget);
        aspect_get(a, "extAspectTarget", uc.extAspectTarget);
        adv_get(a, "refreshRateTarget", uc.refreshRateTarget);
        adv_get(a, "idleWorkActive", uc.idleWorkActive);
        adv_get(a, "developerMode", uc.developerMode);
    }

    if (uc.refreshRate == UC::RefreshRate::Display) g_target_display.store(true, std::memory_order_relaxed);

    single_aa(uc);
    uc.validate();
}

json to_friendly(const UC& uc)
{

    json j;
    j["schema"] = 2;
    j["drawDistance"] = std::round(draw_distance() * 100.0f) / 100.0f;
    j["hudPlacement"] = std::round(hud_placement() * 100.0f) / 100.0f;
    j["keepCutsceneDrawDistance"] = keep_cutscene_draw_distance();
    j["vrMode"] = vr_mode();
    j["legacyTerrain"] = legacy_terrain();
    j["terrainTessellation"] = terrain_tessellation();
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

    json limits = json::object();

    for (size_t i = 0; i < std::size(kLightLimitKeys); ++i) {
        limits[kLightLimitKeys[i].first] = g_light_limit[i].load(std::memory_order_relaxed);
    }

    rt["lightLimits"] = limits;
    j["rayTracing"] = rt;
    json adv = json::object();

    // widescreen <-> aspectRatio
    if (uc.aspectRatio == UC::AspectRatio::Expand) {
        j["widescreen"] = true;
    } else if (uc.aspectRatio == UC::AspectRatio::Original) {
        j["widescreen"] = false;
    } else {
        adv["aspectRatio"] = uc.aspectRatio;
        adv["aspectTarget"] = aspect_to_json(uc.aspectTarget);
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
    adv["extAspectTarget"] = aspect_to_json(uc.extAspectTarget);

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
        r.migrated = !j.contains("drawDistance") || !j.contains("hudPlacement") || !j.contains("terrainTessellation");
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

// ROGUESQ_* overrides of the loaded config (they win over the file); each acts only when its var is explicitly set.
void apply_env_overrides(UC& uc)
{

    // ROGUESQ_MSAA=2|4|8: multisample anti-aliasing (default off).
    if (env_str("ROGUESQ_MSAA"))
    {
        int s = env_int("ROGUESQ_MSAA");
        uc.antialiasing = (s >= 8) ? UC::Antialiasing::MSAA8X
                                     : (s >= 4) ? UC::Antialiasing::MSAA4X
                                     : (s >= 2) ? UC::Antialiasing::MSAA2X
                                                : UC::Antialiasing::None;

        fprintf(stderr, "[RT64] MSAA=%ux\n", UC::msaaSampleCount(uc.antialiasing));
    }

    // ROGUESQ_RES_SCALE=<mult>: internal render-resolution multiplier (Manual).
    if (const char* v = env_str("ROGUESQ_RES_SCALE")) {
        double m = std::atof(v);

        if (m > 0.0) {
            uc.resolution = UC::Resolution::Manual;
            uc.resolutionMultiplier = m;

            fprintf(stderr, "[RT64] resolutionMultiplier=%.2f\n", m);
        }
    }

    // ROGUESQ_SSAA=<n>: supersample/downsample factor (>=2 sharper, costlier).
    int ssaa = env_int("ROGUESQ_SSAA");

    if (ssaa >= 1) {
        uc.downsampleMultiplier = ssaa;

        fprintf(stderr, "[RT64] downsample=%d\n", ssaa);
    }

    // ROGUESQ_FXAA=0|1: FXAA on the presented image.
    if (const char* v = env_str("ROGUESQ_FXAA")) {
        uc.fxaa = (v[0] != '0');

        fprintf(stderr, "[RT64] fxaa=%d\n", uc.fxaa ? 1 : 0);
    }

    single_aa(uc);

    // ROGUESQ_ASPECT=<w/h> forces a ratio; ROGUESQ_WIDESCREEN=1 expands to the window.
    if (const char* v = env_str("ROGUESQ_ASPECT")) {
        double a = std::atof(v);

        if (a > 0.0) {
            uc.aspectRatio = UC::AspectRatio::Manual;
            uc.aspectTarget = a;

            fprintf(stderr, "[RT64] aspect=Manual %.4f\n", a);
        }
    } else if (env_on("ROGUESQ_WIDESCREEN")) {
        uc.aspectRatio = UC::AspectRatio::Expand;

        fprintf(stderr, "[RT64] aspect=Expand\n");
    }

    // ROGUESQ_HDR=1: high internal color format.
    if (env_on("ROGUESQ_HDR")) {
        uc.internalColorFormat = UC::InternalColorFormat::High;

        fprintf(stderr, "[RT64] HDR internal color format\n");
    }

    // ROGUESQ_TEX_FILTER=nearest|linear|aa drives both the VI upscale filter (filtering) and the per-texture sampler (threePointFiltering).
    // threePointFiltering off forces GPU bilinear on every tile, G_TF_POINT sprites included; the upscale filter alone leaves game textures alone.
    if (const char* v = env_str("ROGUESQ_TEX_FILTER"))
    {
        std::string_view s(v);

        if (s == "nearest") { uc.filtering = UC::Filtering::Nearest; uc.threePointFiltering = true; }
        else if (s == "linear") { uc.filtering = UC::Filtering::Linear; uc.threePointFiltering = false; }
        else if (s == "aa") uc.filtering = UC::Filtering::AntiAliasedPixelScaling;

        fprintf(stderr, "[RT64] filtering=%s threePoint=%d\n", v, uc.threePointFiltering ? 1 : 0);
    }

    // ROGUESQ_THREE_POINT=0|1: per-texture sampler independent of the upscale filter.
    // 0 = GPU bilinear on all textures; 1 = N64 3-point (faithful; point on G_TF_POINT tiles).
    if (const char* v = env_str("ROGUESQ_THREE_POINT"))
    {
        uc.threePointFiltering = (v[0] != '0');

        fprintf(stderr, "[RT64] threePointFiltering=%d\n", uc.threePointFiltering ? 1 : 0);
    }

    // ROGUESQ_RT_INTERP=0 disables frame interpolation, =<hz> enables it at that target (raster-only, not the logic tick).
    // Unset keeps the baseline: off unless roguesq_video.json sets frameInterpolation.
    if (const char* v = env_str("ROGUESQ_RT_INTERP")) {
        if (v[0] == '0')
        {
            uc.refreshRate = UC::RefreshRate::Original;

            fprintf(stderr, "[RT64] frame interpolation OFF\n");
        } else {
            int hz = env_int("ROGUESQ_RT_INTERP");

            if (hz < 20) hz = 60;

            uc.refreshRate = UC::RefreshRate::Manual;
            uc.refreshRateTarget = hz;

            fprintf(stderr, "[RT64] frame interpolation ON (Manual %d Hz)\n", hz);
        }
    }

    // ROGUESQ_GFX_API=vulkan|d3d12|d3d11|d3d10 forces the backend (default Automatic).
    if (const char* api = env_str("ROGUESQ_GFX_API")) {
        if (std::string_view(api) == "vulkan")
        {
            uc.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;

            fprintf(stderr, "[RT64] graphics API forced to Vulkan via ROGUESQ_GFX_API\n");
        } else if (std::string_view(api) == "d3d12") {
            uc.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;

            fprintf(stderr, "[RT64] graphics API forced to D3D12 via ROGUESQ_GFX_API\n");
        } else if ((std::string_view(api) == "d3d11") || (std::string_view(api) == "d3d10"))
        {
            uc.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D11;
            rs64::video::set_d3d10(std::string_view(api) == "d3d10");

            fprintf(stderr, "[RT64] graphics API forced to %s via ROGUESQ_GFX_API\n", rs64::video::d3d10() ? "D3D10" : "D3D11");
        }
    }

#ifdef _WIN32

    // D3D10 is the D3D11 backend on a device capped at feature level 10; an explicit PLUME_D3D11_FEATURE_LEVEL wins.
    if ((uc.graphicsAPI == RT64::UserConfiguration::GraphicsAPI::D3D11) && rs64::video::d3d10() && (env_str("PLUME_D3D11_FEATURE_LEVEL") == nullptr))
    {
        _putenv_s("PLUME_D3D11_FEATURE_LEVEL", "10_0");
    }

#endif
}
} // namespace rs64::video
