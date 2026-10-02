#include "nav_sequencer.h"
#include "game_state.h"        // rs64_state_current_id
#include "touch_menu.h"
#include "debug_logs.h"
#include "lockstep_core.h"
#include "host_api.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>

// Multiplayer's lobby state, published as a flag (0 idle, 1 hosting, 2 joining, 3 connected, 4 failed); 0 without multiplayer.
static int lobby_state() { return rs64::host::flag("mp_lobby_state"); }

// N64 button masks (subset; mirror of main.cpp).
#define N64_A_BUTTON     0x8000
#define N64_B_BUTTON     0x4000
#define N64_START_BUTTON 0x1000
#define N64_D_JPAD       0x0400
#define N64_U_JPAD       0x0800
#define N64_L_JPAD       0x0200
#define N64_R_JPAD       0x0100

// ---- Level-held input injection (read every poll by get_n64_input) ----
// While driving, the sequencer owns controller 0: the held button (or neutral) is returned on every poll.

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

RsNavTarget rs64_nav_parse(const char* v) {
    RsNavTarget t{ NAV_NONE, -1, -1 };
    if (!v) return t;
    auto arg = [](const char* s) { const char* c = std::strchr(s, ':'); return (c && c[1]) ? std::atoi(c + 1) : -1; };
    auto arg2 = [](const char* s) { const char* c = std::strchr(s, ','); return (c && c[1]) ? std::atoi(c + 1) : -1; };
    if (!std::strncmp(v, "level", 5))         { t.kind = NAV_LEVEL;    t.a = arg(v); t.b = arg2(v); }
    else if (!std::strncmp(v, "abort", 5))    { t.kind = NAV_ABORT;    t.a = arg(v); t.b = arg2(v); }
    else if (!std::strncmp(v, "cutscene", 8)) { t.kind = NAV_CUTSCENE; t.a = arg(v); t.b = arg2(v) < 0 ? 0 : arg2(v); }
    else if (!std::strncmp(v, "demo", 4))     { t.kind = NAV_DEMO;     t.a = arg(v) < 0 ? 0 : arg(v); }
    else if (!std::strncmp(v, "lobby:page", 10)) {
        // Test: stop on the multiplayer page with nothing pressed, handing the pad back.
        t.kind = NAV_LOBBY;
        t.b = 2;
        t.a = 0;
    }
    else if (!std::strncmp(v, "lobby:host", 10) || !std::strncmp(v, "lobby:join", 10)) {
        t.kind = NAV_LOBBY;
        t.b = v[6] == 'h' ? 0 : 1;
        t.a = arg2(v) < 0 ? 0 : arg2(v);
        const char* c1 = std::strchr(v, ',');
        const char* c2 = c1 ? std::strchr(c1 + 1, ',') : nullptr;
        t.c = (c2 && c2[1]) ? std::atoi(c2 + 1) : -1;
    }
    return t;
}

static RsNavTarget g_target{ NAV_NONE, -1, -1 };

extern "C" void rs64_nav_set_target(const char* boot_target) {
    g_target = rs64_nav_parse(boot_target);
}

// ---- Step machine ----

// A step waits for `wait_state`, then runs an action and advances; on frame-budget overflow the sequence aborts (logged) instead of hanging.
static int s_step = 0;
static int s_overlay = -1;
static int s_watchdog = 0;
static int s_since_press = 9999;
static bool s_disabled = false;
static const char* s_last_state = "";

// Presents per step before bailing (~20s at 60fps); ROGUESQ_NAV_STEP_BUDGET raises it for slow builds (Linux Debug, fast-clock replays).
static int step_budget() {
    static const int s = recomp::dbg::env_int("ROGUESQ_NAV_STEP_BUDGET", 1200);
    return s;
}

// Press a button for several presents (a press the menu will register), then release by simply
// not injecting on later ticks. Called across successive ticks via a pulse counter.
static int s_pulse = 0;
static uint16_t s_pulse_btn = 0;
static void begin_pulse(uint16_t btn) {
    s_pulse = 8; s_pulse_btn = btn;
    g_driving.store(true, std::memory_order_release);
}
static bool run_pulse() {
    if (s_pulse <= 0) { rs64_nav_inject(0, 0, 0); return false; }
    rs64_nav_inject(s_pulse_btn, 0, 0);
    --s_pulse;
    // Still holding: skip step logic this tick.
    return true;
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

// Level target: saves do not persist headless, so the flow always passes ENTER NAME (letter A + START),
// then writes the real level/craft on their screens and lets the game confirm.
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
    // A new pilot auto-launches a forced level, so there is no select screen; pin level+craft only after name entry (step >= 2), since writing currentLevel earlier freezes the flow.
    if (s_step >= 2) {
        // gGameSettings currentLevel (u8), gCurrentLevel (u32)
        nav_write_u8(rdram, 0x130B40, (uint8_t)g_target.a);
        nav_write_u32(rdram, 0x130B70, (uint32_t)g_target.a);
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
                // gGameSettings currentLevel (u8), gCurrentLevel (u32)
                nav_write_u8(rdram, 0x130B40, (uint8_t)g_target.a);
                nav_write_u32(rdram, 0x130B70, (uint32_t)g_target.a);
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

// Abort target: pause, pick ABORT MISSION (cursor written like touch taps, HUD+0xD68, then A), then YES and wait for the front end.
// YES is the first confirm row unless ROGUESQ_NAV_ABORT_YES says otherwise; landing back on the root means that row was NO, so the next is tried.
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
        // root: ABORT MISSION -> confirm submenu
        case 12: {
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
        // MISSION FAILED results -> one A -> SELECT LEVEL; a second A would launch the level again
        case 15: {
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

// Lobby target: MAIN MENU -> MULTIPLAYER -> pilot select -> the MULTIPLAYER page -> HOST GAME or JOIN GAME (the join address comes from roguesq_net.json) -> CONNECTED -> mission select, level, craft, mission.
// Entries are found by label in gCurrentMenuData (+0x00 title, +0x08 entries[8], +0x94 cursor, +0x95 count) and picked by writing the cursor, as touch taps do.
static const int LOBBY_FIRST_STEP = 30;

static std::string nav_str(uint8_t* rdram, uint32_t vaddr) {
    std::string s;
    // Mod labels live in the recomp heap above 0x81000000, inside the 512 MB RDRAM.
    if (vaddr < 0x80000000u || vaddr >= 0xA0000000u - 64u) return s;
    for (uint32_t i = 0; i < 40; ++i) {
        const char c = (char)rdram[((vaddr - 0x80000000u) + i) ^ 3];
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

static uint32_t nav_word(uint8_t* rdram, uint32_t phys) {
    return ((uint32_t)rdram[(phys + 0) ^ 3] << 24) | ((uint32_t)rdram[(phys + 1) ^ 3] << 16) | ((uint32_t)rdram[(phys + 2) ^ 3] << 8) | rdram[(phys + 3) ^ 3];
}

static int nav_find_entry(uint8_t* rdram, const char* label) {
    const int n = rdram[(0xCE730u + 0x95) ^ 3];
    for (int i = 0; i < n && i < 8; ++i)
        if (nav_str(rdram, nav_word(rdram, 0xCE730u + 0x08 + i * 4)) == label) return i;
    return -1;
}

static bool nav_pick(uint8_t* rdram, const char* label) {
    const int i = nav_find_entry(rdram, label);
    if (i < 0) return false;
    nav_write_u8(rdram, 0xCE730u + 0x94, (uint8_t)i);
    return true;
}

static void nav_lobby(uint8_t* rdram, const char* st) {
    const std::string title = nav_str(rdram, nav_word(rdram, 0xCE730u));
    switch (s_step) {
        case 30:  // main menu: MULTIPLAYER (like START, it opens the pilot select); the title screen before it (no entries) takes START
            if (rdram[(0xCE730u + 0x04) ^ 3] == 1) { nav_advance(); return; }
            if (is(st, "menu.main") && nav_pick(rdram, "MULTIPLAYER")) nav_repress(N64_A_BUTTON);
            else if (is(st, "menu") && rdram[(0xCE730u + 0x95) ^ 3] == 0) nav_repress(N64_START_BUTTON);
            break;
        case 31:  // pilot select: confirm the pilot in front (the runner copies a save); the lobby page opens instead of mission select
            if (title == "MULTIPLAYER") { nav_advance(); return; }
            if (rdram[(0xCE730u + 0x04) ^ 3] == 1) nav_repress(N64_A_BUTTON);
            break;
        // the page: HOST GAME or JOIN GAME, re-pressed until the lobby starts (a press during the page's swipe-in is dropped)
        case 32: {
            if (g_target.b == 2) {
                if (title == "MULTIPLAYER") {
                    fprintf(stderr, "[nav] lobby: page reached\n");
                    s_disabled = true;
                }
                break;
            }
            const char* entry = g_target.b == 0 ? "HOST GAME" : "JOIN GAME";
            if (lobby_state() != 0) {
                fprintf(stderr, "[nav] lobby: %s\n", entry);
                nav_advance();
                return;
            }
            if (title == "MULTIPLAYER" && nav_pick(rdram, entry)) nav_repress(N64_A_BUTTON);
            break;
        }
        // wait for the handshake; the page then leaves for mission select by itself
        case 33: {
            const int state = lobby_state();
            if (state == 3) { fprintf(stderr, "[nav] lobby connected\n"); nav_advance(); return; }
            if (state == 4) {
                fprintf(stderr, "[nav] lobby failed\n");
                s_disabled = true;
            }
            s_watchdog = 0;
            break;
        }
        // menu-overlay screen (u16 0x800CFF50): 1 mission select -> write the level, A; 0 craft select -> A; until the mission starts
        case 34: {
            if (is(st, "mission")) {
                fprintf(stderr, "[nav] lobby: level %d reached\n", g_target.a);
                s_disabled = true;
                return;
            }
            if (is(st, "cinematic") || is(st, "unknown")) s_watchdog = 0;
            // Test: ROGUESQ_NAV_WATCH_CUTSCENES=1 sits through every cutscene (a late joiner), pressing nothing while one plays.
            static const bool s_watch = recomp::dbg::env_on("ROGUESQ_NAV_WATCH_CUTSCENES");
            if (s_watch && rs64_state_in_cinematic()) break;
            // Any cutscene, and the co-op hangar launch, takes START.
            if (rs64_state_in_cinematic() || (!s_watch && rs64::host::flag("hangar_launch"))) {
                nav_repress(N64_START_BUTTON);
                break;
            }
            // The menu overlay's screens (u16 0x800CFF50, stale once the overlay is gone): 1 mission select, 0 craft select.
            if (s_overlay == 1) {
                const uint16_t screen = (uint16_t)((rdram[0xCFF50u ^ 3] << 8) | rdram[0xCFF51u ^ 3]);
                if (screen == 1) {
                    s_watchdog = 0;
                    // The host walks the cursor (u8 0x800CDAC1; screen state 0x800CDAC4) to its level; the client's own follow code picks the host's.
                    if (g_target.b != 0) break;
                    const int step = rs64::ls::follow_step(rdram[0xCDAC1u ^ 3], rdram[0xCDAC4u ^ 3], g_target.a, 3, true);
                    const uint16_t btn = step == rs64::ls::kFollowLeft ? N64_L_JPAD : step == rs64::ls::kFollowRight ? N64_R_JPAD : step == rs64::ls::kFollowConfirm ? N64_A_BUTTON : 0;
                    if (btn) nav_repress(btn);
                } else if (screen == 0) {
                    // Craft select: step the bays (u8 0x800CD6E4 -> craft table 0x800CC3F8) to the requested craft, then confirm.
                    // A bay turn animates before the bay byte changes: after a press, wait for the change (or 240 presents) before judging again.
                    static int s_bay_seen = -1, s_bay_wait = 0;
                    const int bay = rdram[0xCD6E4u ^ 3];
                    const int here = rdram[(0xCC3F8u + bay) ^ 3];
                    if (bay != s_bay_seen) {
                        fprintf(stderr, "[nav] lobby: craft bay %d = craft %d (want %d)\n", bay, here, g_target.c);
                        s_bay_seen = bay;
                        s_bay_wait = 0;
                    }
                    if (s_bay_wait > 0) {
                        --s_bay_wait;
                        break;
                    }
                    if (g_target.c >= 0 && here != g_target.c) {
                        begin_pulse(N64_R_JPAD);
                        s_bay_wait = 240;
                    } else {
                        // START confirms the craft and then skips the hangar launch; a watching client confirms with A and skips nothing itself.
                        static const bool s_watch_launch = recomp::dbg::env_on("ROGUESQ_NAV_WATCH_CUTSCENES");
                        nav_repress(s_watch_launch ? N64_A_BUTTON : N64_START_BUTTON);
                    }
                }
                break;
            }
            // The mission's opening cutscene takes START (skipping is safe since the link-branch codegen fix); loading screens are waited out.
            if (is(st, "cinematic")) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    fprintf(stderr, "[nav] lobby: skipping the opening cutscene\n");
                }
                nav_repress(N64_START_BUTTON);
            }
            break;
        }
        default: break;
    }
}

// Cutscene target: mainGameLoop's boot intro call (cinematicLoopBody(0x13, 2, 0) at 0x8003E10C, after the attribution screens) plays cutscene:<level>[,<kind>] instead, once; it loads cuts/id<level>_<intro|extro|special> (kind 0/1/2), and only 3,2 and 19,2 exist for kind 2.
// The craft byte gGameSettings+1 is set from the per-level table 0x8009EC50 (+3), as the game's cutscene viewer loop does.
extern "C" void rs64_nav_boot_cutscene(uint8_t* rdram, uint32_t* level, uint32_t* kind) {
    static bool s_done = false;
    static const RsNavTarget t = rs64_nav_parse(recomp::dbg::env_str("ROGUESQ_BOOT_TARGET"));
    const bool mission = t.a >= 0 && t.a <= 15 && t.b >= 0 && t.b <= 1;
    const bool special = t.b == 2 && (t.a == 3 || t.a == 19);
    if (s_done || t.kind != NAV_CUTSCENE || !(mission || special)) return;
    s_done = true;
    if (t.a <= 15) nav_write_u8(rdram, 0x130B41, rdram[(0x9EC50u + (uint32_t)t.a * 4 + 3) ^ 3]);
    *level = (uint32_t)t.a;
    *kind = (uint32_t)t.b;
    fprintf(stderr, "[nav] boot cutscene level %d kind %d\n", t.a, t.b);
    fflush(stderr);
}

// Demo target: the custom menu is auto-disabled so the attract-idle path runs and the demo fires on the natural idle timeout.
static void nav_demo(uint8_t* rdram, const char* st) {
    // Forcing it instant needs the unlocated "last input frame"; the idle clock is 0x8013889C/0x8013A524 at ~35/s, threshold ~2380.
    if (is(st, "menu.main") || is(st, "menu")) {
        // demoId, then the wrap counter
        nav_write_u8(rdram, 0x130B54, (uint8_t)g_target.a);
        nav_write_u8(rdram, 0x130B55, 0);
    }
    if (is(st, "attract.demo") || is(st, "mission")) {
        fprintf(stderr, "[nav] demo %d triggered (state=%s)\n", g_target.a, st);
        fflush(stderr);
        s_disabled = true;
    }
}


// ROGUESQ_FORCE_LEVEL / ROGUESQ_FORCE_CRAFT: no input, the player drives; the fields are pinned from SELECT LEVEL, AVAILABLE CRAFT or the end of ENTER NAME (a new pilot auto-launches) until the mission starts.
static void force_tick(uint8_t* rdram) {
    static const int level = recomp::dbg::env_int("ROGUESQ_FORCE_LEVEL", -1);
    static const int craft = recomp::dbg::env_int("ROGUESQ_FORCE_CRAFT", -1);
    if ((level < 0 && craft < 0) || !rdram) return;
    static bool s_armed = false;
    static bool s_was_name = false;
    const char* st = rs64_state_current_id();
    const bool name = is(st, "menu.account.enter_name");
    const bool was_armed = s_armed;
    if (is(st, "mission")) s_armed = false;
    else if ((s_was_name && !name) || is(st, "menu.account.level_select") || is(st, "menu.account.craft_select")) s_armed = true;
    s_was_name = name;
    if (s_armed != was_armed) {
        fprintf(stderr, "[force] %s (state=%s) level=0x%02X craft=0x%02X\n", s_armed ? "pinning" : "released", st, rdram[0x130B40 ^ 3], rdram[0x130B41 ^ 3]);
        fflush(stderr);
    }
    if (!s_armed) return;
    if (level >= 0) {
        // gGameSettings currentLevel (u8), gCurrentLevel (u32)
        nav_write_u8(rdram, 0x130B40, (uint8_t)level);
        nav_write_u32(rdram, 0x130B70, (uint32_t)level);
    }
    if (craft >= 0) nav_write_u8(rdram, 0x130B41, (uint8_t)craft);
}

extern "C" void rs64_nav_tick(uint8_t* rdram, int overlay) {
    s_overlay = overlay;
    force_tick(rdram);
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

    static bool s_lobby_init = false;
    if (g_target.kind == NAV_LOBBY && !s_lobby_init) {
        s_lobby_init = true;
        s_step = LOBBY_FIRST_STEP;
    }

    if (++s_watchdog > step_budget()) {
        fprintf(stderr, "[nav] stuck step=%d state=%s -- aborting sequence\n", s_step, st);
        std::string entries;
        for (int i = 0; i < 8; ++i) entries += " [" + nav_str(rdram, nav_word(rdram, 0xCE730u + 0x08 + i * 4)) + "]";
        fprintf(stderr, "[nav] menu %u title [%s] count %u cursor %u:%s\n", rdram[(0xCE730u + 0x04) ^ 3], nav_str(rdram, nav_word(rdram, 0xCE730u)).c_str(), rdram[(0xCE730u + 0x95) ^ 3], rdram[(0xCE730u + 0x94) ^ 3], entries.c_str());
        fflush(stderr);
        s_disabled = true;
        return;
    }

    switch (g_target.kind) {
        case NAV_LEVEL:    nav_level(rdram, st); break;
        case NAV_CUTSCENE: s_disabled = true; break;
        case NAV_DEMO:     nav_demo(rdram, st); break;
        case NAV_ABORT:    nav_abort(rdram, st); break;
        case NAV_LOBBY:    nav_lobby(rdram, st); break;
        default: s_disabled = true; break;
    }
}
