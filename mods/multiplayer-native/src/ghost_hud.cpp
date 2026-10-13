// Ghost co-op HUD, radar dot and puppet pose hooks.
#include "ghost_internal.h"
#include "debug_logs.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace rs64::ghost;
using namespace rs64::ls;
using rs64::mips::rw;
using rs64::mips::ww;
using rs64::mips::rf;

// renderRadarMinimap after placeRadarDots (0x800C6290; s0 = radar, s2 = centre): the other player as a 2x2 purple dot (palette 15), plotted like placeRadarDots so the next restore removes it: world x/z -> 2*v*k - c (k 0x800A67CC, c 0x800A67D4) inside the +-0x800A67D0 window.
// 64x64 CI4 at radar+0, index ((px & 0x3F) >> 1) + ((pz & 0x3F) << 5); each dot backs up {u16 index, 4 bytes} at radar+0xC38 + 6n; count u16 radar+0xDB8 (64 max).
extern "C" void rs64_ghost_radar(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    if (!s.in_session || s.despawned || !s.puppet_shown) {
        return;
    }
    const uint32_t radar = (uint32_t)ctx->r16;
    const uint32_t centre = (uint32_t)ctx->r18;
    if (radar < 0x80000000u || radar >= 0x80800000u || centre < 0x80000000u || centre >= 0xA0000000u) {
        return;
    }
    const uint32_t n = rs64::mips::rh(rdram, radar + 0xDB8u);
    if (n >= 64) {
        return;
    }
    const float k = rf(rdram, 0x800A67CCu);
    const float half = rf(rdram, 0x800A67D0u);
    const float c = rf(rdram, 0x800A67D4u);
    const float cx = 2.0f * rf(rdram, centre) * k;
    const float cz = 2.0f * rf(rdram, centre + 8u) * k;
    const int px = (int)(2.0f * s.puppet_pos.x * k - c);
    const int pz = (int)(2.0f * s.puppet_pos.z * k - c);
    if (!((int)(cx - half) < px && px < (int)(cx + half) && (int)(cz - half) < pz && pz < (int)(cz + half))) {
        return;
    }
    const uint32_t idx = (uint32_t)(((px & 0x3F) >> 1) + ((pz & 0x3F) << 5));
    const uint32_t cells[4] = {idx, (idx + 1u) & 0x7FFu, (idx + 0x20u) & 0x7FFu, (idx + 0x21u) & 0x7FFu};
    const uint32_t backup = radar + 0xC38u + 6u * n;
    auto wh = [rdram](uint32_t a, uint16_t v) { *(uint16_t*)(rdram + ((a - 0x80000000u) ^ 2)) = v; };
    wh(backup, (uint16_t)idx);
    for (int i = 0; i < 4; ++i) {
        wb(rdram, backup + 2u + (uint32_t)i, rb(rdram, radar + cells[i]));
    }
    // Palette index 15 is never used by the radar (checked across a mission): purple in all three radar palettes (16 RGB entries of 3 bytes at 0x80109FF8 / 0x8010A028 / 0x8010A058; radar+0x844 points at the current one).
    // The rest are a grey ramp (the map) and pure red, green and blue (12-14), so a saturated purple stands apart from both.
    const uint8_t pal = 0xF;
    for (uint32_t pa : {0x80109FF8u, 0x8010A028u, 0x8010A058u}) {
        wb(rdram, pa + 3u * pal + 0u, 0xD0);
        wb(rdram, pa + 3u * pal + 1u, 0x00);
        wb(rdram, pa + 3u * pal + 2u, 0xFF);
    }
    auto put = [&](uint32_t cell, bool high) {
        const uint8_t v = rb(rdram, radar + cell);
        wb(rdram, radar + cell, high ? (uint8_t)((v & 0x0F) | (pal << 4)) : (uint8_t)((v & 0xF0) | pal));
    };
    if ((px & 1) == 0) {
        put(cells[0], true);
        put(cells[0], false);
        put(cells[2], true);
        put(cells[2], false);
    } else {
        put(cells[0], false);
        put(cells[1], true);
        put(cells[2], false);
        put(cells[3], true);
    }
    wh(radar + 0xDB8u, (uint16_t)(n + 1));
}

// initVoiceSubtitleSystem's fontAlloc (0x80055B08, a2 = slot count): in ghost co-op the mission font gets slots 1-2 for the co-op message line and the name label.
extern "C" void rs64_ghost_hud_fonts(recomp_context* ctx) {
    if (rs64_ghost_mode()) {
        ctx->r6 = 3;
    }
}

// Lays a mission-font slot out with text (its own RDRAM string buffer, allocated once) and shows or hides it.
static void hud_slot_text(uint8_t* rdram, recomp_context* ctx, uint32_t font, uint32_t slot, uint32_t* buf, const std::string& text) {
    if (!*buf) {
        const uint32_t a = rs64::mp::alloc(rdram, 40);
        if (!a) {
            return;
        }
        *buf = a;
    }
    const size_t n = std::min<size_t>(text.size(), 39);
    for (size_t i = 0; i <= n; ++i) {
        rdram[((*buf - 0x80000000u) + (uint32_t)i) ^ 3] = i < n ? (uint8_t)text[i] : 0;
    }
    rs64::mp::call(rdram, ctx, 0x80063CFCu, {font, *buf, slot, (uint32_t)-0x100}, {0u});
    rs64::mp::call(rdram, ctx, 0x80061C74u, {font, slot, text.empty() ? 0u : 0xFFu});
}

// Links a mission-font slot into this frame's 2D overlay list, centred on x, y (origin at the screen centre, +y down).
// The laid-out glyphs (count u16 +8, s16 x,y pairs at +0x10) start at the element's x, so it is shifted back by half their width (last x plus one average advance).
static void hud_link_centred(uint8_t* rdram, recomp_context* ctx, uint32_t font, uint32_t slot, float x, float y) {
    const uint32_t elem = rs64::mp::call(rdram, ctx, 0x80063C3Cu, {font, slot}).v0;
    if (elem < 0x80000000u || elem >= 0xA0000000u) {
        return;
    }
    const uint32_t n = rs64::mips::rh(rdram, elem + 0x8u);
    const uint32_t xy = rw(rdram, elem + 0x10u);
    float half = 0.0f;
    if (n > 1 && xy >= 0x80000000u && xy < 0xA0000000u) {
        const float last = (float)(int16_t)rs64::mips::rh(rdram, xy + 4u * (n - 1));
        half = (last + last / (float)(n - 1)) * 0.5f;
    }
    wf(rdram, elem + 0x18u, x - half);
    wf(rdram, elem + 0x1Cu, y);
    const uint32_t head = rw(rdram, 0x80138D28u);
    ww(rdram, elem + 0x0u, head);
    if (head) {
        ww(rdram, head + 0x4u, elem);
    }
    ww(rdram, elem + 0x4u, 0);
    ww(rdram, 0x80138D28u, elem);
}

// runInMissionFrame's draw pass, right after the subtitle is linked (0x800FAC70): the co-op message line, laid out on the mission font (u16 0x80B39014) slot 1 when it changes,
// then linked into the 2D overlay list (head at viewport C+8 = 0x80138D28; element +0 next, +4 prev) at the top of the screen (origin at the centre, +y down; H at C+0x14).
extern "C" void rs64_ghost_hud_draw(uint8_t* rdram, recomp_context* ctx) {
    Ghost& s = g();
    static std::string shown;
    static uint32_t buf = 0;
    static double left_at = -1.0;
    if (!s.in_session) {
        shown.clear();
        left_at = -1.0;
        return;
    }
    if (s.peer_left && left_at < 0.0) {
        left_at = now_s();
    }
    const bool left_recent = s.peer_left && now_s() - left_at < 5.0;
    const std::string msg = rs64::ls::hud_message(s.out, left_recent, s.remote_down && s.remote_in, rb(rdram, kLives));
    const uint32_t font = rs64::mips::rh(rdram, 0x80B39014u);
    if (msg != shown) {
        hud_slot_text(rdram, ctx, font, 1, &buf, msg);
        shown = msg;
    }
    const float h = (float)(int32_t)rw(rdram, 0x80138D20u + 0x14u);
    if (!msg.empty()) {
        hud_link_centred(rdram, ctx, font, 1, 0.0f, -h * 0.5f + 28.0f);
    }
    // The other player's pilot name 30 units above their ship on screen: its position projected the way the crosshair is (camera matrix 0x80138D4C, row-major 3x4;
    // cot of half the fov at C+0x20; sx = (W/2)(x cot / z) 0.75, sy = (H/2)(y cot / z), both +down, as the local craft checks out; shown only in front of the near plane C+0x24).
    static std::string label_shown;
    static uint32_t label_buf = 0;
    const bool label_on = !s.despawned && s.puppet_shown && !s.remote_name.empty();
    const std::string label = label_on ? s.remote_name : std::string();
    if (label != label_shown) {
        hud_slot_text(rdram, ctx, font, 2, &label_buf, label);
        label_shown = label;
    }
    if (!label_on) {
        return;
    }
    const Vec3 p = s.puppet_pos;
    float m[12];
    for (int i = 0; i < 12; ++i) {
        m[i] = rf(rdram, 0x80138D4Cu + 4u * (uint32_t)i);
    }
    const float vx = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[9];
    const float vy = m[3] * p.x + m[4] * p.y + m[5] * p.z + m[10];
    const float vz = m[6] * p.x + m[7] * p.y + m[8] * p.z + m[11];
    if (vz <= rf(rdram, 0x80138D20u + 0x24u)) {
        return;
    }
    const float half_fov = rf(rdram, 0x80138D20u + 0x20u) * 3.14159265f / 360.0f;
    const float cot = std::cos(half_fov) / std::sin(half_fov);
    const float w = (float)(int32_t)rw(rdram, 0x80138D20u + 0x10u);
    hud_link_centred(rdram, ctx, font, 2, (w * 0.5f) * (vx * cot / vz) * 0.75f, (h * 0.5f) * (vy * cot / vz) - 30.0f);
}

// The imposter's tick: the pose to show this frame (0 = hold the current pose, seeded from it until the first state arrives).
extern "C" int rs64_ghost_puppet(const float seed[9], float dt, float out[12]) {
    Ghost& s = g();
    if (!s.in_session || s.despawned) {
        return 0;
    }
    if (!s.puppet.has_target()) {
        PuppetState at;
        at.pos = {seed[0], seed[1], seed[2]};
        at.fwd = {seed[3], seed[4], seed[5]};
        at.down = {seed[6], seed[7], seed[8]};
        s.puppet.reset(at);
        return 0;
    }
    const PuppetState p = s.puppet.step(now_s(), dt);
    const Vec3* parts[4] = {&p.pos, &p.fwd, &p.down, &p.vel};
    for (int i = 0; i < 4; ++i) {
        out[i * 3 + 0] = parts[i]->x;
        out[i * 3 + 1] = parts[i]->y;
        out[i * 3 + 2] = parts[i]->z;
    }
    s.puppet_shown = true;
    s.puppet_pos = p.pos;
    s.puppet_up = p.down;
    return 1;
}

// The other player's S-foils for their puppet, 0 closed .. 250 open.
extern "C" int rs64_ghost_remote_foil(void) {
    return g().remote_foil;
}
