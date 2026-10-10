#include <cstdio>
#include <fstream>

#include "video_config.h"

using nlohmann::json;
using UC = RT64::UserConfiguration;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string uc_dump(const UC& uc) {
    json j = uc;                 // RT64 to_json
    return j.dump();
}

static void test_to_friendly_basic() {
    UC uc; uc.validate();
    json j = rs64::video::to_friendly(uc);
    CHECK(j.contains("schema"));
    CHECK(j.value("schema", 0) == 2);
    CHECK(j.contains("advanced"));
}

static void test_to_friendly_mapping() {
    UC uc; uc.validate();
    uc.aspectRatio = UC::AspectRatio::Expand;
    uc.resolution = UC::Resolution::Manual; uc.resolutionMultiplier = 2.0;
    uc.antialiasing = UC::Antialiasing::MSAA4X;
    uc.filtering = UC::Filtering::Nearest;
    uc.threePointFiltering = true;
    uc.refreshRate = UC::RefreshRate::Manual; uc.refreshRateTarget = 60;
    uc.internalColorFormat = UC::InternalColorFormat::High;
    uc.displayBuffering = UC::DisplayBuffering::Double;
    uc.graphicsAPI = UC::GraphicsAPI::Vulkan;

    json j = rs64::video::to_friendly(uc);
    CHECK(j.value("widescreen", false) == true);
    CHECK(j["resolutionScale"].is_number() && j["resolutionScale"].get<double>() == 2.0);
    CHECK(j.value("antialiasing", -1) == 4);
    CHECK(j.value("smoothUpscale", true) == false);
    CHECK(j.value("smoothTextures", true) == false);   // threePoint true -> smoothTextures false
    CHECK(j.value("frameInterpolation", false) == true);
    CHECK(j.value("targetFramerate", 0) == 60);
    CHECK(j.value("hdr", false) == true);
    CHECK(j.value("tripleBuffering", true) == false);
    CHECK(j.value("graphicsAPI", std::string()) == "vulkan");
}

static void test_to_friendly_thirdstate_to_advanced() {
    UC uc; uc.validate();
    uc.filtering = UC::Filtering::Linear;                 // no friendly bool for Linear
    uc.aspectRatio = UC::AspectRatio::Manual; uc.aspectTarget = 2.35;
    json j = rs64::video::to_friendly(uc);
    CHECK(!j.contains("smoothUpscale"));
    CHECK(j["advanced"].value("filtering", std::string()) == "Linear");
    CHECK(!j.contains("widescreen"));
    CHECK(j["advanced"].value("aspectRatio", std::string()) == "Manual");
}

static void test_apply_basic() {
    UC uc; uc.validate();
    json j = {
        {"schema", 2},
        {"widescreen", true},
        {"resolutionScale", "auto"},
        {"antialiasing", 4},
        {"smoothTextures", false},
        {"frameInterpolation", false},
        {"graphicsAPI", "d3d12"},
    };
    rs64::video::apply_friendly(uc, j);
    CHECK(uc.aspectRatio == UC::AspectRatio::Expand);
    CHECK(uc.resolution == UC::Resolution::WindowIntegerScale);
    CHECK(uc.antialiasing == UC::Antialiasing::MSAA4X);
    CHECK(uc.threePointFiltering == true);               // smoothTextures false -> threePoint true
    CHECK(uc.refreshRate == UC::RefreshRate::Original);
    CHECK(uc.graphicsAPI == UC::GraphicsAPI::D3D12);
}

static void test_apply_resolution_number() {
    UC uc; uc.validate();
    rs64::video::apply_friendly(uc, json{{"resolutionScale", 2.0}});
    CHECK(uc.resolution == UC::Resolution::Manual);
    CHECK(uc.resolutionMultiplier == 2.0);
    rs64::video::apply_friendly(uc, json{{"resolutionScale", 1}});
    CHECK(uc.resolution == UC::Resolution::Original);
}

static void test_advanced_wins() {
    UC uc; uc.validate();
    json j = {
        {"widescreen", true},                            // -> Expand
        {"advanced", {{"aspectRatio", "Manual"}, {"aspectTarget", 2.35}}},
    };
    rs64::video::apply_friendly(uc, j);
    CHECK(uc.aspectRatio == UC::AspectRatio::Manual);    // advanced overrode friendly
    CHECK(uc.aspectTarget == 2.35);
}

static void test_roundtrip() {
    // Each crafted UC must survive to_friendly -> apply_friendly unchanged,
    // starting from a fresh validated baseline (as the loader does).
    auto rt = [](UC src) {
        src.validate();
        json j = rs64::video::to_friendly(src);
        UC dst; dst.validate();
        rs64::video::apply_friendly(dst, j);
        CHECK(uc_dump(src) == uc_dump(dst));
    };
    { UC u; rt(u); }                                                   // defaults
    { UC u; u.aspectRatio = UC::AspectRatio::Expand; rt(u); }
    { UC u; u.resolution = UC::Resolution::Manual; u.resolutionMultiplier = 3.0; rt(u); }
    { UC u; u.resolution = UC::Resolution::WindowIntegerScale; rt(u); }
    { UC u; u.antialiasing = UC::Antialiasing::MSAA8X; rt(u); }
    { UC u; u.filtering = UC::Filtering::Linear; rt(u); }              // third state via advanced
    { UC u; u.aspectRatio = UC::AspectRatio::Manual; u.aspectTarget = 2.35; rt(u); }
    { UC u; u.internalColorFormat = UC::InternalColorFormat::Standard; rt(u); }
    { UC u; u.refreshRate = UC::RefreshRate::Display; rt(u); }
    { UC u; u.upscale2D = UC::Upscale2D::All; u.hardwareResolve = UC::HardwareResolve::Enabled; rt(u); }
}

static std::string write_tmp(const char* name, const std::string& content) {
    std::string path = std::string(name);
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

static void test_load_missing() {
    UC uc; uc.validate();
    auto r = rs64::video::load(uc, "does_not_exist_video.json");
    CHECK(r.loaded == false);
    CHECK(r.migrated == false);
}

static void test_load_friendly() {
    UC uc; uc.validate();
    std::string p = write_tmp("tmp_friendly_video.json",
        R"({"schema":2,"drawDistance":1.0,"widescreen":true,"resolutionScale":"auto"})");
    auto r = rs64::video::load(uc, p);
    CHECK(r.loaded == true);
    CHECK(r.migrated == false);
    CHECK(uc.aspectRatio == UC::AspectRatio::Expand);
    CHECK(uc.resolution == UC::Resolution::WindowIntegerScale);
    std::remove(p.c_str());
}

static void test_load_legacy_migrates() {
    UC uc; uc.validate();
    std::string p = write_tmp("tmp_legacy_video.json",
        R"({"aspectRatio":"Expand","resolution":"Original"})");
    auto r = rs64::video::load(uc, p);
    CHECK(r.loaded == true);
    CHECK(r.migrated == true);
    CHECK(uc.aspectRatio == UC::AspectRatio::Expand);
    std::remove(p.c_str());
}

static void test_keep_cutscene_default_and_roundtrip() {
    rs64::video::set_keep_cutscene_draw_distance(true);
    UC uc; uc.validate();
    json j = rs64::video::to_friendly(uc);
    CHECK(j.value("keepCutsceneDrawDistance", false) == true);
    json in = { {"schema", 2}, {"drawDistance", 1.5}, {"keepCutsceneDrawDistance", false} };
    rs64::video::apply_friendly(uc, in);
    CHECK(rs64::video::keep_cutscene_draw_distance() == false);
    json out = rs64::video::to_friendly(uc);
    CHECK(out.value("keepCutsceneDrawDistance", true) == false);
    rs64::video::set_keep_cutscene_draw_distance(true);
    rs64::video::set_draw_distance(1.0f);
}

static void test_keep_cutscene_absent_key_not_migrated() {
    rs64::video::set_keep_cutscene_draw_distance(true);
    UC uc; uc.validate();
    std::string p = write_tmp("tmp_nokey_video.json", R"({"schema":2,"drawDistance":1.3})");
    auto r = rs64::video::load(uc, p);
    CHECK(r.loaded == true);
    CHECK(r.migrated == false);
    CHECK(rs64::video::keep_cutscene_draw_distance() == true);
    std::remove(p.c_str());
    rs64::video::set_draw_distance(1.0f);
}

static void test_fullscreen_roundtrip_and_peek() {
    rs64::video::set_fullscreen(false);
    UC uc; uc.validate();
    CHECK(rs64::video::to_friendly(uc).value("fullscreen", true) == false);
    std::string p = write_tmp("tmp_fullscreen_video.json", R"({"schema":2,"drawDistance":1.0,"fullscreen":true})");
    CHECK(rs64::video::peek_fullscreen(p) == true);
    CHECK(rs64::video::fullscreen() == false);
    auto r = rs64::video::load(uc, p);
    CHECK(r.loaded == true);
    CHECK(rs64::video::fullscreen() == true);
    CHECK(rs64::video::to_friendly(uc).value("fullscreen", false) == true);
    std::remove(p.c_str());
    CHECK(rs64::video::peek_fullscreen(p) == false);
    rs64::video::set_fullscreen(false);

    // The env pin survives a config load and is never saved; a player toggle clears it.
    rs64::video::pin_fullscreen(true);
    p = write_tmp("tmp_fullscreen_video.json", R"({"schema":2,"drawDistance":1.0,"fullscreen":false})");
    rs64::video::load(uc, p);
    std::remove(p.c_str());
    CHECK(rs64::video::fullscreen() == true);
    CHECK(rs64::video::to_friendly(uc).value("fullscreen", true) == false);
    rs64::video::set_fullscreen(false);
    CHECK(rs64::video::fullscreen() == false);
}

static void test_effective_draw_distance() {
    using rs64::video::effective_draw_distance;
    CHECK(effective_draw_distance(2.0f, false, 15) == 2.0f);
    CHECK(effective_draw_distance(2.0f, true, 15) == 1.0f);
    CHECK(effective_draw_distance(2.0f, false, rs64::video::kGameViewLevel) == 1.0f);
    CHECK(effective_draw_distance(1.0f, false, 0) == 1.0f);
}

// rayTracing: the file round-trips the player's setting; an env var pins the live switch but never leaks into the file.
static void test_ray_tracing_settings() {
    using rs64lights::Feature;
    _putenv("ROGUESQ_RT_FOG_SHAFTS=1");
    UC uc; uc.validate();
    json j = rs64::video::to_friendly(uc);
    CHECK(j.contains("rayTracing"));
    CHECK(j["rayTracing"]["lights"] == false && j["rayTracing"]["shadows"] == false && j["rayTracing"]["softShadows"] == true);
    rs64::video::apply_friendly(uc, json{{"rayTracing", {{"lights", true}, {"shadows", true}, {"fogShafts", false}}}});
    CHECK(rs64lights::feature(Feature::Lights) && rs64lights::feature(Feature::Shadows));
    CHECK(rs64lights::feature(Feature::FogShafts));
    j = rs64::video::to_friendly(uc);
    CHECK(j["rayTracing"]["lights"] == true && j["rayTracing"]["fogShafts"] == false);
    rs64::video::set_rt_setting(Feature::Shadows, false);
    CHECK(!rs64lights::feature(Feature::Shadows) && (rs64::video::to_friendly(uc)["rayTracing"]["shadows"] == false));
    j = rs64::video::to_friendly(uc);
    CHECK(j["rayTracing"]["ambientOcclusion"] == false && j["rayTracing"]["globalIllumination"] == false && j["rayTracing"]["reflections"] == false);
    rs64::video::apply_friendly(uc, json{{"rayTracing", {{"ambientOcclusion", true}}}});
    CHECK(rs64lights::feature(Feature::AmbientOcclusion));
    _putenv("ROGUESQ_RT_FOG_SHAFTS=");
}

int main() {
    test_ray_tracing_settings();
    test_keep_cutscene_default_and_roundtrip();
    test_keep_cutscene_absent_key_not_migrated();
    test_fullscreen_roundtrip_and_peek();
    test_effective_draw_distance();
    test_to_friendly_basic();
    test_to_friendly_mapping();
    test_to_friendly_thirdstate_to_advanced();
    test_apply_basic();
    test_apply_resolution_number();
    test_advanced_wins();
    test_roundtrip();
    test_load_missing();
    test_load_friendly();
    test_load_legacy_migrates();
    if (g_fail == 0) std::fprintf(stderr, "ALL TESTS PASSED\n");
    return g_fail == 0 ? 0 : 1;
}
