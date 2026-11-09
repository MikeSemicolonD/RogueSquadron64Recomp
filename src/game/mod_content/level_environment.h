#pragma once
// A level's fog and clouds, from a mod's assets/level/<folder>/environment.json. No game access here.
#include "json/json.hpp"
#include <cstdint>
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

    return env;
}

}
