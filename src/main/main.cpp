#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cinttypes>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

// ---------------------------------------------------------------------------
// Forward declarations from RecompiledFuncs
// ---------------------------------------------------------------------------
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// ---------------------------------------------------------------------------
// RSP microcode dispatch
// ---------------------------------------------------------------------------
extern RspUcodeFunc aspMain;

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return aspMain;
    default:
        fprintf(stderr, "[RSP] Unknown task type: %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Audio (SDL2)
// ---------------------------------------------------------------------------
static SDL_AudioDeviceID audio_device = 0;
static uint32_t audio_sample_rate = 48000;

static void set_frequency(uint32_t freq) {
    if (audio_device) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    audio_sample_rate = freq;

    SDL_AudioSpec desired{};
    desired.freq     = (int)freq;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = 1024;

    audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    if (!audio_device) {
        fprintf(stderr, "[Audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(audio_device, 0);
}

static void queue_samples(int16_t* samples, size_t num_bytes) {
    if (audio_device) {
        SDL_QueueAudio(audio_device, samples, (Uint32)num_bytes);
    }
}

static size_t get_frames_remaining() {
    if (!audio_device) return 0;
    Uint32 queued = SDL_GetQueuedAudioSize(audio_device);
    // queued is in bytes; 4 bytes per stereo frame (2 ch × 2 bytes)
    return queued / 4;
}

// ---------------------------------------------------------------------------
// Input (SDL2 gamepad — one controller)
// ---------------------------------------------------------------------------
static SDL_GameController* controller = nullptr;

static void poll_input() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            exit(EXIT_SUCCESS);
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            if (!controller) {
                controller = SDL_GameControllerOpen(e.cdevice.which);
            }
        }
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && controller) {
            if (SDL_GameControllerGetJoystick(controller) ==
                SDL_JoystickFromInstanceID(e.cdevice.which)) {
                SDL_GameControllerClose(controller);
                controller = nullptr;
            }
        }
    }
}

static bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0 || !controller) {
        *buttons = 0; *x = 0.0f; *y = 0.0f;
        return false;
    }

    uint16_t btn = 0;
    auto b = [&](uint16_t mask, SDL_GameControllerButton sdl) {
        if (SDL_GameControllerGetButton(controller, sdl)) btn |= mask;
    };

    // N64 button masks (from ultra64.h)
    b(A_BUTTON,     SDL_CONTROLLER_BUTTON_A);
    b(B_BUTTON,     SDL_CONTROLLER_BUTTON_X);
    b(Z_TRIG,       SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    b(START_BUTTON, SDL_CONTROLLER_BUTTON_START);
    b(U_JPAD,       SDL_CONTROLLER_BUTTON_DPAD_UP);
    b(D_JPAD,       SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    b(L_JPAD,       SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    b(R_JPAD,       SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    b(L_TRIG,       SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    b(R_TRIG,       SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    b(U_CBUTTONS,   SDL_CONTROLLER_BUTTON_Y);
    b(D_CBUTTONS,   SDL_CONTROLLER_BUTTON_B);
    b(L_CBUTTONS,   SDL_CONTROLLER_BUTTON_BACK);
    b(R_CBUTTONS,   SDL_CONTROLLER_BUTTON_GUIDE);

    int16_t ax = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ay = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    *x = ax / 32767.0f;
    *y = -(ay / 32767.0f); // N64 Y is inverted vs SDL
    *buttons = btn;
    return true;
}

static void set_rumble(int, bool) { /* TODO */ }

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0 && controller) {
        return { ultramodern::input::DeviceType::Gamepad };
    }
    return { ultramodern::input::DeviceType::None };
}

// ---------------------------------------------------------------------------
// Graphics (SDL2 window creation — rt64 takes over from here)
// ---------------------------------------------------------------------------
ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    return {};
}

// Defined in rt64_render_context.cpp
namespace recomp {
    std::unique_ptr<ultramodern::renderer::RendererContext>
    create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    SDL_Window* window = SDL_CreateWindow(
        "Star Wars: Rogue Squadron 64 Recompiled",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    return { window };
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t, void*) {}

// ---------------------------------------------------------------------------
// Thread naming
// ---------------------------------------------------------------------------
static std::string get_game_thread_name(const OSThread* t) {
    switch (t->id) {
    case 1:  return "[Game] IDLE";
    case 3:  return "[Game] MAIN";
    case 4:  return "[Game] GRAPH";
    case 5:  return "[Game] SCHED";
    case 10: return "[Game] AUDIOMGR";
    case 18: return "[Game] DMAMGR";
    default: return "[Game] " + std::to_string(t->id);
    }
}

// ---------------------------------------------------------------------------
// Game registration
// ---------------------------------------------------------------------------
// ROM hash: xxHash3-64 of rogue_squadron.z64 (USA v1.0, 16MB)
static constexpr uint64_t RS64_ROM_HASH = 0x6B66A44153594DEAULL;

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash             = RS64_ROM_HASH,
        .internal_name        = "ROGUE SQUADRON",
        .game_id              = u8"rs64.n64.us.1.0",
        .mod_game_id          = "rs64",
        .save_type            = recomp::SaveType::Eep4k,
        .is_enabled           = true,
        .entrypoint_address   = get_entrypoint_address(),
        .entrypoint           = recomp_entrypoint,
    },
};

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = recomp::create_render_context,
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx    = create_gfx,
        .create_window = create_window,
        .update_gfx    = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples      = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency      = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input               = poll_input,
        .get_input                = get_n64_input,
        .set_rumble               = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    ultramodern::events::callbacks_t event_callbacks{};

    ultramodern::error_handling::callbacks_t error_callbacks{
        .message_box = [](const char* msg) { fprintf(stderr, "[Error] %s\n", msg); },
    };

    ultramodern::threads::callbacks_t thread_callbacks{
        .get_game_thread_name = get_game_thread_name,
    };

    recomp::start(
        { 0, 1, 0 },   // version: 0.1.0
        {},             // project_version extras
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        event_callbacks,
        error_callbacks,
        thread_callbacks
    );

    return EXIT_SUCCESS;
}
