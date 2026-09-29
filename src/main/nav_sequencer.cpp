#include "nav_sequencer.h"
#include "game_state.h"        // rs64_state_current_id
#include "touch_menu.h"
#include "debug_logs.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>

// N64 button masks (subset; mirror of main.cpp).
#define N64_A_BUTTON     0x8000
#define N64_START_BUTTON 0x1000
#define N64_D_JPAD       0x0400
#define N64_U_JPAD       0x0800

// ---- Level-held input injection (read every poll by get_n64_input) ----
// While the sequencer is driving, it owns controller 0 every frame: the held button (or neutral
// between pulses) is returned on every get_n64_input call, so a press is a clean sustained hold.

static std::atomic<bool> g_driving{false};
static std::atomic<uint32_t> g_hold_btn{0};
static std::atomic<int> g_hold_x{0};
static std::atomic<int> g_hold_y{0};

extern "C" void rs64_nav_inject(uint16_t buttons, float x, float y) {
    g_hold_btn.store(buttons, std::memory_order_relaxed);
    g_hold_x.store((int)(x * 127.0f), std::memory_order_relaxed);
    g_hold_y.store((int)(y * 127.0f), std::memory_order_relaxed);
}

extern "C" bool rs64_nav_consume(uint16_t* buttons, float* x, float* y) {
    if (!g_driving.load(std::memory_order_acquire)) return false;
    *buttons = (uint16_t)g_hold_btn.load(std::memory_order_relaxed);
    *x = g_hold_x.load(std::memory_order_relaxed) / 127.0f;
    *y = g_hold_y.load(std::memory_order_relaxed) / 127.0f;
    return true;
}

// ---- Target ----

extern "C" RsNavTarget rs64_nav_parse(const char* v) {
    RsNavTarget t{ NAV_NONE, -1, -1 };
    if (!v) return t;
    auto arg = [](const char* s) { const char* c = std::strchr(s, ':'); return (c && c[1]) ? std::atoi(c + 1) : -1; };
    auto arg2 = [](const char* s) { const char* c = std::strchr(s, ','); return (c && c[1]) ? std::atoi(c + 1) : -1; };
    if (!std::strncmp(v, "level", 5))         { t.kind = NAV_LEVEL;    t.a = arg(v); t.b = arg2(v); }
    else if (!std::strncmp(v, "abort", 5))    { t.kind = NAV_ABORT;    t.a = arg(v); t.b = arg2(v); }
    else if (!std::strncmp(v, "cutscene", 8)) { t.kind = NAV_CUTSCENE; t.a = arg(v); }
    else if (!std::strncmp(v, "demo", 4))     { t.kind = NAV_DEMO;     t.a = arg(v) < 0 ? 0 : arg(v); }
    return t;
}

static RsNavTarget g_target{ NAV_NONE, -1, -1 };

extern "C" void rs64_nav_set_target(const char* boot_target) {
    g_target = rs64_nav_parse(boot_target);
}

// ---- Step machine ----

// A step: wait until the game reaches `wait_state`, then run an action and advance. Actions are
// filled per target in Task 3/5/6. Each step has a frame budget; on overflow the sequence aborts
// (logged) rather than hanging the boot.
static int s_step = 0;
static int s_overlay = -1;
static int s_watchdog = 0;
static int s_since_press = 9999;
static bool s_disabled = false;
static const char* s_last_state = "";

static const int STEP_BUDGET = 1200;   // presents (~20s at 60fps) per step before bail

// Press a button for several presents (a press the menu will register), then release by simply
// not injecting on later ticks. Called across successive ticks via a pulse counter.
static int s_pulse = 0;
static uint16_t s_pulse_btn = 0;
static void begin_pulse(uint16_t btn) {
    s_pulse = 8; s_pulse_btn = btn;
    g_driving.store(true, std::memory_order_release);  // take over controller 0 once we act
}
static bool run_pulse() {
    if (s_pulse <= 0) { rs64_nav_inject(0, 0, 0); return false; }
    rs64_nav_inject(s_pulse_btn, 0, 0);   // hold the button this frame
    --s_pulse;
    return true;   // still holding; skip step logic this tick
}

static void nav_advance() { s_step++; s_watchdog = 0; s_since_press = 9999; }

static void nav_write_u8(uint8_t* rdram, uint32_t phys, uint8_t val) { rdram[phys ^ 3] = val; }

static void nav_write_u32(uint8_t* rdram, uint32_t phys, uint32_t val) {
    rdram[(phys + 0) ^ 3] = (uint8_t)(val >> 24);
    rdram[(phys + 1) ^ 3] = (uint8_t)(val >> 16);
    rdram[(phys + 2) ^ 3] = (uint8_t)(val >> 8);
    rdram[(phys + 3) ^ 3] = (uint8_t)(val);
}

// Menus can ignore a press that lands before they accept input, so steps re-press on this interval.
static const int REPRESS_INTERVAL = 60;

static bool is(const char* st, const char* v) { return std::strcmp(st, v) == 0; }

// Press `btn` on the retry interval (used while parked waiting for a checkpoint).
static void nav_repress(uint16_t btn) {
    if (s_since_press >= REPRESS_INTERVAL) { begin_pulse(btn); s_since_press = 0; }
    else s_since_press++;
}

// Level target: drive the front-end to an actual in-mission state. Saves do not persist headless,
// so the flow always passes ENTER NAME; we pick a letter (A) + finish (START) rather than depend
// on the name buffer, then write the real level/craft on their screens and let the game confirm.
static int s_name_toggle = 0;
static const int ABORT_FIRST_STEP = 10;

// Mission reached: the level target stops here; the abort target goes on to its pause-menu steps.
static void nav_mission_reached() {
    if (g_target.kind == NAV_ABORT) {
        s_step = ABORT_FIRST_STEP - 1;
        nav_advance();
    } else {
        s_disabled = true;
    }
}

static void nav_level(uint8_t* rdram, const char* st) {
    // A brand-new pilot (saves don't persist headless) auto-launches a forced level -- there is no
    // level/craft select screen to drive. Pin the requested level+craft only AFTER name entry
    // (step >= 2); writing currentLevel during profile creation corrupts the flow and freezes.
    if (s_step >= 2) {
        nav_write_u8(rdram, 0x130B40, (uint8_t)g_target.a);   // gGameSettings currentLevel (u8)
        nav_write_u32(rdram, 0x130B70, (uint32_t)g_target.a); // gCurrentLevel (u32; indexed <<4)
        if (g_target.b >= 0) nav_write_u8(rdram, 0x130B41, (uint8_t)g_target.b);
    }

    // Success as soon as the mission launches (once past name entry).
    if (s_step >= 2 && is(st, "mission")) {
        fprintf(stderr, "[nav] level target=%d reached; currentLevel=0x%02X craft=0x%02X\n",
                g_target.a, rdram[0x130B40 ^ 3], rdram[0x130B41 ^ 3]);
        fflush(stderr);
        nav_mission_reached();
        return;
    }
    switch (s_step) {
        case 0:  // reach ENTER NAME: press A through main-menu -> select-game -> (new profile)
            if (is(st, "menu.account.enter_name")) { nav_advance(); return; }
            if (is(st, "menu.main") || is(st, "menu.account") || is(st, "menu")) nav_repress(N64_A_BUTTON);
            break;
        case 1:  // ENTER NAME: pick a letter (A) then finish (START); alternate so misses retry
            if (!is(st, "menu.account.enter_name")) { nav_advance(); return; }
            if (s_since_press >= REPRESS_INTERVAL) {
                begin_pulse((s_name_toggle++ & 1) ? N64_START_BUTTON : N64_A_BUTTON);
                s_since_press = 0;
            } else s_since_press++;
            break;
        case 2:  // after name entry the new pilot auto-launches: confirm any menu prompt
                 // (ARE-YOU-SURE) but do NOT press during the intro cinematic -- mashing skip
                 // there triggers the cutscene freeze. Just wait for the mission.
            if (is(st, "menu.account.level_select")) { nav_advance(); return; }
            if (!is(st, "cinematic")) nav_repress(N64_A_BUTTON);
            break;
        case 3:  // SELECT LEVEL: write target level, confirm, until AVAILABLE CRAFT
            if (is(st, "menu.account.level_select")) {
                nav_write_u8(rdram, 0x130B40, (uint8_t)g_target.a);   // gGameSettings currentLevel
                nav_write_u8(rdram, 0x130B70, (uint8_t)g_target.a);   // gCurrentLevel (low byte)
                nav_repress(N64_A_BUTTON);
            } else if (is(st, "menu.account.craft_select")) {
                nav_advance();
            }
            break;
        case 4:  // AVAILABLE CRAFT: write target craft, confirm, until mission
            if (is(st, "menu.account.craft_select")) {
                if (g_target.b >= 0) nav_write_u8(rdram, 0x130B41, (uint8_t)g_target.b);
                nav_repress(N64_A_BUTTON);
            } else if (is(st, "mission")) {
                nav_advance();
            }
            break;
        case 5:  // reached the mission
            fprintf(stderr, "[nav] level %d reached (state=%s)\n", g_target.a, st);
            fflush(stderr);
            nav_mission_reached();
            break;
        default: break;
    }
}

// Abort target, after the mission launches: pause, pick ABORT MISSION (the root record whose nextMenu is the confirm
// submenu 3), then YES, and wait for the front end. The cursor is written the way touch taps do (HUD+0xD68) and A
// confirms it. YES is the first confirm row unless ROGUESQ_NAV_ABORT_YES says otherwise; landing back on the root
// means that row was NO, so the next row is tried.
static int s_settle = 0;
static int s_yes = -1;
static int s_abort_wait = 0;
static void nav_log_pause(uint8_t* rdram, const char* what) {
    const int menu = rs64::touch::pause_submenu(rdram);
    fprintf(stderr, "[nav] abort %s: %s records%s\n", what, rs64::touch::describe_pause(rdram).c_str(),
            rs64::touch::describe_pause_records(rdram, menu < 0 ? 0 : menu).c_str());
    fflush(stderr);
}

static void nav_select_and_confirm(uint8_t* rdram, int k) {
    if (rs64::touch::pause_entry(rdram) != k) {
        rs64::touch::pause_set_entry(rdram, k);
        s_since_press = REPRESS_INTERVAL - 10;
        return;
    }
    nav_repress(N64_A_BUTTON);
}

static void nav_abort(uint8_t* rdram, const char* st) {
    if (s_step < ABORT_FIRST_STEP) {
        nav_level(rdram, st);
        return;
    }
    const bool paused = rs64::touch::read_paused(rdram);
    const int menu = rs64::touch::pause_submenu(rdram);
    switch (s_step) {
        case 10:  // let the mission start before pausing
            if (is(st, "mission") && !paused && ++s_settle >= 180) nav_advance();
            break;
        case 11:  // START until the pause root is open; no re-press while the HUD animates
            if (menu == 0) {
                nav_log_pause(rdram, "pause open");
                nav_advance();
            } else if (!paused) {
                nav_repress(N64_START_BUTTON);
            }
            break;
        case 12: {  // root: ABORT MISSION -> confirm submenu
            if (menu == 3) {
                nav_log_pause(rdram, "confirm open");
                if (s_yes < 0) s_yes = recomp::dbg::env_int("ROGUESQ_NAV_ABORT_YES", 0);
                nav_advance();
                break;
            }
            if (menu != 0) {
                if (!paused) s_step = 11;
                break;
            }
            const int k = rs64::touch::pause_find_entry(rdram, 0, 3);
            if (k < 0) {
                nav_log_pause(rdram, "no ABORT entry");
                s_disabled = true;
                break;
            }
            nav_select_and_confirm(rdram, k);
            break;
        }
        case 13:  // confirm: YES
            if (menu == 3) {
                nav_select_and_confirm(rdram, s_yes);
            } else if (menu == 0) {
                fprintf(stderr, "[nav] abort: confirm row %d was NO; trying %d\n", s_yes, s_yes + 1);
                fflush(stderr);
                s_yes++;
                s_step = 12;
            } else if (!paused) {
                fprintf(stderr, "[nav] abort confirmed (YES=%d, state=%s)\n", s_yes, st);
                fflush(stderr);
                nav_advance();
            }
            break;
        case 14:  // back to the front end (menu overlay; the classifier still says mission on the pilot screens, whose menu ptr is 0)
            if (s_overlay == 1 || !std::strncmp(st, "menu", 4)) {
                fprintf(stderr, "[nav] abort: results screen (overlay=%d state=%s)\n", s_overlay, st);
                fflush(stderr);
                s_settle = 0;
                nav_advance();
            } else if (++s_abort_wait > 600 && !paused) {
                nav_repress(N64_A_BUTTON);
            }
            break;
        case 15: {  // MISSION FAILED results -> one A -> SELECT LEVEL; a second A would launch the level again
            static uint32_t s_prev_key = 0xFFFFFFFFu;
            const uint16_t screen = (uint16_t)((rdram[0x0CFF50 ^ 3] << 8) | rdram[0x0CFF51 ^ 3]);
            const uint32_t key = ((uint32_t)s_overlay << 24) ^ ((uint32_t)rdram[0x0CE734 ^ 3] << 16) ^ screen;
            if (key != s_prev_key) {
                fprintf(stderr, "[nav] abort results: overlay=%d state=%s screen=%u menuPtr=0x%02X%02X%02X%02X menuId=%u\n", s_overlay, st, screen,
                        rdram[0x0CE730 ^ 3], rdram[0x0CE731 ^ 3], rdram[0x0CE732 ^ 3], rdram[0x0CE733 ^ 3], rdram[0x0CE734 ^ 3]);
                fflush(stderr);
                s_prev_key = key;
            }
            // Front-end screen u16: 3 results, 1 SELECT LEVEL (menu ptr is 0 here, so account_screen() can't be used).
            if (s_overlay == 1 && screen == 1) {
                fprintf(stderr, "[nav] abort complete: level %d -> SELECT LEVEL\n", g_target.a);
                fflush(stderr);
                s_disabled = true;
            } else if (s_overlay == 1 && screen == 3) {
                nav_repress(N64_A_BUTTON);
            }
            break;
        }
        default: break;
    }
}

// Demo target: with the custom menu disabled (auto for demo), the attract path works again.
// Fire it immediately by pinning demoId + setting the attract flag (bit 0x400 of gGameSettings+0x10
// = 0x130B50) once the front end is up, instead of waiting out the ~60s idle timeout.
static void nav_demo(uint8_t* rdram, const char* st) {
    // Pin which demo the attract plays. The custom menu is auto-disabled for the demo target
    // (menu_config) so the front-end attract-idle path runs; the demo then fires on the natural
    // idle timeout. (Forcing it instant needs the reset-on-input "last input frame", not yet
    // pinned -- the idle clock is 0x8013889C/0x8013A524 @ ~35/s, threshold ~2380.)
    if (is(st, "menu.main") || is(st, "menu")) {
        nav_write_u8(rdram, 0x130B54, (uint8_t)g_target.a);          // demoId
        nav_write_u8(rdram, 0x130B55, 0);                            // wrap counter
    }
    if (is(st, "attract.demo") || is(st, "mission")) {
        fprintf(stderr, "[nav] demo %d triggered (state=%s)\n", g_target.a, st);
        fflush(stderr);
        s_disabled = true;
    }
}


extern "C" void rs64_nav_tick(uint8_t* rdram, int overlay) {
    s_overlay = overlay;
    if (g_target.kind == NAV_NONE || !rdram) return;
    if (s_disabled) { g_driving.store(false, std::memory_order_release); return; }
    // Do NOT drive before the first action: the boot START pulse must advance the title ->
    // menu.main first. begin_pulse() flips g_driving on once the sequence starts acting.

    const char* st = rs64_state_current_id();
    if (std::strcmp(st, s_last_state) != 0) {
        fprintf(stderr, "[nav] step=%d state=%s\n", s_step, st);
        fflush(stderr);
        s_last_state = st;
    }

    // Finish any in-flight button pulse before evaluating the next step.
    if (run_pulse()) return;

    if (++s_watchdog > STEP_BUDGET) {
        fprintf(stderr, "[nav] stuck step=%d state=%s -- aborting sequence\n", s_step, st);
        fflush(stderr);
        s_disabled = true;
        return;
    }

    switch (g_target.kind) {
        case NAV_LEVEL:    nav_level(rdram, st); break;
        case NAV_CUTSCENE: /* Task 6 */ break;
        case NAV_DEMO:     nav_demo(rdram, st); break;
        case NAV_ABORT:    nav_abort(rdram, st); break;
        default: s_disabled = true; break;
    }
}
