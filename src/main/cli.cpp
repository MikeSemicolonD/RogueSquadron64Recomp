#include "main/cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#endif

namespace rs64::cli {
static constexpr const char* kPlayLinkFlag = "--play-link";

#ifdef _WIN32

static std::filesystem::path exe_path()
{

    std::wstring path(32768, L'\0');

    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));

    return path;
}

static bool set_user_registry_text(const wchar_t* key, const wchar_t* name, const std::wstring& value)
{

    HKEY handle = nullptr;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
    {
        return false;
    }

    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG rc = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);

    RegCloseKey(handle);

    return rc == ERROR_SUCCESS;
}

// The Explorer leaves this file in the game folder, so a player who never uses it gets no registry entry. "ready" tells the Explorer the link works.
void register_play_link()
{

    static constexpr const wchar_t* kLinkKey = L"Software\\Classes\\rs64recomp";

    const std::filesystem::path exe = exe_path();
    const std::filesystem::path marker = exe.parent_path() / "explorer-link.txt";
    std::error_code ec;

    if (!std::filesystem::exists(marker, ec))
    {
        return;
    }

    const std::wstring open = L"\"" + exe.wstring() + L"\" --play-link \"%1\"";
    const std::wstring command = std::wstring(kLinkKey) + L"\\shell\\open\\command";

    const bool ok = set_user_registry_text(kLinkKey, nullptr, L"URL:Rogue Squadron 64 Recompiled")
        && set_user_registry_text(kLinkKey, L"URL Protocol", L"")
        && set_user_registry_text(command.c_str(), nullptr, open);

    if (ok)
    {
        std::ofstream(marker) << "ready";
    }

    fprintf(stderr, "[main] Explorer play link %s\n", ok ? "registered" : "could not be registered");
}

#else

void register_play_link() {}

#endif

// Set an env var so the getenv-based knobs below pick it up. The whole program
// is configured through the environment, not argv, so CLI args are translated here.
void set_env(const char* name, const char* value) {

#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// User-facing runtime flags. Each maps a --flag to a ROGUESQ_* env var that the
// getenv-based knobs elsewhere read. The full debug/trace catalog stays env-only
// (docs/engine/debug-trace-env-vars.md); reach any of those with --set NAME=VALUE.
struct CliFlag {
    const char* flag;   // long name without leading "--"
    const char* env;    // target ROGUESQ_* variable
    enum Kind { Bool, Value } kind;
    const char* on;     // Bool: value for --flag; Value: unused
    const char* off;    // Bool: value for --no-flag; Value: unused
    const char* help;
};

static const CliFlag kCliFlags[] = {
    {"gfx-api",          "ROGUESQ_GFX_API",          CliFlag::Value, "",  "",  "graphics API: vulkan | d3d12 | d3d11 | d3d10 (default auto)"},
    {"precompile-d3d11-shaders", "ROGUESQ_PRECOMPILE_D3D11", CliFlag::Value, "", "", "compile the D3D11 shaders for every combination in <list> into d3d11_shader_cache/ and exit (no ROM needed)"},
    {"hle-dev-mode",     "ROGUESQ_HLE_DEV_MODE",     CliFlag::Bool,  "1", "0", "RT64 ImGui inspector on F1 (default on in Debug)"},
    {"f5-native",        "ROGUESQ_F5_NATIVE",        CliFlag::Bool,  "1", "0", "emit F5 geometry (default on); --no- parses without emitting"},
    {"audio-ucode",      "ROGUESQ_NO_AUDIO_UCODE",   CliFlag::Bool,  "0", "1", "MusyX synth (default); --no-audio-ucode uses the silent stub"},
    {"audio-gain",       "ROGUESQ_AUDIO_GAIN",       CliFlag::Value, "",  "",  "master gain multiplier (e.g. 0.35; 0 = silent)"},
    {"audio-latency-ms", "ROGUESQ_AUDIO_LATENCY_MS", CliFlag::Value, "",  "",  "audio buffer latency in milliseconds"},
    {"dump-pcm",         "ROGUESQ_DUMP_PCM",         CliFlag::Value, "",  "",  "write synth output to a 22050 Hz stereo WAV at <path>"},
    {"fake-controller",  "ROGUESQ_FAKE_CONTROLLER",  CliFlag::Bool,  "1", "0", "fake a connected controller (headless runs)"},
    {"auto-start",       "ROGUESQ_AUTO_START",       CliFlag::Value, "",  "",  "pulse START after <ms> (headless runs)"},
    {"hide-window",      "ROGUESQ_HIDE_WINDOW",      CliFlag::Bool,  "1", "0", "create the window hidden (background process; no display/screenshots)"},
    {"maximized",        "ROGUESQ_MAXIMIZED",        CliFlag::Bool,  "1", "0", "start with the window maximized"},
    {"unfocused",        "ROGUESQ_UNFOCUSED",        CliFlag::Bool,  "1", "0", "open the window behind other windows without taking focus (test runs)"},
    {"fullscreen",       "ROGUESQ_FULLSCREEN",       CliFlag::Bool,  "1", "0", "start in borderless fullscreen (Alt+Enter toggles)"},
    {"language",         "ROGUESQ_LANGUAGE",         CliFlag::Value, "",  "",  "game text language for this run: en | de | fr or a language mod's code (the saved setting is unchanged)"},
    {"window-size",     "ROGUESQ_WINDOW_SIZE",      CliFlag::Value, "",  "",  "initial window client size WxH (e.g. 1280x720)"},
    {"widescreen",       "ROGUESQ_WIDESCREEN",       CliFlag::Bool,  "1", "0", "expand the aspect ratio to fill the window"},
    {"vr",               "ROGUESQ_VR",               CliFlag::Bool,  "1", "0", "play in a VR headset (OpenXR builds)"},
    {"draw-distance",    "ROGUESQ_DRAW_DIST",        CliFlag::Value, "",  "",  "draw distance multiplier (e.g. 1.3; terrain and fog capped at 2.5)"},
    {"force-level",      "ROGUESQ_FORCE_LEVEL",      CliFlag::Value, "",  "",  "missions you launch from the menus are level N (0-18)"},
    {"force-craft",      "ROGUESQ_FORCE_CRAFT",      CliFlag::Value, "",  "",  "missions you launch from the menus use craft N (0-8)"},

#ifdef RS64_MCP
    {"mcp-port",         "ROGUESQ_MCP_PORT",         CliFlag::Value, "",  "",  "dev: serve the RT64 MCP bridge on 127.0.0.1:<port> (tools/debug/mcp)"},
#endif

    {"boot-target",      "ROGUESQ_BOOT_TARGET",      CliFlag::Value, "",  "",  "skip the intro to a target: menu | menu:LABEL/LABEL... | demo:N | level:N[,craft] | abort:N | cutscene:N | lobby:host|join[,level]; N can be a mod level's id"},
    {"record",           "ROGUESQ_LS_RECORD",        CliFlag::Value, "",  "",  "record the next mission's input to <path.rec>, to replay it later"},
    {"replay",           "ROGUESQ_LS_REPLAY",        CliFlag::Value, "",  "",  "play a recording made with --record (start its level with --boot-target)"},
    {"keep-running",     "ROGUESQ_LS_KEEP_RUNNING",  CliFlag::Bool,  "1", "0", "stay on the last screen when a replay ends instead of quitting"},
};

static void print_cli_usage()
{

    fprintf(stderr, "Rogue Squadron 64 Recompiled %s\n\nOptions:\n", RS64_VERSION_STRING);
    fprintf(stderr, "  -m, --mute                 silence output (master gain 0)\n");

    for (const CliFlag& f : kCliFlags) {
        char name[64];

        if (f.kind == CliFlag::Bool)
            snprintf(name, sizeof(name), "--[no-]%s", f.flag);
        else
            snprintf(name, sizeof(name), "--%s <value>", f.flag);

        fprintf(stderr, "  %-26s %s\n", name, f.help);
    }

    fprintf(stderr, "  --set NAME=VALUE           set any ROGUESQ_* variable directly\n");
    fprintf(stderr, "  -h, --help                 show this help and exit\n");
    fprintf(stderr, "  -v, --version              print the version and exit\n");
    fprintf(stderr,
        "\nEvery option maps to a ROGUESQ_* environment variable; NAME=VALUE (bare) works too.\n"
        "Full debug/trace variable list: docs/engine/debug-trace-env-vars.md\n");
}

// Translate argv into the ROGUESQ_* env vars the rest of main reads. Returns an
// exit code to stop startup (0 for --help, 2 for a bad option), or -1 to continue.
int apply_args(int argc, char* argv[]) {

    // Nothing else may come with a link: a quote inside one would otherwise add options such as --set.
    if (argc >= 2 && !strcmp(argv[1], kPlayLinkFlag))
    {
        std::string target;

        if (argc != 3 || !play_link_target(argv[2], target))
        {
            fprintf(stderr, "error: bad play link\n");

            return 2;
        }

#ifdef _WIN32
        std::error_code ec;

        std::filesystem::current_path(exe_path().parent_path(), ec);
#endif

        if (!target.empty())
        {
            set_env("ROGUESQ_BOOT_TARGET", target.c_str());
        }

        return -1;
    }

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_cli_usage();

            return 0;
        }

        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("Rogue Squadron 64 Recompiled %s\n", RS64_VERSION_STRING);

            return 0;
        }

        if (!strcmp(a, "--mute") || !strcmp(a, "-m")) {
            set_env("ROGUESQ_AUDIO_GAIN", "0");
            continue;
        }

        if (!strcmp(a, "--set")) {
            const char* kv = (i + 1 < argc) ? argv[++i] : nullptr;
            const char* eq = kv ? strchr(kv, '=') : nullptr;

            if (!eq) {
                fprintf(stderr, "error: --set expects NAME=VALUE\n");

                return 2;
            }

            std::string name(kv, eq - kv);
            set_env(name.c_str(), eq + 1);
            continue;
        }

        // Long flag: --flag, --flag=value, or --no-flag.
        if (a[0] == '-' && a[1] == '-')
        {
            const char* body = a + 2;
            const char* eq = strchr(body, '=');
            std::string name = eq ? std::string(body, eq - body) : std::string(body);
            bool negated = false;

            if (name.rfind("no-", 0) == 0) { negated = true; name.erase(0, 3); }

            const CliFlag* found = nullptr;

            for (const CliFlag& f : kCliFlags) {
                if (name == f.flag) { found = &f; break; }
            }

            if (!found) {
                fprintf(stderr, "error: unknown option '%s' (try --help)\n", a);

                return 2;
            }

            if (found->kind == CliFlag::Bool)
            {
                set_env(found->env, negated ? found->off : found->on);
            } else {
                if (negated) {
                    fprintf(stderr, "error: '--no-%s' is not a toggle\n", found->flag);

                    return 2;
                }

                const char* val = eq ? eq + 1 : ((i + 1 < argc) ? argv[++i] : nullptr);

                if (!val) {
                    fprintf(stderr, "error: --%s expects a value\n", found->flag);

                    return 2;
                }

                set_env(found->env, val);
            }

            continue;
        }

        // Bare NAME=VALUE passthrough (scripts / any env var).
        if (const char* eq = strchr(a, '=')) {
            std::string name(a, eq - a);
            set_env(name.c_str(), eq + 1);
            continue;
        }

        fprintf(stderr, "error: unexpected argument '%s' (try --help)\n", a);

        return 2;
    }

    return -1;
}
}
