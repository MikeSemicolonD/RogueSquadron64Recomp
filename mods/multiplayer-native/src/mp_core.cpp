#include "mp_core.h"
#include <cmath>
#include <cstring>

namespace rs64::ls {

namespace {

struct Span { uint32_t addr; uint32_t size; };

bool in_rdram(uint32_t kseg0, uint32_t size) {
    return kseg0 >= 0x80000000u && (uint64_t)(kseg0 - 0x80000000u) + size <= kRdramSize;
}

uint8_t rd8(const uint8_t* rdram, uint32_t kseg0) {
    return rdram[(kseg0 - 0x80000000u) ^ 3];
}

uint32_t rd32(const uint8_t* rdram, uint32_t kseg0) {
    return ((uint32_t)rd8(rdram, kseg0) << 24) | ((uint32_t)rd8(rdram, kseg0 + 1) << 16) |
           ((uint32_t)rd8(rdram, kseg0 + 2) << 8) | (uint32_t)rd8(rdram, kseg0 + 3);
}

uint64_t hash_spans(const uint8_t* rdram, const std::vector<Span>& spans) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const Span& s : spans) {
        for (uint32_t i = 0; i < s.size; ++i) {
            const uint8_t b = rd8(rdram, s.addr + i);
            h = fnv1a(&b, 1, h);
        }
    }
    return h;
}

void wr8(uint8_t* rdram, uint32_t kseg0, uint8_t v) {
    rdram[(kseg0 - 0x80000000u) ^ 3] = v;
}

void wr32(uint8_t* rdram, uint32_t kseg0, uint32_t v) {
    for (uint32_t i = 0; i < 4; ++i) {
        wr8(rdram, kseg0 + i, (uint8_t)(v >> (24 - 8 * i)));
    }
}

}

uint64_t hash_imposter(const uint8_t* rdram) {
    uint64_t h = hash_spans(rdram, {{kImposterRec, 0x100}});
    const uint32_t blk = rd32(rdram, kImposterRec + 0xE0);
    if (in_rdram(blk, 0x30)) {
        for (uint32_t i = 0; i < 0x30; ++i) {
            const uint8_t b = rd8(rdram, blk + i);
            h = fnv1a(&b, 1, h);
        }
    }
    return h;
}

void pad2_bytes(const FrameInput* f, uint8_t out[6]) {
    for (int i = 0; i < 6; ++i) {
        out[i] = f ? f->pad[i] : 0;
    }
    out[0] &= (uint8_t)~0x10;
    out[4] = 1;
    out[5] = 0;
}

float poly_sin(float x) {
    const float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}

float poly_cos(float x) {
    const float x2 = x * x;
    return 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f));
}

static Vec3 v_add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 v_scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float v_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 v_cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 v_norm(Vec3 a) {
    const float len = std::sqrt(v_dot(a, a));
    return len > 0.0f ? v_scale(a, 1.0f / len) : a;
}

static float fabs_f(float v) {
    return v < 0.0f ? -v : v;
}

static float clamp1(float v) {
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

// Normalised only when long enough, like the game's normIfLen.
static Vec3 v_norm_if_len(Vec3 a) {
    const float len = std::sqrt(v_dot(a, a));
    return len >= 1e-4f ? v_scale(a, 1.0f / len) : a;
}

float asin_a(float x) {
    if (x > 0.99999899f) {
        return 1.5707964f;
    }
    if (x < -0.99999899f) {
        return -1.5707964f;
    }
    const bool small = fabs_f(x) < 0.70710677f;
    float p = x;
    if (!small) {
        p = std::sqrt(1.0f - x * x);
        if (x < 0.0f) {
            p = -p;
        }
    }
    if (!(fabs_f(p) < 0.001f)) {
        const float p2 = p * p;
        const float p3 = p * p2;
        const float p5 = p3 * p2;
        const float p7 = p5 * p2;
        const float p9 = p7 * p2;
        p = (((p + p3 * 0.16666667f) + p5 * 0.075f) + p7 * 0.044642858f) + p9 * 0.047445997f;
    }
    if (small) {
        return p;
    }
    return x < 0.0f ? -1.5707964f - p : 1.5707964f - p;
}

// tickPlayerHudIndicators' normalisation (clamp, 0.015 dead zone), then the physics dead zone (0.1) and squaring unless linear.
static float stick_axis(int8_t raw, float scale, bool linear) {
    float v = clamp1((float)raw / scale);
    if (fabs_f(v) < 0.015f) {
        v = 0.0f;
    }
    float d = 0.0f;
    if (v > 0.1f) {
        d = v - 0.1f;
    } else if (v < -0.1f) {
        d = v + 0.1f;
    }
    return linear ? d : d * fabs_f(d);
}

static float bank_deg(Vec3 right, Vec3 fwd) {
    const Vec3 level_up = v_norm(v_cross(fwd, v_norm(v_cross(Vec3{0.0f, 1.0f, 0.0f}, fwd))));
    return (-asin_a(v_dot(right, level_up)) * 180.0f) / 3.1415927f;
}

// orient = orient * E(yaw, pitch, roll) (0x800AA570), a body-frame rotation; columns of orient are right, down, fwd.
static void rotate_body(ImposterCraft& c, float yaw_deg, float pitch_deg, float roll_deg) {
    const float k = 0.017453292f;
    const float ca = poly_cos(yaw_deg * k);
    const float sa = poly_sin(yaw_deg * k);
    const float cb = poly_cos(pitch_deg * k);
    const float sb = poly_sin(pitch_deg * k);
    const float cc = poly_cos(roll_deg * k);
    const float sc = poly_sin(roll_deg * k);
    const float e01 = ca * sc + (sa * cc) * sb;
    const float e02 = -sa * cb;
    const float e11 = cc * cb;
    const float e12 = sb;
    const float e21 = sa * sc - (ca * cc) * sb;
    const float e22 = ca * cb;
    const Vec3 r = v_cross(c.down, c.fwd);
    const Vec3 d = c.down;
    const Vec3 f = c.fwd;
    const Vec3 nd = v_add(v_add(v_scale(r, e01), v_scale(d, e11)), v_scale(f, e21));
    const Vec3 nf = v_add(v_add(v_scale(r, e02), v_scale(d, e12)), v_scale(f, e22));
    c.fwd = v_norm(nf);
    c.down = v_norm(v_add(nd, v_scale(c.fwd, -v_dot(nd, c.fwd))));
}

void imposter_fly(ImposterCraft& c, const ImposterPad& in, const ImposterWorld& w, float dt) {
    // Speed (updateXwingFlightControls): only the target ramps, and speed takes it every frame. sc is last frame's S-foil speed scale.
    const float sc = c.foil_t * 0.5f + 1.0f;
    c.boost_hold = in.boost ? c.boost_hold + dt : 0.0f;
    float tgt = c.target;
    if (in.brake && !in.boost) {
        tgt -= 0.1875f;
    }
    if (tgt < sc * 2.25f) {
        tgt = sc * 2.25f;
    }
    tgt += c.boost_hold * 0.1875f;
    if (!in.brake && !in.boost && c.speed < 2.625f) {
        tgt += 0.1875f;
        if (tgt > sc * 2.625f) {
            tgt = sc * 2.625f;
        }
    }
    if (c.foil_t != 0.0f) {
        tgt = sc * 3.75f;
    }
    if (tgt > sc * 3.75f) {
        tgt = sc * 3.75f;
    }
    c.target = tgt;
    c.speed = tgt;
    // S-foils: the toggle (or, when closed, the brake) starts a one-second move; it finishes on its own.
    if (!c.foil_closed) {
        if (c.foil_t != 0.0f || in.foil) {
            c.foil_t += dt;
            if (c.foil_t > 1.0f) {
                c.foil_t = 1.0f;
                c.foil_closed = true;
            }
        }
    } else if (c.foil_t != 1.0f || in.foil || in.brake) {
        c.foil_t -= dt;
        if (c.foil_t < 0.0f) {
            c.foil_t = 0.0f;
            c.foil_closed = false;
        }
    }
    const float handling = c.foil_closed ? 1.28f : 1.6f;

    // Rotation (updatePlayerCraftPhysics, normal control mode, X-wing constants).
    const bool linear = (w.settings & 0x40) != 0;
    const float x = stick_axis(in.stick_x, 75.0f, linear);
    const float y = stick_axis(in.stick_y, 80.0f, linear);
    const Vec3 right = v_cross(c.down, c.fwd);
    const bool fp = right.y < 0.0f;
    float roll = -((-(x * handling) * -30.0f) * 1.75f);
    float yaw = -(((x * handling) * 40.0f) * handling);
    float bank = bank_deg(right, c.fwd);
    if (fabs_f(bank) >= 60.0f) {
        yaw *= 0.5f;
    }
    float turn_pitch = fabs_f(yaw) * dt * (fabs_f(x) * 2.5f * (fabs_f(bank) / 60.0f));
    if ((yaw < 0.0f) == fp) {
        yaw *= 0.5f;
        roll *= 1.5f;
    }
    if (fp ? x > 0.0f : x < 0.0f) {
        turn_pitch = 0.0f;
    }
    float pitch = (-30.0f * handling) * (-(y * handling));
    if (w.settings & 0x4) {
        pitch = -pitch;
    }
    float d8 = yaw * dt;
    float dc = pitch * dt;
    float e0 = roll * dt;
    const Vec3 flat = v_norm_if_len(Vec3{c.fwd.x, 0.0f, c.fwd.z});
    const float p_abs = 90.0f - asin_a(v_dot(flat, c.fwd)) * 57.2958f;
    const bool centred = x == 0.0f && y == 0.0f;
    if ((w.settings & 0x1) && centred) {
        if (p_abs > 60.0f && (c.fwd.y < 0.0f || w.level == 10)) {
            c.level_latch = true;
        }
        if (p_abs < 0.5f) {
            c.level_latch = false;
        }
    } else {
        c.level_latch = false;
    }
    bool auto_roll = centred;
    if (p_abs >= 75.0f) {
        auto_roll = false;
    }
    if (p_abs >= 60.0f) {
        d8 = 0.0f;
    }
    if (p_abs >= 45.0f) {
        turn_pitch *= 1.0f - (p_abs - 45.0f) / 45.0f;
    }
    dc -= turn_pitch;
    if (in.freeze) {
        d8 = 0.0f;
        dc = 0.0f;
        e0 = 0.0f;
    }
    if (c.down.y < 0.0f) {
        bank = fp ? 180.0f - bank : -180.0f - bank;
    }
    if (auto_roll && fabs_f(bank) > 0.1f) {
        e0 = (-bank * 2.0f) * dt;
    }
    if (c.level_latch) {
        dc = (c.fwd.y < 0.0f ? p_abs * 0.75f : -p_abs * 0.75f) * dt;
    }
    if (fabs_f(d8) < 0.1f) {
        d8 = 0.0f;
    }
    if (fabs_f(dc) < 0.1f) {
        dc = 0.0f;
    }
    if (fabs_f(e0) < 0.1f) {
        e0 = 0.0f;
    }
    rotate_body(c, d8, dc, e0);

    c.pos = v_add(c.pos, v_scale(c.fwd, c.speed * dt));
    // World Y points down: the ground is a larger y than the craft, the ceiling a smaller one.
    if (c.pos.y > w.ground_y - 0.02f) {
        c.pos.y = w.ground_y - 0.02f;
    }
    if (c.pos.y < w.ceiling_y) {
        c.pos.y = w.ceiling_y;
    }
}

bool imposter_fire(bool fire, uint16_t* cooldown) {
    if (*cooldown > 0) {
        --*cooldown;
        return false;
    }
    if (fire) {
        *cooldown = 6;
        return true;
    }
    return false;
}

void clear_imposter_record(uint8_t* rdram) {
    for (uint32_t i = 0; i < 0x100; ++i) {
        wr8(rdram, kImposterRec + i, 0);
    }
}

Vec3 imposter_spawn_pos(const Vec3& p1, const Vec3& right) {
    return Vec3{p1.x + right.x * 1.5f, p1.y, p1.z + right.z * 1.5f};
}

bool imposter_spawn_ok(uint32_t slot, uint32_t npc, uint32_t blk, uint32_t mesh) {
    auto in_ram = [](uint32_t a) { return a >= 0x80000000u && a < 0x80000000u + (uint32_t)kRdramSize; };
    return slot < kNpcSlots && in_ram(npc) && in_ram(blk) && in_ram(mesh);
}

static uint16_t rd16(const uint8_t* rdram, uint32_t a) {
    return (uint16_t)((rd8(rdram, a) << 8) | rd8(rdram, a + 1));
}

bool npc_pool_room(const uint8_t* rdram) {
    return (int32_t)rd32(rdram, 0x80130BC8u) >= 0 && rd16(rdram, 0x80130BB4u) != 0xFFFFu;
}

bool mesh_loaded(const uint8_t* rdram, const char* name) {
    uint32_t sum = 0;
    for (const char* c = name; *c; ++c) {
        sum += (uint8_t)*c;
    }
    uint16_t idx = rd16(rdram, 0x801394B0u + (sum % 25u) * 2);
    for (int hops = 0; idx != 0xFFFFu && hops < 1024; ++hops) {
        const uint32_t entry = 0x80139020u + idx * 12u;
        if (!in_rdram(entry, 12)) {
            return false;
        }
        const uint32_t str = rd32(rdram, entry + 4);
        if (!in_rdram(str, 16)) {
            return false;
        }
        size_t i = 0;
        while (i < 16 && name[i] && rd8(rdram, str + (uint32_t)i) == (uint8_t)name[i]) {
            ++i;
        }
        if (i == 16 || (name[i] == 0 && rd8(rdram, str + (uint32_t)i) == 0)) {
            return true;
        }
        idx = rd16(rdram, entry);
    }
    return false;
}

const char* wingman_model_name(int model) {
    static const char* const kModels[8] = {"wmxwng", "wmywng", "wmawng", "wmvwng", "wmspdr", "wmfalc", "tie", "t16"};
    return (model >= 0 && model < 8) ? kModels[model] : nullptr;
}

int wingman_model(const uint8_t* rdram) {
    for (int i = 0; i < 8; ++i) {
        if (mesh_loaded(rdram, wingman_model_name(i))) {
            return i;
        }
    }
    return -1;
}

const char* hud_message(bool out, bool peer_left, bool peer_down, int lives) {
    if (out) {
        return "SPECTATING";
    }
    if (peer_left) {
        return "WINGMATE LEFT";
    }
    if (peer_down) {
        return lives == 0 ? "WINGMATE OUT" : "WINGMATE DOWN";
    }
    return "";
}

int browse_report(int state, uint32_t result) {
    return (state == 4 && result == 0xFFFFFEu) ? 2 : state;
}

bool TeamTriggers::on_edge(int player, uint16_t event, bool enter) {
    const uint8_t bit = (uint8_t)(1u << (player & 1));
    uint8_t& in = inside_[event];
    if (enter) {
        if (in & bit) {
            return false;
        }
        const bool first = in == 0;
        in |= bit;
        return first;
    }
    if (!(in & bit)) {
        return false;
    }
    in &= (uint8_t)~bit;
    return in == 0;
}

void TeamTriggers::forget(int player) {
    const uint8_t bit = (uint8_t)(1u << (player & 1));
    for (auto& e : inside_) {
        e.second &= (uint8_t)~bit;
    }
}

int puppet_model(int craft) {
    return ((craft >= 0 && craft <= 5) || craft == 7) ? craft : -1;
}

LifeOutcome shared_life(uint8_t pool, bool player_out) {
    if (player_out) {
        return LifeOutcome::Ignore;
    }
    if (pool >= 2) {
        return LifeOutcome::Spend;
    }
    return pool == 1 ? LifeOutcome::Out : LifeOutcome::GameOver;
}

void spectate_pad(uint8_t pad[6], uint16_t survivor_held, uint16_t fire_mask) {
    const uint16_t fire = survivor_held & fire_mask;
    pad[0] = (uint8_t)((pad[0] & 0x10) | (fire >> 8));
    pad[1] = (uint8_t)fire;
    pad[2] = 0;
    pad[3] = 0;
}

void SkipShare::on_remote(uint16_t scene) {
    have_ = true;
    scene_ = scene;
    pressing_since_s_ = -1.0;
}

bool SkipShare::press(double now_s, bool cinematic, uint16_t scene) {
    if (!have_) {
        return false;
    }

    // Wrapping compare: the phase byte wraps after 256 missions.
    const int16_t ahead = (int16_t)(uint16_t)(scene_ - scene);

    if (ahead < 0 || (ahead == 0 && pressing_since_s_ >= 0.0 && !cinematic)) {
        have_ = false;
        pressing_since_s_ = -1.0;
        return false;
    }

    if (ahead > 0 || !cinematic) {
        return false;
    }

    if (pressing_since_s_ < 0.0) {
        pressing_since_s_ = now_s;
    }
    last_press_s_ = now_s;
    return std::fmod(now_s - pressing_since_s_, 2.0 * kPulseS) < kPulseS;
}

void Puppet::reset(const PuppetState& at) {
    shown_ = at;
    target_ = at;
    has_target_ = false;
}

void Puppet::on_state(const PuppetState& s, double now_s, double sent_s) {
    target_ = s;
    received_s_ = now_s;
    has_target_ = true;
    if (interp_s_ <= 0.0 || sent_s < 0.0) {
        return;
    }
    // A sender restart (new mission) or reordering: the timeline starts over.
    if (!buf_.empty() && sent_s <= buf_.back().sent) {
        if (sent_s < buf_.back().sent - 1.0) {
            buf_.clear();
            offsets_.clear();
        } else {
            return;
        }
    }
    buf_.push_back({sent_s, s});
    while (buf_.size() > 2 && buf_.front().sent < sent_s - 1.5) {
        buf_.pop_front();
    }
    offsets_.push_back({now_s, now_s - sent_s});
    while (offsets_.size() > 1 && offsets_.front().first < now_s - 4.0) {
        offsets_.pop_front();
    }
    offset_ = offsets_.front().second;
    for (const auto& o : offsets_) {
        offset_ = o.second < offset_ ? o.second : offset_;
    }
}

static PuppetState lerp_state(const PuppetState& a, const PuppetState& b, float u) {
    PuppetState r;
    r.pos = v_add(a.pos, v_scale(v_add(b.pos, v_scale(a.pos, -1.0f)), u));
    r.vel = v_add(a.vel, v_scale(v_add(b.vel, v_scale(a.vel, -1.0f)), u));
    Vec3 f = v_add(a.fwd, v_scale(v_add(b.fwd, v_scale(a.fwd, -1.0f)), u));
    Vec3 d = v_add(a.down, v_scale(v_add(b.down, v_scale(a.down, -1.0f)), u));
    f = v_dot(f, f) < 1e-8f ? b.fwd : v_norm(f);
    d = v_add(d, v_scale(f, -v_dot(d, f)));
    r.fwd = f;
    r.down = v_dot(d, d) < 1e-8f ? b.down : v_norm(d);
    return r;
}

PuppetState Puppet::sample(double now_s) const {
    if (buf_.empty()) {
        return target_;
    }
    const double t = now_s - offset_ - interp_s_;
    if (t <= buf_.front().sent) {
        return buf_.front().s;
    }
    if (t >= buf_.back().sent) {
        double ahead = t - buf_.back().sent;
        ahead = ahead > kPuppetCoastS ? kPuppetCoastS : ahead;
        PuppetState r = buf_.back().s;
        r.pos = v_add(r.pos, v_scale(r.vel, (float)ahead));
        return r;
    }
    for (size_t i = 1; i < buf_.size(); ++i) {
        if (buf_[i].sent >= t) {
            const Sample& a = buf_[i - 1];
            const Sample& b = buf_[i];
            const double span = b.sent - a.sent;
            return lerp_state(a.s, b.s, span > 0.0 ? (float)((t - a.sent) / span) : 1.0f);
        }
    }
    return buf_.back().s;
}

PuppetState Puppet::step(double now_s, float dt) {
    if (!has_target_) {
        return shown_;
    }
    if (interp_s_ > 0.0 && !buf_.empty()) {
        // The interpolated track is already smooth; a light blend only covers the step from extrapolating back to real samples.
        const PuppetState raw = sample(now_s);
        const Vec3 err = v_add(raw.pos, v_scale(shown_.pos, -1.0f));
        float k = (float)(dt / 0.05);
        k = k > 1.0f ? 1.0f : (k < 0.0f ? 0.0f : k);
        const bool snap = v_dot(err, err) > kPuppetSnap * kPuppetSnap || v_dot(shown_.fwd, raw.fwd) < 0.0f;
        shown_.pos = snap ? raw.pos : v_add(shown_.pos, v_scale(err, k));
        shown_.fwd = raw.fwd;
        shown_.down = raw.down;
        shown_.vel = raw.vel;
        return shown_;
    }
    double age = now_s - received_s_;
    age = age < 0.0 ? 0.0 : (age > kPuppetCoastS ? kPuppetCoastS : age);
    const Vec3 goal = v_add(target_.pos, v_scale(target_.vel, (float)age));
    const Vec3 err = v_add(goal, v_scale(shown_.pos, -1.0f));
    float k = (float)(dt / kPuppetBlendS);
    k = k > 1.0f ? 1.0f : (k < 0.0f ? 0.0f : k);
    const float k_pos = v_dot(err, err) > kPuppetSnap * kPuppetSnap ? 1.0f : k;
    shown_.pos = v_add(shown_.pos, v_scale(err, k_pos));
    // Streamed turns move a few degrees per update; a heading more than 90 degrees off is a respawn or teleport, so it snaps (a straight blend would stall at the reversal).
    const float k_rot = v_dot(shown_.fwd, target_.fwd) < 0.0f ? 1.0f : k;
    Vec3 f = v_add(shown_.fwd, v_scale(v_add(target_.fwd, v_scale(shown_.fwd, -1.0f)), k_rot));
    Vec3 d = v_add(shown_.down, v_scale(v_add(target_.down, v_scale(shown_.down, -1.0f)), k_rot));
    if (v_dot(f, f) < 1e-8f) {
        f = target_.fwd;
    }
    f = v_norm(f);
    d = v_add(d, v_scale(f, -v_dot(d, f)));
    if (v_dot(d, d) < 1e-8f) {
        d = v_add(target_.down, v_scale(f, -v_dot(target_.down, f)));
    }
    shown_.fwd = f;
    shown_.down = v_norm(d);
    shown_.vel = target_.vel;
    return shown_;
}

void build_imposter_record(uint8_t* rdram, const Vec3& pos) {
    auto wf = [rdram](uint32_t a, float f) { uint32_t b; memcpy(&b, &f, 4); wr32(rdram, a, b); };
    clear_imposter_record(rdram);
    const uint32_t r = kImposterRec;
    wr8(rdram, r + 0x1, 0x27);
    wr8(rdram, r + 0x6, 0xFF);
    wr8(rdram, r + 0x7, 0xFF);
    wr32(rdram, r + 0x0C, 0x800A9028u);
    wf(r + 0x10, pos.x);
    wf(r + 0x14, pos.y);
    wf(r + 0x18, pos.z);
    wf(r + 0x28, 0.014f);
    wf(r + 0x2C, 0.014f);
    wf(r + 0x30, 0.014f);
    wr8(rdram, r + 0x34, 0xFF);
    wr8(rdram, r + 0x35, 0xFF);
    wr32(rdram, r + 0x54, 0x80000000u);
    // Health per difficulty (npcHealthTableIndex 0x80137CE4 selects the entry).
    wr32(rdram, r + 0x6C, 1000u);
    wr32(rdram, r + 0x70, 1000u);
    wr32(rdram, r + 0x74, 1000u);
    wr32(rdram, r + 0x90, 7u);
    wr32(rdram, r + 0x94, 0xF0u);
    // Private state (the handler reads nothing past +0xDC): S-foil t +0xE8, speed +0xEC, fire cooldown +0xF2, target +0xF4, boost hold +0xF8, level latch +0xFC, S-foils closed +0xFD.
    wf(r + 0xEC, 2.625f);
    wf(r + 0xF4, 2.625f);
    wr8(rdram, r + 0xF0, '-');
}

}
