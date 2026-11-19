#pragma once
// Saving the game's settings files so a crash or a typo never costs the player the file.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include "json/json.hpp"

namespace rs64::settings {

// The value at key, or nullopt when it is missing or of the wrong type: a wrong type is logged and skipped so one bad entry never resets the file.
template <class T>
std::optional<T> get(const nlohmann::json& o, const char* key, const std::string& where = {})
{

    if (!o.is_object() || !o.contains(key) || o.at(key).is_null())
    {
        return std::nullopt;
    }

    // nlohmann reads true/false as 1/0 into a number; a bool in a number field is a mistake, not a value.
    if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    {
        if (o.at(key).is_boolean())
        {
            fprintf(stderr, "[settings] %s%signoring \"%s\": type must be number, but is boolean\n", where.c_str(), where.empty() ? "" : ": ", key);

            return std::nullopt;
        }
    }

    try {
        return o.at(key).get<T>();
    } catch (const std::exception& e) {
        const std::string what = e.what();
        const size_t at = what.find("] ");

        fprintf(stderr, "[settings] %s%signoring \"%s\": %s\n", where.c_str(), where.empty() ? "" : ": ", key, at == std::string::npos ? what.c_str() : what.c_str() + at + 2);

        return std::nullopt;
    }
}

template <class T>
T value_or(const nlohmann::json& o, const char* key, const T& fallback, const std::string& where = {})
{
    return get<T>(o, key, where).value_or(fallback);
}

// Visitors for a fields(c, f) list that names each key of a settings struct once: reader loads o into the fields, writer stores them into o.
inline auto reader(const nlohmann::json& o, const std::string& where = {})
{
    return [&o, where](const char* key, auto& v) { v = value_or(o, key, v, where); };
}

inline auto writer(nlohmann::json& o)
{
    return [&o](const char* key, const auto& v) { o[key] = v; };
}

// Parses a settings file; a syntax error is logged with where it is.
inline bool parse_file(std::istream& in, const std::string& path, nlohmann::json& out)
{

    try {
        in >> out;
    } catch (const std::exception& e) {
        fprintf(stderr, "[settings] %s: %s\n", path.c_str(), e.what());

        return false;
    }

    return true;
}

// Writes text to path through a temp file and a rename: a crash mid-save leaves the old file whole.
inline bool write_file(const std::string& path, const std::string& text)
{

    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);

    if (!out.is_open())
    {
        return false;
    }

    out << text;
    out.close();

    if (!out.good())
    {
        return false;
    }

    std::error_code ec;

    std::filesystem::rename(tmp, path, ec);

    return !ec;
}

// A settings file that could not be read is kept as <path>.bad before defaults replace it.
inline void keep_bad_file(const std::string& path)
{

    std::error_code ec;

    if (!std::filesystem::exists(path, ec))
    {
        return;
    }

    const std::string bad = path + ".bad";

    std::filesystem::rename(path, bad, ec);

    fprintf(stderr, "[settings] %s could not be read; %s\n", path.c_str(), ec ? "it will be replaced" : ("kept as " + bad).c_str());
}

}
