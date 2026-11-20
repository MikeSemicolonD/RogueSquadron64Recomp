#pragma once
// A level's fog and clouds, from a mod's assets/level/<folder>/environment.json, and its sun colour from the level's hmp. No game access here.
#include "json/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rs64env {

// Fog past the stock view at a longer draw distance. Auto: Thin when the fog starts under 0.85 of the far plane, else Scale.
enum class FogReach { Auto, Scale, Thin, Stock };

struct Color {
    bool set = false;
    uint8_t rgb[3] = { 0, 0, 0 };
};

struct Environment {
    // Fraction of the far plane where fog starts; <= 0 keeps the game's.
    float fogStart = -1.0f;
    FogReach fogReach = FogReach::Auto;
    Color fogColor;
    bool clouds = false;
    // World units per second.
    float windX = 0.0f, windZ = 0.0f;
    // World height the cloud layers stay around (game Y, down is positive); without it they keep their offset from the camera.
    bool hasAltitude = false;
    float altitude = 0.0f;
    Color cloudTint;
    // Flight area: how far in from the map's min X, max X, min Z and max Z edges the craft is turned back.
    bool hasBounds = false;
    float bounds[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

inline Color parseColor(const nlohmann::json& v, const char* name, std::vector<std::string>& errors)
{

    Color c;

    if (!v.is_array() || v.size() != 3)
    {
        errors.push_back(std::string(name) + " is not [r, g, b]");

        return c;
    }

    for (size_t i = 0; i < 3; i++)
    {
        if (!v[i].is_number_integer() || v[i].get<int>() < 0 || v[i].get<int>() > 255)
        {
            errors.push_back(std::string(name) + " values must be 0-255");

            return c;
        }

        c.rgb[i] = (uint8_t)v[i].get<int>();
    }

    c.set = true;

    return c;
}

inline Environment parseEnvironment(const std::string& text, std::vector<std::string>& errors)
{

    Environment env;
    const auto doc = nlohmann::json::parse(text, nullptr, false);

    if (doc.is_discarded() || !doc.is_object())
    {
        errors.push_back("not a JSON object");

        return env;
    }

    if (doc.contains("fog") && doc["fog"].is_object())
    {
        const auto& fog = doc["fog"];

        if (fog.contains("start") && fog["start"].is_number())
        {
            const float start = fog["start"].get<float>();

            if (start > 0.0f && start < 1.0f) env.fogStart = start;
            else errors.push_back("fog start must be between 0 and 1");
        }

        const std::string reach = fog.contains("reach") && fog["reach"].is_string() ? fog["reach"].get<std::string>() : "auto";

        if (reach == "scale") env.fogReach = FogReach::Scale;
        else if (reach == "thin") env.fogReach = FogReach::Thin;
        else if (reach == "stock") env.fogReach = FogReach::Stock;
        else if (reach != "auto") errors.push_back("unknown fog reach " + reach);

        if (fog.contains("color")) env.fogColor = parseColor(fog["color"], "fog color", errors);
    }

    if (doc.contains("clouds") && doc["clouds"].is_object())
    {
        const auto& clouds = doc["clouds"];

        env.clouds = true;

        if (clouds.contains("wind") && clouds["wind"].is_array() && clouds["wind"].size() == 2 && clouds["wind"][0].is_number() && clouds["wind"][1].is_number())
        {
            env.windX = clouds["wind"][0].get<float>();
            env.windZ = clouds["wind"][1].get<float>();
        }

        if (clouds.contains("altitude") && clouds["altitude"].is_number())
        {
            env.hasAltitude = true;
            env.altitude = clouds["altitude"].get<float>();
        }

        if (clouds.contains("tint")) env.cloudTint = parseColor(clouds["tint"], "cloud tint", errors);
    }

    if (doc.contains("bounds"))
    {
        const auto& b = doc["bounds"];
        const bool ok = b.is_array() && (b.size() == 4) && std::all_of(b.begin(), b.end(), [](const nlohmann::json& v) { return v.is_number() && (v.get<float>() >= 0.0f); });

        if (ok)
        {
            env.hasBounds = true;

            for (size_t i = 0; i < 4; i++) env.bounds[i] = b[i].get<float>();
        }
        else
        {
            errors.push_back("bounds is not [min X, max X, min Z, max Z] insets of 0 or more");
        }
    }

    return env;
}

// The mission sun's colour as load_level_hmp builds it: the lighting block (offset at +0x20) holds a count, then 0x20-byte light objects;
// the first type-0 (+0x18) one without the optional flag (+0x1B) is the sun, its float colour (+0xC) normalized.
inline bool hmpSunColor(const uint8_t* hmp, size_t size, float out[3])
{

    const auto be32 = [hmp](size_t o) { return ((uint32_t)hmp[o] << 24) | ((uint32_t)hmp[o + 1] << 16) | ((uint32_t)hmp[o + 2] << 8) | hmp[o + 3]; };

    if (size < 0x24)
    {
        return false;
    }

    const size_t light = be32(0x20);

    if (light + 0xC > size)
    {
        return false;
    }

    const uint32_t count = be32(light);

    for (uint32_t i = 0; i < count; i++)
    {
        const size_t o = light + i * 0x20u;

        if (o + 0x20 > size)
        {
            return false;
        }

        if (hmp[o + 0x18] != 0 || hmp[o + 0x1B] != 0)
        {
            continue;
        }

        float c[3];

        for (int k = 0; k < 3; k++)
        {
            const uint32_t bits = be32(o + 0xC + k * 4);

            std::memcpy(&c[k], &bits, 4);
        }

        const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);

        if (!(len > 0.0f))
        {
            return false;
        }

        for (int k = 0; k < 3; k++)
        {
            out[k] = c[k] / len;
        }

        return true;
    }

    return false;
}

}
