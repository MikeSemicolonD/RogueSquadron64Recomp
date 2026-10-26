#include <cstdio>
#include <cstdlib>
#include "os_compat.h"
#include <cstring>
#include <csignal>
#include <vector>
#include <unordered_map>
#include <cinttypes>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <exception>
#include <typeinfo>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/events.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "librecomp/mods.hpp"
#include "common/rt64_common.h"
#include "imgui/imgui.h"
#include "rhi/rt64_render_hooks.h"
#include "gui/rt64_file_dialog.h"
#include "audio/audio.h"
#include "main/cli.h"
#include "main/crash.h"
#include "main/debug_pollers.h"
#include "input/input_host.h"
#include "main/ui_panels.h"
#include "input/input_bindings.h"
#include "input/rumble.h"
#include "input/touch_input.h"
#include "input/touch_menu.h"
#include "input/touch_config.h"
#include "game/game_state.h"           // rs64_state_current_id
#include "util/debug_logs.h"
#include "host_api/host_api.h"
#include "main/main.h"                 // this file's own exports (fullscreen/quit hooks)
#include "game/upstream_compat.h"      // rs64_vi_driven
#include "game/hook_helpers.h"         // g_vi_tick, g_boot_pulse_start
#include "game/nav_sequencer.h"        // rs64_nav_consume, rs64_nav_tick

#include "video/rt64_render_context.h"  // recomp::create_render_context
#include "video/video_config.h"
#include "main/renderdoc_capture.h"
#include <mutex>

using recomp::dbg::env_on;
using recomp::dbg::env_int;
using namespace rs64::app;

// Android keeps SDL's main -> SDL_main rename: SDLActivity calls SDL_main in libmain.so.
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED
#endif

#ifdef _WIN32
#define RS64_NULL_DEVICE "NUL"
#else
#define RS64_NULL_DEVICE "/dev/null"
#endif

#ifdef __ANDROID__
#include "main/android_host.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#pragma comment(lib, "Dbghelp.lib")
#include <crtdbg.h>
#include "SDL.h"
#include "SDL_syswm.h"
#else
#include "SDL2/SDL.h"
// SDL_syswm.h is only needed for the Win32 HWND path in create_window(); on
// Linux it pulls X11 <Xlib.h>, whose None/Bool/Status macros collide with C++
// enum members (e.g. ultramodern::input::Pak::None), so it is not included here.
#ifdef __APPLE__
#include "SDL2/SDL_syswm.h"
#include "SDL2/SDL_metal.h"
#endif
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void rs64_register_overlays();
const char* const* rs64_register_patches();
void rs64_register_mod_events(const char* const* patch_events);
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

// Wrapper: captures rdram into the watchdog global on first call so the
// poll/diagnostic threads can attach. Forwards to the real entrypoint.
extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;
extern "C" void rs64_asset_overrides_init();
extern "C" void rs64_camera_shots_init();
extern "C" void rs64_mod_levels_init();
extern "C" void rs64_menu_config_init(void);   // menu_config.cpp
extern "C" void rs64_entrypoint_with_rdram_capture(uint8_t* rdram, recomp_context* ctx) {

    g_recomp_rdram_for_wp_raw = rdram;
    rs64_asset_overrides_init();
    rs64_camera_shots_init();
    rs64_mod_levels_init();
    recomp_entrypoint(rdram, ctx);
}

// The game's N64 "main" function — renamed to avoid clash with C main()
extern "C" void rs_main(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// (Periodic mqdiag watchdog removed — mqdiag_dump lived in the librecomp
//  fork and isn't in upstream. Hangs are rare enough now that on-demand
//  dump-game.ps1 is sufficient.)

// Captured RDRAM base (set by the entrypoint wrapper) so detached diagnostic
// watchdog threads can read game memory.
extern "C" { volatile uint8_t* volatile g_recomp_rdram_for_wp_raw = nullptr; }

// ---------------------------------------------------------------------------
// Graphics (SDL2 window creation — rt64 takes over from here)
// ---------------------------------------------------------------------------
ultramodern::gfx_callbacks_t::gfx_data_t create_gfx()
{

    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");

#ifdef __ANDROID__

    // Touches must not also arrive as mouse clicks (the mouse bindings would fire).
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    // Two threads pump SDL events (window thread + game input thread); block-on-pause releases only one of them on resume.
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");

    // SDL otherwise lists the accelerometer as a joystick, and the raw-joystick bindings turn phone tilt into stick input on every screen.
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");

#else

    // Touchscreen on desktop ("enabled" in roguesq_touch.json): compositors often deliver touches as a mouse (Raspberry Pi OS's labwc
    // mouseEmulation), so the pointer acts as a finger and drives no mouse bindings; real touches don't also become clicks.
    if (rs64::touch::shared_config().enabled) {
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
    }

#endif

#ifndef NDEBUG
    // Debug builds: don't steal focus from the editor when the window first shows.
    SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
#endif

#if defined(__linux__) && !defined(__ANDROID__)

    // The window class names rs64recomp.desktop, where Wayland (and X11 docks) take the icon from.
    const char* const wm_class = "rs64recomp";
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", wm_class, 0);
    setenv("SDL_VIDEO_X11_WMCLASS", wm_class, 0);

#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO | SDL_INIT_SENSOR) != 0)
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }

    // Force-feedback sticks only; missing haptic support just means no rumble on them.
    if (SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0)
    {
        fprintf(stderr, "[input] haptic init failed (force-feedback sticks won't rumble): %s\n", SDL_GetError());
    }

#ifdef __ANDROID__

    // Runs synchronously on SDL's Java thread as the app backgrounds, before Android destroys the surface.
    SDL_AddEventWatch([](void*, SDL_Event* ev) -> int {

        if (ev->type == SDL_APP_WILLENTERBACKGROUND) {
            rs64::audio::set_foreground(false);
            rs64_render_suspend_surface();
        } else if (ev->type == SDL_APP_DIDENTERFOREGROUND) {
            rs64::audio::set_foreground(true);
        }

        return 0;
    }, nullptr);

#endif

    return nullptr;
}

#if defined(__linux__) && !defined(__ANDROID__)
#include "favicon_ico.h"
#include "stb/stb_image.h"

// favicon.ico holds one PNG; its directory entry gives the PNG's size (byte 14) and offset (byte 18).
static void set_window_icon(SDL_Window* window)
{

    const uint8_t* ico = reinterpret_cast<const uint8_t*>(rs64_favicon_ico);
    uint32_t size = 0;
    uint32_t offset = 0;
    memcpy(&size, ico + 14, sizeof(size));
    memcpy(&offset, ico + 18, sizeof(offset));

    if (uint64_t(offset) + size > sizeof(rs64_favicon_ico))
    {
        return;
    }

    int w = 0;
    int h = 0;
    stbi_uc* pixels = stbi_load_from_memory(ico + offset, (int)size, &w, &h, nullptr, 4);

    if (!pixels)
    {
        return;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);

    if (surface) {
        SDL_SetWindowIcon(window, surface);
        SDL_FreeSurface(surface);
    }

    stbi_image_free(pixels);
}

#endif

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t)
{
    // On non-Windows RT64 renders through Vulkan and needs the window created
    // with SDL_WINDOW_VULKAN so SDL_Vulkan_CreateSurface can bind to it. On
    // Windows the backend is D3D12 via the HWND, so the flag is omitted.
    // ROGUESQ_HIDE_WINDOW=1: create the window hidden so headless runs are a true background
    // process (no visible window). The HWND/surface is still valid, so RT64 keeps rendering and
    // presenting and the VI-paced loop proceeds; only on-screen display + window screenshots are lost.
    const bool hide_window = env_on("ROGUESQ_HIDE_WINDOW");
    Uint32 window_flags = SDL_WINDOW_RESIZABLE | (hide_window ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);

    if (env_on("ROGUESQ_MAXIMIZED"))
        window_flags |= SDL_WINDOW_MAXIMIZED;

#if defined(__APPLE__)
    window_flags |= SDL_WINDOW_METAL;
#elif !defined(_WIN32)
    window_flags |= SDL_WINDOW_VULKAN;
#endif

#ifdef __ANDROID__
    // SDLActivity hides the system bars (immersive mode) only for a fullscreen window.
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#else

    if (!hide_window) {
        if (recomp::dbg::env_str("ROGUESQ_FULLSCREEN")) {
            rs64::video::pin_fullscreen(env_on("ROGUESQ_FULLSCREEN"));
        } else if (rs64::video::peek_fullscreen("roguesq_video.json")) {
            rs64::video::set_fullscreen(true);
        }

        if (rs64::video::fullscreen()) {
            g_fullscreen_applied = true;
            window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
    }

#endif

    if (const char* lang = recomp::dbg::env_str("ROGUESQ_LANGUAGE")) {
        rs64::video::pin_language(lang);
    }

    int window_w = 640, window_h = 480;

    if (const char* ws = recomp::dbg::env_str("ROGUESQ_WINDOW_SIZE")) {
        int w = 0, h = 0;

        if (sscanf(ws, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) {
            window_w = w;
            window_h = h;
        }
    }

    // ROGUESQ_WINDOW_POS=x,y, "left" or "right" (just left or right of the primary display's centre, 8 px apart), e.g. for a co-op pair.
    int window_x = SDL_WINDOWPOS_CENTERED, window_y = SDL_WINDOWPOS_CENTERED;

    if (const char* wp = recomp::dbg::env_str("ROGUESQ_WINDOW_POS"))
    {
        SDL_Rect usable{};
        int x = 0, y = 0;
        const bool left = strcmp(wp, "left") == 0;

        if ((left || strcmp(wp, "right") == 0) && SDL_GetDisplayUsableBounds(0, &usable) == 0)
        {
            window_x = left ? usable.x + usable.w / 2 - window_w - 4 : usable.x + usable.w / 2 + 4;
            window_y = usable.y + (usable.h - window_h) / 2;
        } else if (sscanf(wp, "%d,%d", &x, &y) == 2) {
            window_x = x;
            window_y = y;
        }
    }

    if (env_on("ROGUESQ_UNFOCUSED")) {
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
    }

    auto create = [&] { return SDL_CreateWindow("Star Wars: Rogue Squadron 64 Recompiled", window_x, window_y, window_w, window_h, window_flags); };

    SDL_Window* sdl_window = create();

#ifdef __ANDROID__

    // On a portrait phone's first launch the activity is still turning to landscape, so its surface isn't there yet.
    int tries = 0;

    for (; !sdl_window && tries < 50; ++tries) {
        SDL_Delay(100);
        sdl_window = create();
    }

    if (tries > 0) {
        fprintf(stderr, "[window] native window ready after %d retries (%s)\n", tries, sdl_window ? "ok" : SDL_GetError());
    }

#endif

    if (!sdl_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }

#if defined(__linux__) && !defined(__ANDROID__)
    set_window_icon(sdl_window);
#endif

    // SDL starts text input by default on desktop, which pops up an on-screen keyboard on touchscreens; the lobby turns it on while typing.
    SDL_StopTextInput();
    extern SDL_Window* g_sdl_window;
    g_sdl_window = sdl_window;

#if defined(_WIN32)

    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(sdl_window, &wm);

    return { wm.info.win.window };

#elif defined(__APPLE__)

    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(sdl_window, &wm);
    SDL_MetalView view = SDL_Metal_CreateView(sdl_window);

    return { wm.info.cocoa.window, SDL_Metal_GetLayer(view) };

#else
    return sdl_window;
#endif
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t)
{
    // Window messages only dispatch on the thread that owns the window, so events (and RT64's F1-F4 filter) must be pumped here.
    SDL_PumpEvents();

#ifdef __ANDROID__

    // Back in the foreground: move RT64 onto the new window once SDL has one (until then SDL may still report the old one).
    if (rs64::audio::foreground() && rs64_render_surface_suspended()) {
        rs64_render_resume_surface();
    }

#endif

    // Build the menu config on the main thread; doing it lazily from a game-thread menu hook races menu-audio init.
    static bool s_cfg = false;

    if (!s_cfg) {
        s_cfg = true;
        rs64_menu_config_init();
    }

    static bool s_rumble = false;

    if (!s_rumble) {
        s_rumble = true;
        std::thread(rumble_thread_main).detach();
    }

    // Capture the mouse for steering while focused, unless the F6 controls window, the F1 inspector or a clickable menu is up.
    // Drain the relative-motion accumulator on each transition so enabling doesn't jump.
    bool want = (SDL_GetKeyboardFocus() != nullptr)
             && !rs64::touch::shared_config().enabled
             && !g_show_controls.load()
             && !rs64_rt64_inspector_open()
             && !mouse_menu_context(touch_context());
    g_mouse_capture.store(want);
    static bool s_rel = false;

    if (want != s_rel) {
        SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
        SDL_GetRelativeMouseState(nullptr, nullptr);
        s_rel = want;
    }
}

// ---------------------------------------------------------------------------
// Per-VI frame-barrier signal (step 1 of the frame-pacing root fix)
// ---------------------------------------------------------------------------
// The game's per-frame gfx barrier — submitGfxFrame's osRecvMesg on
// D_8011A7E8 (0x8011A7E8) and D_8011A818 (0x8011A818) — was BLOCK→NOBLOCK
// patched to dodge a boot deadlock, which removed the loop's 60Hz pacing (see
// memory: frame-pacing root cause). This callback fires once per HOST VI
// retrace (60Hz, from the VI thread) and posts a token to those queues, so the
// barrier always has a frame token available at the true display cadence.
//
// While the recvs are still NOBLOCK this is purely additive and observable only
// as "no regression": extra tokens fill the size-1 queues then drop
// (requeue_if_blocked=false), and do_send treats an uninitialized queue as full
// and no-ops — so it's safe even before the game creates these queues. It
// primes the barriers so the NOBLOCK instruction patches can later be reverted
// (step 2) and the game loop will pace to the real VI instead of running free.
// Disable with ROGUESQ_VI_BARRIER_SIGNAL=0.
static void rs64_vi_callback()
{

    g_vi_tick = g_vi_tick + 1;

    if (rs64_vi_driven()) return;   // hardware protocol: no host-injected tokens (they double-signal size-1 queues)

    g_vi_tick = g_vi_tick - 1;
    static int s_on = -1;

    if (s_on < 0) s_on = env_on("ROGUESQ_VI_BARRIER_SIGNAL", true);

    // Tick for rs64_attrib_wait_vi (attribution loop paces to the real VI).
    g_vi_tick = g_vi_tick + 1;

    if (!s_on)
    {
        return;
    }

    ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x8011A7E8u, (OSMesg)0, false, false);
    ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x8011A818u, (OSMesg)0, false, false);

    // [FRAME-PACING] Host-signal viRetraceHandlerThread's VI queue (0x80114388) per VI. Its osViSetEvent
    // registration isn't delivering retrace messages during the cinematic (is_game_started gate / latch),
    // so the buffer-drain consumer sleeps -> cinematic video buffers never return to free -> the
    // bufferArbiterProducerScanWait drain-barrier hangs (boot "freeze" at iter ~903). Posting the
    // registered message (0x8011A4CC) every VI wakes it to drain buffers. ROGUESQ_VI_RETRACE_SIGNAL=0 to disable.
    static int s_vr = -1;

    if (s_vr < 0) s_vr = env_on("ROGUESQ_VI_RETRACE_SIGNAL", true);

    if (s_vr)
    {
        ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x80114388u, (OSMesg)0x8011A4CCu, false, false);
    }

    // [FRAME-PACING] Host-signal the frame-sync barrier queue (0x80128D10) per VI. The cinematic
    // producer (bufferArbiterProducerScanWait → awaitFrameSyncMesgBlock) BLOCKs here waiting for a
    // frame-sync token that signalFrameSyncMesgNonblock would post — but that signaller isn't
    // firing during the cinematic, so the producer hangs (watchdog: cinematicLoopBody freeze at
    // iter ~891). Posting a token each VI gives it a 60Hz frame-sync pulse so it proceeds (the
    // recv discards the message value). ROGUESQ_VI_FRAMESYNC_SIGNAL=0 to disable.
    static int s_fs = -1;

    if (s_fs < 0) s_fs = env_on("ROGUESQ_VI_FRAMESYNC_SIGNAL", true);

    if (s_fs)
    {
        ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x80128D10u, (OSMesg)0, false, false);
    }

    // [FRAME-PACING] Host-signal the video-buffer drain queue (0x80128CF0) per VI. Near the END of
    // the cinematic the producer blocks in waitOnVideoQueue (funcs_5.c:5421, osRecvMesg BLOCK on
    // 0x80128CF0) waiting for a video buffer to return to free; bufferArbiterProducerScanWait
    // loops there forever (watchdog: cinematicLoopBody freeze at iter ~1118). The retrace consumer
    // isn't posting the free-token during the cinematic, so pulse it each VI like the sibling
    // barriers above. ROGUESQ_VI_VIDEOQ_SIGNAL=0 to disable.
    // Default OFF: unproven and correlated with the mode-2 framebuffer-flood crash in testing
    // (poking the producer to not wait for buffers seems to let it submit garbage-CIMG DLs).
    // The op_b5 DL-walk bound (rt64_gbi_f3dfactor5.cpp) is what stabilized the cinematic; this
    // signal is opt-in for further frame-pacing experiments. ROGUESQ_VI_VIDEOQ_SIGNAL=1 to enable.
    static int s_vq = -1;

    if (s_vq < 0) s_vq = env_on("ROGUESQ_VI_VIDEOQ_SIGNAL");

    if (s_vq)
    {
        ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x80128CF0u, (OSMesg)0, false, false);
    }
}

// ---------------------------------------------------------------------------
// Thread naming
// ---------------------------------------------------------------------------
static std::string get_game_thread_name(const OSThread* t) {

    switch (t->id)
    {
    case 1:  return "[Game] IDLE";
    case 3:  return "[Game] MAIN";
    case 4:  return "[Game] EEPROM";  // entry func_8006F2CC: save writer (osEepromLongWrite), not GRAPH
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
static constexpr const char8_t* kGameId = u8"rs64.n64.us.1.0";
static constexpr const char* kAppName = "Rogue Squadron 64 Recompiled";
static constexpr const char* kRomName = "Star Wars: Rogue Squadron (USA v1.0)";

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash             = RS64_ROM_HASH,
        .internal_name        = "ROGUE SQUADRON",
        .game_id              = kGameId,
        .mod_game_id          = "rs64",
        .save_type            = recomp::SaveType::Eep4k,
        .is_enabled           = true,
        .entrypoint_address   = get_entrypoint_address(),
        .entrypoint           = rs64_entrypoint_with_rdram_capture,
    },
};

static std::string rom_problem_text(recomp::RomValidationError r)
{

    if (r == recomp::RomValidationError::NotARom)
    {
        return "That file is not an N64 ROM.\n\n";
    }

    if (r == recomp::RomValidationError::IncorrectRom || r == recomp::RomValidationError::IncorrectVersion)
    {
        return std::string("That ROM is not ") + kRomName + ".\n\n";
    }

    return "That file could not be read.\n\n";
}

// Choose / Quit dialog: 1 = choose, 0 = quit, -1 = the dialog could not be shown.
static int rom_dialog(Uint32 flags, const char* title, const std::string& text, const char* choose_label)
{

    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, choose_label },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
    };

    const SDL_MessageBoxData box{ flags, nullptr, title, text.c_str(), 2, buttons, nullptr };
    int choice = 0;

    if (SDL_ShowMessageBox(&box, &choice) != 0)
    {
        return -1;
    }

    return choice;
}

int main(int argc, char* argv[])
{

#ifdef __ANDROID__

    // No console, shell environment, or exe directory on Android: log to logcat, read env from a file, run from the app's files dir.
    rs64::android::start_logcat_pump();
    rs64::android::load_env_file();
    std::filesystem::current_path(rs64::android::data_dir());
    rs64::android::install_bundled_mods();

#endif

    if (int rc = rs64::cli::apply_args(argc, argv); rc >= 0)
    {
        return rc;
    }

    fprintf(stderr, "[main] Rogue Squadron 64 Recompiled %s\n", RS64_VERSION_STRING);

    if (const char* comboList = recomp::dbg::env_str("ROGUESQ_PRECOMPILE_D3D11"))
    {
        const char* cacheDir = recomp::dbg::env_str("PLUME_D3D11_SHADER_CACHE");
        const std::string dir = (cacheDir != nullptr) ? cacheDir : "d3d11_shader_cache";
        rs64::crash::install_exception_filter();
        int failed = RT64::PrecompileD3D11Shaders(comboList, dir);

#ifdef _WIN32

        // Feature level 10 GPUs need their own shader model 4 set in the same folder (its keys don't collide).
        if ((failed == 0) && (recomp::dbg::env_str("PLUME_D3D11_FEATURE_LEVEL") == nullptr))
        {
            _putenv_s("PLUME_D3D11_FEATURE_LEVEL", "10_0");
            failed = RT64::PrecompileD3D11Shaders(comboList, dir);
        }

#endif

        return (failed == 0) ? 0 : 1;
    }

    rs64_renderdoc_init();

    // Input bindings: load roguesq_input.json next to the exe, or write the
    // defaults as an editable template if it's absent. ROGUESQ_INPUT_RESET=1
    // overwrites it with defaults.
    std::string cfg = rs64::input::default_config_path();
    const char* reset = recomp::os::getenv("ROGUESQ_INPUT_RESET");
    bool force = reset && reset[0] && reset[0] != '0';

    if (force || !rs64::input::load_bindings(g_bindings, cfg))
    {
        g_bindings = rs64::input::default_bindings();
        rs64::input::save_bindings(g_bindings, cfg);

        fprintf(stderr, "[input] wrote default bindings to %s\n", cfg.c_str());
    } else {
        fprintf(stderr, "[input] loaded bindings from %s\n", cfg.c_str());
    }

    g_report_rumble_pak.store(g_bindings.rumble.enabled);
    fflush(stderr);

    // Touch controls: roguesq_touch.json (on by default on Android); ROGUESQ_TOUCH_DEBUG draws the menu tap boxes.
    rs64::touch::Config tc = rs64::touch::shared_config();

    if (env_on("ROGUESQ_TOUCH_DEBUG")) {
        tc.debug_hitboxes = true;
    }

    g_touch.set_config(tc);

    g_touch.set_menu_hover([](float x, float y) {

        uint8_t* r = touch_rdram();

        if (!r || touch_context() != rs64::touch::Context::PauseMenu)
        {
            return -1;
        }

        float w, h;
        window_size(&w, &h);

        return rs64::touch::pause_hover(r, x, y, w, h);
    });

    g_touch.set_menu_tap([](float x, float y) {

        const rs64::touch::TapAction a = touch_menu_tap(x, y);

        if (g_touch.config().debug_hitboxes)
        {
            fprintf(stderr, "[touch] tap %.3f,%.3f screen=%d state=%s -> action %d\n", x, y, (int)touch_account_screen(), rs64_state_current_id(), (int)a);
        }

        return a;
    });

    RT64::SetRenderHookImgui(&draw_dev_ui);

#ifdef _WIN32

    // Log PE base so we can resolve absolute addresses from SEH logs to RVAs
    // (RVA = abs_addr - pe_base). Logged once on startup; helps with the
    // cinematic-loop AV diagnostics.
    HMODULE hSelf = GetModuleHandleW(NULL);

    fprintf(stderr, "[main] PE base=%p\n", (void*)hSelf);
    fflush(stderr);

#endif

    // RT64's RT64_LOG_PRINTF macro (debug builds) writes to GlobalLogFile via
    // unchecked fprintf + fflush. Pristine RT64 expects RT64::Application::start
    // to open it (we bypass Application), so without redirection the pointer
    // stays NULL and any RT64 debug-log call asserts in the CRT.
    //
    // Routing to stderr works but RT64 calls these macros at high rate (every
    // fullSync), and the per-call fflush starves the SDL message pump → game
    // window goes "Not Responding". Send writes to NUL instead — same effect
    // as the fork's `if(false) fprintf` cruft, but stays in our repo.
    // GlobalLogFile only exists in RT64 debug builds (under !NDEBUG); in Release
    // the RT64_LOG_* macros are no-ops, so the redirect is both unneeded and uncompilable.
#ifndef NDEBUG

    if (FILE *nul = recomp::os::fopen(RS64_NULL_DEVICE, "w")) {
        RT64::GlobalLogFile = nul;
    } else {
        RT64::GlobalLogFile = stderr;  // last-resort fallback
    }

#endif

    rs64::diag::start_state_poller();
    rs64::diag::start_phase_poller();

    rs64::crash::install_handlers();

    rs64_register_overlays();
    rs64_register_mod_events(rs64_register_patches());

    // Use the working directory as the config/data path (portable mode).
    recomp::register_config_path(std::filesystem::current_path());

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    // Check if the ROM is already stored; if not, try to import it from common filenames.
    recomp::check_all_stored_roms();
    std::u8string rs_game_id = supported_games[0].game_id;

    if (!recomp::is_rom_valid(rs_game_id))
    {

        static const char* rom_names[] = {
         "rogue_squadron", "rogue squadron",
         "rogue_squadron_64", "rogue squadron 64",
         "RogueSquadron", "roguesquadron",
         "RogueSquadron 64", "roguesquadron 64",
         "rs", "rs64"
        };

        static const char* rom_exts[] = { ".z64", ".v64", ".n64" };

        // The working directory, the game's own folder, and the folder above it (where a ROM ends up when the package unpacks into its own subfolder).
        std::vector<std::filesystem::path> rom_dirs{ std::filesystem::current_path() };

        if (char* base = SDL_GetBasePath())
        {
            const std::filesystem::path game_dir = std::filesystem::path(base).parent_path();
            SDL_free(base);
            rom_dirs.push_back(game_dir);
            rom_dirs.push_back(game_dir.parent_path());
        }

        bool imported = false;

        for (const std::filesystem::path& dir : rom_dirs) {
            for (const char* name : rom_names) {
                for (const char* ext : rom_exts)
                {
                    const std::filesystem::path rom = dir / (std::string(name) + ext);

                    if (!imported && (recomp::select_rom(rom, rs_game_id) == recomp::RomValidationError::Good))
                    {
                        fprintf(stderr, "[ROM] Imported %s\n", rom.string().c_str());
                        imported = true;
                    }
                }
            }
        }
    }

    // Re-check after any import attempt so is_rom_valid reflects the new file.
    recomp::check_all_stored_roms();

#ifdef __ANDROID__

    // No ROM yet: ask for one with the system file picker until a valid one is imported, or the player quits.
    std::string rom_problem;

    while (!recomp::is_rom_valid(rs_game_id))
    {
        const std::string picked = rs64::android::pick_rom();

        if (picked.empty())
        {
            const std::string text = rom_problem + kAppName + " needs your " + kRomName + " N64 ROM.";

            if (rom_dialog(SDL_MESSAGEBOX_INFORMATION, "ROM needed", text, "Choose ROM") != 1)
            {
                return 0;
            }

            rom_problem.clear();
            continue;
        }

        const recomp::RomValidationError r = recomp::select_rom(picked, rs_game_id);
        std::error_code ec;
        std::filesystem::remove(picked, ec);

        fprintf(stderr, "[ROM] picked file: validation %d\n", (int)r);

        if (r == recomp::RomValidationError::Good) {
            recomp::check_all_stored_roms();
            continue;
        }

        rom_problem = rom_problem_text(r);

        if (rom_dialog(SDL_MESSAGEBOX_WARNING, "Wrong file", rom_problem, "Choose again") != 1)
        {
            return 0;
        }

        rom_problem.clear();
    }

#else

    // No ROM found: say where it goes and offer a file picker, instead of starting to a silent black window.
    std::string rom_problem;

    while (!recomp::is_rom_valid(rs_game_id))
    {
        const std::string text = rom_problem + kAppName + " needs your " + kRomName + " N64 ROM.\n\n"
            "Place it next to the game as rogue_squadron.z64, or choose the file now.";
        const Uint32 flags = rom_problem.empty() ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_WARNING;
        const int choice = rom_dialog(flags, "ROM needed", text, "Choose ROM...");

        if (choice < 0)
        {
            fprintf(stderr, "[ROM] No ROM found. Place your Rogue Squadron (USA v1.0) ROM next to the game as rogue_squadron.z64.\n");

            return 1;
        }

        if (choice != 1)
        {
            return 0;
        }

        RT64::FileDialog::initialize();
        const std::filesystem::path picked = RT64::FileDialog::getOpenFilename({ RT64::FileFilter("N64 ROM", "z64,n64,v64") });
        RT64::FileDialog::finish();
        rom_problem.clear();

        if (picked.empty()) {
            continue;
        }

        const recomp::RomValidationError r = recomp::select_rom(picked, rs_game_id);

        fprintf(stderr, "[ROM] picked %s: validation %d\n", picked.string().c_str(), (int)r);

        if (r == recomp::RomValidationError::Good) {
            recomp::check_all_stored_roms();
            continue;
        }

        rom_problem = rom_problem_text(r);
    }

#endif

    recomp::start(recomp::Configuration{
        .project_version = { 0, 1, 0 },
        .window_handle = {},
        .rsp_callbacks = {
            .get_rsp_microcode = rs64::audio::get_rsp_microcode,
        },
        .renderer_callbacks = {
            .create_render_context = recomp::create_render_context,
        },
        .audio_callbacks = {
            .queue_samples        = rs64::audio::queue_samples,
            .get_frames_remaining = rs64::audio::get_frames_remaining,
            .set_frequency        = rs64::audio::set_frequency,
        },
        .input_callbacks = {
            .poll_input                = poll_input,
            .get_input                 = get_n64_input,
            .set_rumble                = set_rumble,
            .get_connected_device_info = get_connected_device_info,
        },
        .gfx_callbacks = {
            .create_gfx    = create_gfx,
            .create_window = create_window,
            .update_gfx    = update_gfx,
        },
        .events_callbacks = {
            .vi_callback = rs64_vi_callback,
            .gfx_init_callback = []() {
                std::u8string game_id = kGameId;

                if (recomp::is_rom_valid(game_id)) {
                    recomp::start_game(game_id);
                }
            },
        },
        .error_handling_callbacks = {
            // Also a dialog: Release has no console, so a stderr-only error (e.g. a mod conflict) left a silent black window.
            .message_box = [](const char* msg) {
                fprintf(stderr, "[Error] %s\n", msg);
                fflush(stderr);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kAppName, msg, nullptr);
            },
        },
        .threads_callbacks = {
            .get_game_thread_name = get_game_thread_name,
        },
        .message_queue_control = {},
    });

    return EXIT_SUCCESS;
}
