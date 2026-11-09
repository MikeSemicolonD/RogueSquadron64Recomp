#pragma once

#include <string>
#include <string_view>

#ifndef RS64_VERSION_STRING
#define RS64_VERSION_STRING "unknown"
#endif

// Command-line flags. Each one sets a ROGUESQ_* environment variable, which is how the rest of the program is configured.
namespace rs64::cli {
    // Returns an exit code to stop startup (0 for --help, 2 for a bad option), or -1 to continue.
    int apply_args(int argc, char* argv[]);

    void set_env(const char* name, const char* value);

    // Registers the rs64recomp: link for this exe, only if the RS64 Explorer has marked its folder.
    void register_play_link();

    // A web page writes this link, so only what the Explorer sends is accepted: rs64recomp:, rs64recomp:level:ID or rs64recomp:cutscene:ID,K.
    inline bool play_link_target(std::string_view url, std::string& target)
    {

        static constexpr std::string_view kScheme = "rs64recomp:";
        static constexpr std::string_view kLevel = "level:";
        static constexpr std::string_view kCutscene = "cutscene:";

        if (url.substr(0, kScheme.size()) != kScheme)
        {
            return false;
        }

        std::string_view rest = url.substr(kScheme.size());

        if (!rest.empty() && rest.back() == '/')
        {
            rest.remove_suffix(1);
        }

        size_t at = 0;

        const auto digit = [](char c) { return c >= '0' && c <= '9'; };
        const auto word = [&](char c) { return digit(c) || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };

        // A level is a number or a mod level's id, such as new_level_2.
        const auto span = [&](auto allowed, size_t longest) {

            const size_t start = at;

            while (at < rest.size() && allowed(rest[at]))
                ++at;

            return at > start && at - start <= longest;
        };

        bool valid = rest.empty();

        if (rest.substr(0, kLevel.size()) == kLevel)
        {
            at = kLevel.size();
            valid = span(word, 48);
        }
        else if (rest.substr(0, kCutscene.size()) == kCutscene)
        {
            at = kCutscene.size();
            valid = span(word, 48) && at < rest.size() && rest[at++] == ',' && span(digit, 2);
        }

        if (!valid || (!rest.empty() && at != rest.size()))
        {
            return false;
        }

        target = std::string(rest);

        return true;
    }
}
