#pragma once
// Saving the game's settings files so a crash or a typo never costs the player the file.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace rs64::settings {

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
