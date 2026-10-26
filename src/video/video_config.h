#ifndef RS64_VIDEO_CONFIG_H
#define RS64_VIDEO_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "common/rt64_user_configuration.h"
#include "hle/rt64_rs64_lights.h"
#include "json/json.hpp"

namespace rs64::video {

struct LoadResult {
    bool loaded = false;    // a file was present and parsed
    bool migrated = false;  // the file was legacy raw and should be rewritten friendly
};

// Friendly JSON -> uc. Applies present friendly keys, then "advanced" raw
// overrides (which win), then uc.validate(). Absent keys leave uc untouched.
void apply_friendly(RT64::UserConfiguration& uc, const nlohmann::json& j);

// uc -> friendly JSON. Fields no friendly key can represent go into "advanced",
// so the round-trip is lossless against a matching baseline.
nlohmann::json to_friendly(const RT64::UserConfiguration& uc);

// Draw distance multiplier ("drawDistance" in roguesq_video.json), host-side and not part of RT64's config. Read live by the draw-distance hooks.
constexpr float kDrawDistanceMin = 1.0f;
constexpr float kDrawDistanceMax = 8.0f;
float draw_distance();
void set_draw_distance(float mult);

// The game's own terrain instead of RT64's whole-map pass ("legacyTerrain", default off; ROGUESQ_F5_STATIC_TERRAIN overrides). Its grid breaks up
// past kLegacyTerrainDrawDistanceMax, so turning it on clamps the saved draw distance and set_draw_distance clamps to draw_distance_max().
constexpr float kLegacyTerrainDrawDistanceMax = 1.3f;
bool legacy_terrain();
void set_legacy_terrain(bool on);
float draw_distance_max();

// Cutscenes keep the view distance the game sets per shot ("keepCutsceneDrawDistance", default on); ROGUESQ_CINE_DRAW_DIST overrides.
bool keep_cutscene_draw_distance();
void set_keep_cutscene_draw_distance(bool on);

// RT64 copies each rendered frame back to RDRAM ("framebufferWriteback", default on; off by default on a Raspberry Pi). This game never reads them
// back, and the copy plus the GPU wait it needs cost slow GPUs about half their frame time. Applies live.
bool framebuffer_writeback();
void set_framebuffer_writeback(bool on);

// "graphicsAPI": "d3d10" is D3D11 with the device capped at feature level 10 (the path D3D10-class GPUs get).
bool d3d10();
void set_d3d10(bool on);

// Fullscreen ("fullscreen" in roguesq_video.json, default off). Alt+Enter and the menu toggle change it and the change is saved.
bool fullscreen();
void set_fullscreen(bool on);
// ROGUESQ_FULLSCREEN: overrides the live state without touching the saved setting; the next set_fullscreen clears it.
void pin_fullscreen(bool on);
// Reads only the "fullscreen" key, so the window can be created fullscreen before the full config loads.
bool peek_fullscreen(const std::string& path);

// Game text language code ("language" in roguesq_video.json: a built-in "en" / "de" / "fr" or a language mod's code; "" = unset, the game's own choice). OPTIONS > LANGUAGE saves it.
std::string language();
void set_language(const std::string& code);
// ROGUESQ_LANGUAGE: overrides the live language without touching the saved setting; the next set_language clears it.
void pin_language(const std::string& code);

// Ray-tracing switches ("rayTracing" in roguesq_video.json: lights, shadows, softShadows, fogShafts; default off, soft on). The stored setting is the
// player's; setting it also flips the live RT64 switch unless a ROGUESQ_RT_* env var pins that switch.
bool rt_setting(rs64lights::Feature f);
void set_rt_setting(rs64lights::Feature f, bool on);

// The GPU supports ray queries (set once RT64 has a device); menus hide the ray-tracing page without it.
bool ray_query_supported();
void set_ray_query_supported(bool on);

// Menu-driven display settings. The menu reads and steps a desired copy of the RT64 fields it owns; the renderer applies it at present (apply_pending).
enum class Display { Resolution, Widescreen, AntiAliasing, FrameInterpolation, TargetFps, TextureFilter, UpscaleFilter };

// Current choice index; -1 = CUSTOM (an anti-aliasing combination matching no preset).
int display_value(Display d);
void step_display(Display d);
int display_choice_count(Display d);
// English choice text ("AUTO", "MSAA 4X", "3-POINT", "DISPLAY", "144", ...); "CUSTOM" for -1.
std::string display_label(Display d, int choice);
bool frame_interpolation_on();

// Choice <-> RT64 field mapping. Resolution: AUTO, 1X..4X. Anti-aliasing: OFF, FXAA, MSAA 2X/4X/8X, SSAA 2X/4X (-1 = CUSTOM). Upscale: NEAREST, LINEAR, AA PIXEL.
int aa_preset(const RT64::UserConfiguration& uc);
void apply_aa_preset(RT64::UserConfiguration& uc, int preset);
int resolution_choice(const RT64::UserConfiguration& uc);
void apply_resolution_choice(RT64::UserConfiguration& uc, int choice);
int upscale_choice(const RT64::UserConfiguration& uc);
void apply_upscale_choice(RT64::UserConfiguration& uc, int choice);

// Renderer side, once per present: copies pending menu changes into uc (and otherwise mirrors uc for the menu). msaa = call updateMultisampling; discard_fbs = updateUserConfig(true).
struct ApplyResult { bool changed = false; bool discard_fbs = false; bool msaa = false; };
ApplyResult apply_pending(RT64::UserConfiguration& uc);

// Refresh rates the window's display supports (ascending, distinct), for TARGET FPS.
void set_refresh_rates(std::vector<int> hz);

// Battle Above Taloraan pins its own far plane and fog and has no terrain, so the multiplier stays 1.0 there.
constexpr uint32_t kGameViewLevel = 10;
float effective_draw_distance(float setting, bool game_view, uint32_t level);

// Seed uc with the baseline before calling. Reads `path`; schema>=2 -> friendly,
// otherwise legacy raw merge (migrated=true). Missing/unparseable -> loaded=false.
LoadResult load(RT64::UserConfiguration& uc, const std::string& path);

} // namespace rs64::video

#endif
