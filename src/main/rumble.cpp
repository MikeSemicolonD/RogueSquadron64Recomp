#include "rumble.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "debug_logs.h"

namespace rs64::rumble {
namespace {

// Player 0 craft record (gPlayers + 4) and the game's rumble slots; see plans/2026-09-26-hotas-support-plan.md.
constexpr uint32_t CRAFT            = 0x80137DBC;
constexpr uint32_t CRAFT_HEALTH     = CRAFT + 0xC0;   // f32
constexpr uint32_t CRAFT_MAX_HEALTH = CRAFT + 0xC4;   // f32
constexpr uint32_t CRAFT_PAK_SLOT   = CRAFT + 0xBB;   // u8
constexpr uint32_t CRAFT_FLAGS      = CRAFT + 0x180;  // u16: 0x8 spiral, 0x10 ground impact, 0x20 dead
constexpr uint32_t CRAFT_EFFECT_ID  = CRAFT + 0x1C4;  // s32
constexpr uint32_t RUMBLE_SLOTS     = 0x80110520;     // stride 0x88; +0x6B effect active
constexpr uint32_t RUMBLE_SUSPENDED = 0x80110740;     // u8
constexpr uint32_t GATE_FLAGS       = 0x80130B50;     // u32, the game skips rumble when & 0x60
constexpr uint32_t GATE_WORD        = 0x8010CA20;     // u32, the game skips rumble when nonzero

constexpr int64_t WINDOW_US = 100000;

using clock = std::chrono::steady_clock;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count();
}

uint32_t rd_u32(const uint8_t* rd, uint32_t va) {
    uint32_t v;
    memcpy(&v, rd + (va & 0x7FFFFF), 4);
    return v;
}
uint16_t rd_u16(const uint8_t* rd, uint32_t va) {
    uint16_t v;
    memcpy(&v, rd + ((va & 0x7FFFFF) ^ 2), 2);
    return v;
}
uint8_t rd_u8(const uint8_t* rd, uint32_t va) {
    return rd[(va & 0x7FFFFF) ^ 3];
}
float rd_f32(const uint8_t* rd, uint32_t va) {
    uint32_t u = rd_u32(rd, va);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

struct Edge { int64_t t; bool on; };

std::mutex        g_mtx;
std::deque<Edge>  g_edges;
bool              g_state = false;
bool              g_base_state = false;   // motor state before the first retained edge

// Fraction of the last WINDOW_US the motor was on.
float duty_locked(int64_t now) {
    while (!g_edges.empty() && g_edges.front().t < now - 2 * WINDOW_US) {
        g_base_state = g_edges.front().on;
        g_edges.pop_front();
    }
    const int64_t t0 = now - WINDOW_US;
    bool state = g_base_state;
    int64_t t = t0, on = 0;
    for (const Edge& e : g_edges) {
        if (e.t <= t0) {
            state = e.on;
            continue;
        }
        if (state) on += e.t - t;
        t = e.t;
        state = e.on;
    }
    if (state) on += now - t;
    return std::clamp((float)on / (float)WINDOW_US, 0.0f, 1.0f);
}

bool effect_enabled(const rs64::input::RumbleConfig& c, int id) {
    if (id == 0) return c.hit;
    if (id >= 1 && id <= 3) return c.collision;
    if (id >= 4 && id <= 6) return c.object_collision;
    if (id == 7) return c.terrain_scrape;
    if (id >= 8 && id <= 10) return c.weapons;
    if (id == 11) return c.death_spiral;
    if (id == 12) return c.crash;
    return true;
}

// Impacts and the crash lean on the low-frequency motor; hits, scrapes, and weapons on the high one.
bool heavy_effect(int id) {
    return (id >= 1 && id <= 6) || id == 11 || id == 12;
}

} // namespace

void motor(int channel, bool on) {
    static bool s_first = true;
    if (s_first && recomp::dbg::log_rumble()) {
        s_first = false;
        fprintf(stderr, "[rumble] first motor call from the game (channel %d, %s)\n", channel, on ? "on" : "off");
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    if (on == g_state) return;
    g_state = on;
    g_edges.push_back({ now_us(), on });
    if (g_edges.size() > 512) {
        g_base_state = g_edges.front().on;
        g_edges.pop_front();
    }
}

CraftStatus craft_status(const uint8_t* rd) {
    CraftStatus s;
    if (!rd) return s;
    const float h = rd_f32(rd, CRAFT_HEALTH);
    const float hmax = rd_f32(rd, CRAFT_MAX_HEALTH);
    if (!std::isfinite(h) || !std::isfinite(hmax) || hmax <= 0.0f) return s;
    const uint16_t flags = rd_u16(rd, CRAFT_FLAGS);
    s.valid = true;
    s.health = std::clamp(h / hmax, 0.0f, 1.0f);
    s.dead = (flags & 0x20) || ((flags & 0x10) && h <= 0.0f);
    s.spiral = (flags & 0x8) && h <= 0.0f && !s.dead;
    return s;
}

Output tick(const rs64::input::RumbleConfig& cfg, const uint8_t* rd, bool in_mission) {
    static float   s_last_health = -1.0f;
    static float   s_hit_scale = 1.0f;
    static int64_t s_hit_t = 0;
    static int     s_last_id = -2;
    static bool    s_last_active = false;
    static bool    s_last_sustain = false;
    static bool    s_last_out = false;

    const int64_t now = now_us();
    float duty;
    { std::lock_guard<std::mutex> lk(g_mtx);
      duty = duty_locked(now); }

    int id = -1;
    bool active = false, gated = false, sustain = false;
    if (rd && in_mission) {
        const uint8_t ch = rd_u8(rd, CRAFT_PAK_SLOT) & 3;
        active = rd_u8(rd, RUMBLE_SLOTS + ch * 0x88u + 0x6B) != 0;
        id = (int32_t)rd_u32(rd, CRAFT_EFFECT_ID);
        gated = (rd_u32(rd, GATE_FLAGS) & 0x60) || rd_u32(rd, GATE_WORD) != 0 || rd_u8(rd, RUMBLE_SUSPENDED) != 0;

        const float h = rd_f32(rd, CRAFT_HEALTH);
        const float hmax = rd_f32(rd, CRAFT_MAX_HEALTH);
        if (s_last_health >= 0.0f && h < s_last_health - 1e-3f && hmax > 0.0f) {
            const float frac = (s_last_health - h) / hmax;
            // A typical laser hit is ~7% of max health (10/150): ~0.8. 10%+ is full strength.
            s_hit_scale = std::clamp(0.4f + frac * 6.0f, 0.4f, 1.0f);
            s_hit_t = now;
            if (recomp::dbg::log_rumble()) {
                fprintf(stderr, "[rumble] health %.2f -> %.2f / %.2f (drop %.1f%%) hit scale %.2f\n", s_last_health, h, hmax, frac * 100.0f, s_hit_scale);
            }
        }
        s_last_health = std::isfinite(h) ? h : -1.0f;

        const uint16_t flags = rd_u16(rd, CRAFT_FLAGS);
        sustain = (flags & 0x8) && h <= 0.0f && !(flags & 0x30) && !gated;

        static int64_t s_state_log_t = 0;
        if (recomp::dbg::log_rumble() && now - s_state_log_t > 2000000) {
            s_state_log_t = now;
            fprintf(stderr, "[rumble] state health=%.2f/%.2f flags=0x%04X effect=%d active=%d gated=%d duty=%.2f speed=%.3f target=%.3f\n",
                    h, hmax, flags, id, (int)active, (int)gated, duty, rd_f32(rd, CRAFT + 0xE8), rd_f32(rd, CRAFT + 0xEC));
        }
    } else {
        s_last_health = -1.0f;
    }

    if (recomp::dbg::log_rumble() && (id != s_last_id || active != s_last_active)) {
        fprintf(stderr, "[rumble] effect id=%d active=%d duty=%.2f\n", id, (int)active, duty);
    }
    s_last_id = id;
    s_last_active = active;

    Output out;
    if (!cfg.enabled) return out;

    float s = duty;
    if (active && !effect_enabled(cfg, id)) s = 0.0f;
    if (cfg.scale_hits_by_damage && active && id == 0 && now - s_hit_t < 1000000) s *= s_hit_scale;
    const bool heavy = active && heavy_effect(id);
    out.lo = s * (heavy ? 1.0f : 0.6f);
    out.hi = s * (heavy ? 0.5f : 1.0f);

    // The game's spiral effect (11) is a one-shot at the start; hold a low pulse until ground impact.
    const bool do_sustain = sustain && cfg.sustain_death_spiral && cfg.death_spiral;
    if (do_sustain) {
        const float p = 0.3f + 0.15f * std::sin((float)(now % 1000000) * 1e-6f * 2.0f * 3.14159265f * 2.5f);
        out.lo = std::max(out.lo, p);
        out.hi = std::max(out.hi, p * 0.3f);
    }
    if (recomp::dbg::log_rumble() && do_sustain != s_last_sustain) {
        fprintf(stderr, "[rumble] death-spiral sustain %s\n", do_sustain ? "start" : "stop");
    }
    s_last_sustain = do_sustain;

    // Up to 2x: the game's PWM duty often lands at 0.1-0.3, which is faint on modern motors. Output still caps at 1.
    const float k = std::clamp(cfg.strength, 0.0f, 2.0f);
    out.lo = std::clamp(out.lo * k, 0.0f, 1.0f);
    out.hi = std::clamp(out.hi * k, 0.0f, 1.0f);

    const bool any = out.lo > 0.0f || out.hi > 0.0f;
    if (recomp::dbg::log_rumble() && any != s_last_out) {
        fprintf(stderr, "[rumble] output %s lo=%.2f hi=%.2f (duty=%.2f id=%d)\n", any ? "on" : "off", out.lo, out.hi, duty, id);
    }
    s_last_out = any;
    return out;
}

} // namespace rs64::rumble
