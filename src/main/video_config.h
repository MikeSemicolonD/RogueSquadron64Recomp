#ifndef RS64_VIDEO_CONFIG_H
#define RS64_VIDEO_CONFIG_H

#include <string>

#include "common/rt64_user_configuration.h"
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
constexpr float kDrawDistanceMax = 2.5f;
float draw_distance();
void set_draw_distance(float mult);

// Seed uc with the baseline before calling. Reads `path`; schema>=2 -> friendly,
// otherwise legacy raw merge (migrated=true). Missing/unparseable -> loaded=false.
LoadResult load(RT64::UserConfiguration& uc, const std::string& path);

} // namespace rs64::video

#endif
