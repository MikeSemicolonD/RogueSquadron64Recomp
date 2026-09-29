#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "touch_input.h"
#include "touch_menu.h"
#include "touch_config.h"
#include <string>

using namespace rs64::touch;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b) (std::fabs((a) - (b)) < 1e-3f)

// Fingers are interpreted in the context of the most recent poll, so each test polls its context first.
static Engine make_engine(Context ctx = Context::Flight) {
    Engine e;
    Config c;
    c.enabled = true;
    e.set_config(c);
    e.set_screen(2000.0f, 1000.0f);
    e.poll(ctx, 0);
    return e;
}

static const ButtonDef* find_button(const OverlayState& ds, uint16_t mask) {
    for (const ButtonDef& b : ds.buttons) {
        if (b.mask == mask) {
            return &b;
        }
    }
    return nullptr;
}

static const ButtonDef* find_id(const std::vector<ButtonDef>& v, const char* id) {
    for (const ButtonDef& b : v) {
        if (b.id && std::strcmp(b.id, id) == 0) {
            return &b;
        }
    }
    return nullptr;
}

static void tap(Engine& e, int64_t id, float x, float y, uint32_t ms) {
    e.finger(id, FingerEvent::Down, x, y, ms);
    e.finger(id, FingerEvent::Up, x, y, ms + 50);
}

// Menu B/START sit in the screen corners; a near miss still presses them (hit radius 2x the drawn one).
static void test_menu_corner_buttons_padded() {
    Engine e = make_engine(Context::ListMenu);
    const OverlayState ds = e.draw_state(Context::ListMenu);
    const ButtonDef* b = find_button(ds, B);
    const ButtonDef* s = find_button(ds, START);
    CHECK(b && s);
    if (!b || !s) {
        return;
    }
    e.finger(1, FingerEvent::Down, b->cx, b->cy + b->r * 1.8f, 0);
    CHECK((e.poll(Context::ListMenu, 16).buttons & B) != 0);
    e.finger(1, FingerEvent::Up, b->cx, b->cy + b->r * 1.8f, 30);
    e.finger(2, FingerEvent::Down, s->cx - s->r * 1.8f / 2.0f, s->cy, 100);
    CHECK((e.poll(Context::ListMenu, 116).buttons & START) != 0);
    e.finger(2, FingerEvent::Up, s->cx - s->r * 1.8f / 2.0f, s->cy, 130);
    e.finger(3, FingerEvent::Down, b->cx, b->cy + b->r * 2.3f, 200);
    CHECK((e.poll(Context::ListMenu, 216).buttons & B) == 0);
}

// Steering roll from the accelerometer (device axes, portrait-natural). Landscape: screen up = device +x, right = device -y.
static void test_tilt_angles_from_gravity() {
    const float g = 9.81f;
    const float deg = 3.14159265f / 180.0f;
    float roll = 0.0f;
    CHECK(tilt_angles(g, 0.0f, 0.0f, false, &roll) && NEAR(roll, 0.0f));
    const float t = 20.0f * deg;
    CHECK(tilt_angles(g * std::cos(t), g * std::sin(t), 0.0f, false, &roll) && std::fabs(roll - t) < 1e-3f);
    CHECK(tilt_angles(g * std::cos(t), -g * std::sin(t), 0.0f, false, &roll) && std::fabs(roll + t) < 1e-3f);
    // Flipped landscape mirrors the in-plane axes.
    CHECK(tilt_angles(-g * std::cos(t), -g * std::sin(t), 0.0f, true, &roll) && std::fabs(roll - t) < 1e-3f);
    // Top edge tipped away (screen facing further up) does not read as roll.
    const float p = 30.0f * deg;
    CHECK(tilt_angles(g * std::cos(p), 0.0f, g * std::sin(p), false, &roll) && NEAR(roll, 0.0f));
    // Lying nearly flat: roll cannot be read.
    CHECK(!tilt_angles(0.5f, 0.5f, g, false, &roll));
}

static Engine tilt_engine(float curve) {
    Engine e;
    Config c;
    c.enabled = true;
    c.gyro = true;
    c.steer_curve = curve;
    e.set_config(c);
    e.set_screen(2000.0f, 1000.0f);
    return e;
}

static void test_tilt_steers_x() {
    Engine e = tilt_engine(1.0f);
    const Config c = e.config();
    const float deg = 3.14159265f / 180.0f;
    e.tilt(0.0f, true);
    CHECK(NEAR(e.poll(Context::Flight, 0).x, 0.0f));
    e.tilt((c.steer_dead_deg - 1.0f) * deg, true);
    CHECK(NEAR(e.poll(Context::Flight, 16).x, 0.0f));
    e.tilt((c.steer_dead_deg + c.steer_full_deg) / 2.0f * deg, true);
    CHECK(std::fabs(e.poll(Context::Flight, 32).x - 0.5f) < 0.01f);
    e.tilt((c.steer_full_deg + 10.0f) * deg, true);
    CHECK(NEAR(e.poll(Context::Flight, 48).x, 1.0f));
    e.tilt(-(c.steer_full_deg + 10.0f) * deg, true);
    CHECK(NEAR(e.poll(Context::Flight, 64).x, -1.0f));
    e.tilt(-(c.steer_full_deg + 10.0f) * deg, false);
    CHECK(NEAR(e.poll(Context::Flight, 80).x, 0.0f));
    e.tilt((c.steer_full_deg + 10.0f) * deg, true);
    CHECK(NEAR(e.poll(Context::ListMenu, 96).x, 0.0f));
    Config off = c;
    off.gyro = false;
    e.set_config(off);
    CHECK(NEAR(e.poll(Context::Flight, 112).x, 0.0f));
    // Response curve: halfway through the range gives 0.5^curve.
    Engine k = tilt_engine(2.0f);
    k.tilt((c.steer_dead_deg + c.steer_full_deg) / 2.0f * deg, true);
    CHECK(std::fabs(k.poll(Context::Flight, 0).x - 0.25f) < 0.01f);
}

// Up/down is mouse-like: the stick follows the phone's pitch motion (top edge tipping away = up) and returns to center when it stops,
// so holding an angle never keeps the nose moving. Past the noise floor, output starts at pitch_min so small motions clear the game's dead zone.
static void test_pitch_follows_motion() {
    Engine e = tilt_engine(1.0f);
    const Config c = e.config();
    e.tilt(0.0f, true);
    e.gyro_rate(0.0f, 0.0f);
    CHECK(NEAR(e.poll(Context::Flight, 0).y, 0.0f));
    e.gyro_rate(0.0f, 0.5f);
    CHECK(NEAR(e.poll(Context::Flight, 16).y, c.pitch_min + c.pitch_gain * 0.5f));
    e.gyro_rate(0.0f, -0.2f);
    CHECK(NEAR(e.poll(Context::Flight, 32).y, -(c.pitch_min + c.pitch_gain * 0.2f)));
    // Stopped (or sensor noise): centered.
    e.gyro_rate(0.0f, 0.01f);
    CHECK(NEAR(e.poll(Context::Flight, 48).y, 0.0f));
    e.gyro_rate(0.0f, 10.0f);
    CHECK(NEAR(e.poll(Context::Flight, 64).y, 1.0f));
    // Works while the phone is too flat to read roll.
    e.tilt(0.0f, false);
    e.gyro_rate(0.0f, 0.5f);
    const Pad p = e.poll(Context::Flight, 80);
    CHECK(NEAR(p.x, 0.0f) && p.y > 0.0f);
}

// Steering: roll rate adds an immediate nudge on top of the held tilt, clamped to full stick.
static void test_steer_rate_assist() {
    Engine e = tilt_engine(1.0f);
    const Config c = e.config();
    const float deg = 3.14159265f / 180.0f;
    e.tilt(0.0f, true);
    e.gyro_rate(1.0f, 0.0f);
    CHECK(NEAR(e.poll(Context::Flight, 0).x, c.rate_assist));
    e.gyro_rate(0.01f, 0.0f);
    CHECK(NEAR(e.poll(Context::Flight, 16).x, 0.0f));
    e.tilt((c.steer_full_deg + 10.0f) * deg, true);
    e.gyro_rate(5.0f, 0.0f);
    CHECK(NEAR(e.poll(Context::Flight, 32).x, 1.0f));
}

static void test_layout_override_moves_and_scales() {
    Engine e;
    Config c;
    c.enabled = true;
    c.layout["A"] = { 0.50f, 0.50f, 2.0f };
    e.set_config(c);
    e.set_screen(2000.0f, 1000.0f);
    e.poll(Context::Flight, 0);
    const OverlayState ds = e.draw_state(Context::Flight);
    const ButtonDef* a = find_button(ds, A);
    CHECK(a && NEAR(a->cx, 0.50f) && NEAR(a->cy, 0.50f) && NEAR(a->r, 0.17f));
    e.finger(1, FingerEvent::Down, 0.52f, 0.50f, 0);
    CHECK((e.poll(Context::Flight, 16).buttons & A) != 0);
    e.finger(1, FingerEvent::Up, 0.52f, 0.50f, 30);
    e.finger(2, FingerEvent::Down, 0.86f, 0.70f, 100);
    CHECK((e.poll(Context::Flight, 116).buttons & A) == 0);
}

static void test_stick_split() {
    Engine e;
    Config c;
    c.enabled = true;
    c.stick_split = 0.30f;
    e.set_config(c);
    e.set_screen(2000.0f, 1000.0f);
    e.poll(Context::Flight, 0);
    e.finger(1, FingerEvent::Down, 0.40f, 0.60f, 0);
    e.finger(1, FingerEvent::Move, 0.48f, 0.60f, 16);
    e.poll(Context::Flight, 32);
    CHECK(!e.draw_state(Context::Flight).stick_down);
    e.finger(2, FingerEvent::Down, 0.20f, 0.60f, 50);
    e.finger(2, FingerEvent::Move, 0.28f, 0.60f, 66);
    CHECK(e.poll(Context::Flight, 82).x > 0.5f);
}

static void test_gear_only_in_menus_opens_editor() {
    Engine e = make_engine(Context::Flight);
    CHECK(!find_id(e.draw_state(Context::Flight).buttons, "GEAR"));
    e.poll(Context::ListMenu, 0);
    const OverlayState menu_ds = e.draw_state(Context::ListMenu);
    const ButtonDef* g = find_id(menu_ds.buttons, "GEAR");
    CHECK(g != nullptr);
    if (!g) {
        return;
    }
    const float gx = g->cx, gy = g->cy;
    CHECK(!e.editing());
    tap(e, 1, gx, gy, 10);
    CHECK(e.editing());
    CHECK(e.poll(Context::ListMenu, 100).buttons == 0);
}

static void test_edit_drag_moves_button_and_blocks_input() {
    Engine e = make_engine(Context::Flight);
    e.begin_edit();
    const OverlayState ds0 = e.draw_state(Context::ListMenu);
    CHECK(ds0.visible && ds0.editing);
    CHECK(find_button(ds0, A) != nullptr);
    e.finger(1, FingerEvent::Down, 0.86f, 0.70f, 0);
    e.finger(1, FingerEvent::Move, 0.70f, 0.40f, 16);
    const Pad p = e.poll(Context::Flight, 32);
    CHECK(p.buttons == 0 && NEAR(p.x, 0.0f));
    e.finger(1, FingerEvent::Up, 0.70f, 0.40f, 48);
    const OverlayState moved = e.draw_state(Context::Flight);
    const ButtonDef* a = find_button(moved, A);
    CHECK(a && NEAR(a->cx, 0.70f) && NEAR(a->cy, 0.40f));
    CHECK(e.draw_state(Context::Flight).selected == A);
}

static void test_edit_plus_minus_scale_selected() {
    Engine e = make_engine(Context::Flight);
    e.begin_edit();
    tap(e, 1, 0.86f, 0.70f, 0);
    const std::vector<ButtonDef> tools = e.draw_state(Context::Flight).tools;
    const ButtonDef* plus = find_id(tools, "PLUS");
    const ButtonDef* minus = find_id(tools, "MINUS");
    CHECK(plus && minus);
    if (!plus || !minus) {
        return;
    }
    const float r0 = find_button(e.draw_state(Context::Flight), A)->r;
    tap(e, 2, plus->cx, plus->cy, 100);
    CHECK(find_button(e.draw_state(Context::Flight), A)->r > r0 * 1.05f);
    tap(e, 3, minus->cx, minus->cy, 200);
    tap(e, 4, minus->cx, minus->cy, 300);
    CHECK(find_button(e.draw_state(Context::Flight), A)->r < r0);
    CHECK(e.draw_state(Context::Flight).selected == A);
}

static void test_edit_pinch_scales() {
    Engine e = make_engine(Context::Flight);
    e.begin_edit();
    const float r0 = find_button(e.draw_state(Context::Flight), A)->r;
    e.finger(1, FingerEvent::Down, 0.86f, 0.70f, 0);
    e.finger(2, FingerEvent::Down, 0.86f, 0.80f, 10);
    e.finger(2, FingerEvent::Move, 0.86f, 0.85f, 20);
    const OverlayState pinched = e.draw_state(Context::Flight);
    const ButtonDef* a = find_button(pinched, A);
    CHECK(a && NEAR(a->r, r0 * 1.5f) && NEAR(a->cx, 0.86f) && NEAR(a->cy, 0.70f));
    e.finger(2, FingerEvent::Move, 0.86f, 2.0f, 30);
    CHECK(NEAR(find_button(e.draw_state(Context::Flight), A)->r, r0 * 2.0f));
    e.finger(2, FingerEvent::Up, 0.86f, 2.0f, 40);
    e.finger(1, FingerEvent::Up, 0.86f, 0.70f, 50);
}

static void test_edit_split_drag() {
    Engine e = make_engine(Context::Flight);
    e.begin_edit();
    CHECK(NEAR(e.draw_state(Context::Flight).stick_split, 0.50f));
    e.finger(1, FingerEvent::Down, 0.505f, 0.40f, 0);
    e.finger(1, FingerEvent::Move, 0.35f, 0.40f, 16);
    e.finger(1, FingerEvent::Up, 0.35f, 0.40f, 32);
    CHECK(NEAR(e.draw_state(Context::Flight).stick_split, 0.35f));
    e.finger(2, FingerEvent::Down, 0.355f, 0.40f, 50);
    e.finger(2, FingerEvent::Move, 0.01f, 0.40f, 66);
    e.finger(2, FingerEvent::Up, 0.01f, 0.40f, 80);
    CHECK(NEAR(e.draw_state(Context::Flight).stick_split, 0.2f));
}

static void test_edit_reset_and_done() {
    Engine e = make_engine(Context::Flight);
    e.begin_edit();
    e.finger(1, FingerEvent::Down, 0.86f, 0.70f, 0);
    e.finger(1, FingerEvent::Move, 0.60f, 0.30f, 16);
    e.finger(1, FingerEvent::Up, 0.60f, 0.30f, 32);
    const std::vector<ButtonDef> tools = e.draw_state(Context::Flight).tools;
    const ButtonDef* reset = find_id(tools, "RESET");
    const ButtonDef* done = find_id(tools, "DONE");
    CHECK(reset && done);
    if (!reset || !done) {
        return;
    }
    tap(e, 2, reset->cx, reset->cy, 100);
    const OverlayState reset_ds = e.draw_state(Context::Flight);
    const ButtonDef* a = find_button(reset_ds, A);
    CHECK(a && NEAR(a->cx, 0.86f) && NEAR(a->cy, 0.70f));
    CHECK(e.config().layout.empty());
    e.finger(3, FingerEvent::Down, 0.86f, 0.70f, 200);
    e.finger(3, FingerEvent::Move, 0.60f, 0.30f, 216);
    e.finger(3, FingerEvent::Up, 0.60f, 0.30f, 232);
    CHECK(!e.take_layout_saved());
    tap(e, 4, done->cx, done->cy, 300);
    CHECK(!e.editing());
    CHECK(e.take_layout_saved());
    CHECK(!e.take_layout_saved());
    const auto it = e.config().layout.find("A");
    CHECK(it != e.config().layout.end() && NEAR(it->second.cx, 0.60f));
    e.finger(5, FingerEvent::Down, 0.60f, 0.30f, 400);
    CHECK((e.poll(Context::Flight, 416).buttons & A) != 0);
}

static void test_edit_back_finishes() {
    Engine e = make_engine(Context::Flight);
    e.begin_edit();
    e.back_pressed();
    CHECK(!e.editing());
    CHECK(e.take_layout_saved());
    CHECK((e.poll(Context::Flight, 16).buttons & B) == 0);
}

static void test_config_layout_roundtrip() {
    const std::string path = "touch_layout_roundtrip.json";
    Config c;
    c.layout["A"] = { 0.25f, 0.75f, 1.5f };
    c.layout["CU"] = { 0.10f, 0.20f, 0.8f };
    c.stick_split = 0.4f;
    CHECK(save_config(c, path));
    const Config l = load_config(path);
    CHECK(l.layout.size() == 2);
    const auto a = l.layout.find("A");
    CHECK(a != l.layout.end() && NEAR(a->second.cx, 0.25f) && NEAR(a->second.cy, 0.75f) && NEAR(a->second.scale, 1.5f));
    CHECK(NEAR(l.stick_split, 0.4f));
    std::remove(path.c_str());
}

static void test_stick_center_deflect_release() {
    Engine e = make_engine();
    const float r = e.config().stick_radius;
    e.finger(1, FingerEvent::Down, 0.20f, 0.60f, 0);
    Pad p = e.poll(Context::Flight, 1);
    CHECK(NEAR(p.x, 0.0f) && NEAR(p.y, 0.0f));
    e.finger(1, FingerEvent::Move, 0.20f + 0.5f * r / 2.0f, 0.60f, 10);   // half the radius
    p = e.poll(Context::Flight, 11);
    const float expect = std::pow((0.5f - 0.08f) / 0.92f, e.config().stick_curve);
    CHECK(std::fabs(p.x - expect) < 0.01f);
    e.finger(1, FingerEvent::Move, 0.20f, 0.60f - 0.30f, 20);
    p = e.poll(Context::Flight, 21);
    CHECK(NEAR(p.y, 1.0f) && NEAR(p.x, 0.0f));
    e.finger(1, FingerEvent::Up, 0.20f, 0.30f, 30);
    p = e.poll(Context::Flight, 31);
    CHECK(NEAR(p.x, 0.0f) && NEAR(p.y, 0.0f));
}

// Softer default: 0.18 of screen height to full deflection, 1.5 response curve.
// Menus show a B (back) button top-left mirroring START top-right; pressing it holds B.
static void test_menu_back_button() {
    for (Context ctx : { Context::ListMenu, Context::Carousel, Context::PauseMenu }) {
        Engine e = make_engine(ctx);
        const OverlayState ds = e.draw_state(ctx);
        const ButtonDef* b = find_button(ds, B);
        const ButtonDef* s = find_button(ds, START);
        CHECK(b && s);
        if (!b || !s) continue;
        CHECK(b->cx < 0.1f && NEAR(b->cy, s->cy) && NEAR(b->cx, 1.0f - s->cx));
        e.finger(1, FingerEvent::Down, b->cx, b->cy, 0);
        CHECK((e.poll(ctx, 1).buttons & B) != 0);
    }
}

static void test_stick_defaults() {
    Config c;
    CHECK(NEAR(c.stick_radius, 0.18f));
    CHECK(NEAR(c.stick_curve, 1.5f));
}

static void test_stick_dead_zone() {
    Engine e = make_engine();
    e.finger(1, FingerEvent::Down, 0.20f, 0.60f, 0);
    e.finger(1, FingerEvent::Move, 0.20f, 0.60f - 0.005f, 5);
    Pad p = e.poll(Context::Flight, 6);
    CHECK(NEAR(p.y, 0.0f));
}

static void test_two_fingers_stick_and_button() {
    Engine e = make_engine();
    OverlayState ds = e.draw_state(Context::Flight);
    const ButtonDef* a = find_button(ds, A);
    CHECK(a != nullptr);
    if (!a) return;
    e.finger(1, FingerEvent::Down, 0.20f, 0.60f, 0);
    e.finger(1, FingerEvent::Move, 0.20f, 0.40f, 5);
    e.finger(2, FingerEvent::Down, a->cx, a->cy, 6);
    Pad p = e.poll(Context::Flight, 7);
    CHECK((p.buttons & A) != 0);
    CHECK(p.y > 0.9f);
}

static void test_slide_between_buttons() {
    Engine e = make_engine();
    OverlayState ds = e.draw_state(Context::Flight);
    const ButtonDef* a = find_button(ds, A);
    const ButtonDef* b = find_button(ds, B);
    CHECK(a && b);
    if (!a || !b) return;
    e.finger(3, FingerEvent::Down, a->cx, a->cy, 0);
    CHECK((e.poll(Context::Flight, 1).buttons & A) != 0);
    e.finger(3, FingerEvent::Move, b->cx, b->cy, 5);
    Pad p = e.poll(Context::Flight, 6);
    CHECK((p.buttons & A) == 0);
    CHECK((p.buttons & B) != 0);
}

static void test_stick_finger_never_presses_buttons() {
    Engine e = make_engine();
    OverlayState ds = e.draw_state(Context::Flight);
    const ButtonDef* a = find_button(ds, A);
    CHECK(a != nullptr);
    if (!a) return;
    e.finger(1, FingerEvent::Down, 0.20f, 0.60f, 0);
    e.finger(1, FingerEvent::Move, a->cx, a->cy, 5);
    CHECK((e.poll(Context::Flight, 6).buttons & A) == 0);
}

static void test_carousel_swipe_one_pulse() {
    Engine e = make_engine(Context::Carousel);
    e.finger(1, FingerEvent::Down, 0.50f, 0.50f, 0);
    e.finger(1, FingerEvent::Move, 0.50f + 0.12f / 2.0f, 0.50f, 100);
    int right_polls = 0;
    for (int i = 0; i < 6; ++i) {
        if (e.poll(Context::Carousel, 101 + i).x > 0.9f) ++right_polls;
    }
    e.finger(1, FingerEvent::Move, 0.50f + 0.30f / 2.0f, 0.50f, 150);
    for (int i = 0; i < 6; ++i) {
        if (e.poll(Context::Carousel, 151 + i).x > 0.9f) ++right_polls;
    }
    e.finger(1, FingerEvent::Up, 0.65f, 0.50f, 200);
    CHECK(right_polls == 3);
}

// List/pause drags scrub: one step at the swipe threshold, then one per further 0.05; each step is a separate press.
static int count_right_presses(Engine& e, Context ctx, uint32_t t0, int polls) {
    int presses = 0;
    bool was = false;
    for (int i = 0; i < polls; ++i) {
        const bool now = e.poll(ctx, t0 + i).x > 0.9f;
        if (now && !was) ++presses;
        was = now;
    }
    return presses;
}

static int count_presses(Engine& e, Context ctx, uint32_t t0, int polls, bool right) {
    int presses = 0;
    bool was = false;
    for (int i = 0; i < polls; ++i) {
        const float x = e.poll(ctx, t0 + i).x;
        const bool now = right ? x > 0.9f : x < -0.9f;
        if (now && !was) ++presses;
        was = now;
    }
    return presses;
}

// Horizontal drags in lists fine-tune sliders: one step per 0.015 of drag once the axis locks, both ways.
static void test_list_horizontal_fine_drag() {
    Engine e = make_engine(Context::ListMenu);
    e.finger(1, FingerEvent::Down, 0.30f, 0.50f, 0);
    e.finger(1, FingerEvent::Move, 0.30f + 0.046f / 2.0f, 0.50f, 900);   // 0.046 right: 3 steps
    CHECK(count_presses(e, Context::ListMenu, 901, 30, true) == 3);
    e.finger(1, FingerEvent::Move, 0.30f + 0.016f / 2.0f, 0.50f, 1000);  // back to 0.016: 2 steps left
    CHECK(count_presses(e, Context::ListMenu, 1001, 30, false) == 2);
    e.finger(1, FingerEvent::Up, 0.30f + 0.016f / 2.0f, 0.50f, 1100);
}

// Vertical drags in lists move between entries: one step at 0.08, then one per 0.05.
static void test_list_vertical_drag_steps() {
    Engine e = make_engine(Context::ListMenu);
    e.finger(1, FingerEvent::Down, 0.30f, 0.30f, 0);
    e.finger(1, FingerEvent::Move, 0.30f, 0.30f + 0.19f, 900);
    int presses = 0;
    bool was = false;
    for (int i = 0; i < 40; ++i) {
        const bool now = e.poll(Context::ListMenu, 901 + i).y < -0.9f;
        if (now && !was) ++presses;
        was = now;
    }
    CHECK(presses == 3);
    e.finger(1, FingerEvent::Up, 0.30f, 0.49f, 1000);
}

static void test_pause_drag_scrubs_and_tap_confirms() {
    Engine e = make_engine(Context::PauseMenu);
    e.finger(1, FingerEvent::Down, 0.30f, 0.50f, 0);
    e.finger(1, FingerEvent::Move, 0.30f + 0.031f / 2.0f, 0.50f, 100);
    CHECK(count_presses(e, Context::PauseMenu, 101, 30, true) == 2);
    e.finger(1, FingerEvent::Up, 0.37f, 0.50f, 150);
    // A tap on a pause line confirms it; a tap that misses every line does nothing (it must not confirm CONTINUE).
    e.set_menu_tap([&](float, float y) { return y > 0.6f ? TapAction::Confirm : TapAction::None; });
    e.finger(2, FingerEvent::Down, 0.50f, 0.50f, 500);
    e.finger(2, FingerEvent::Up, 0.50f, 0.50f, 540);
    for (int i = 0; i < 6; ++i) {
        CHECK((e.poll(Context::PauseMenu, 541 + i).buttons & A) == 0);
    }
    e.finger(3, FingerEvent::Down, 0.50f, 0.70f, 600);
    e.finger(3, FingerEvent::Up, 0.50f, 0.70f, 640);
    CHECK((e.poll(Context::PauseMenu, 641).buttons & A) != 0);
}

static void test_pause_touch_highlights_directly() {
    Engine e = make_engine(Context::PauseMenu);
    std::vector<int> hovered;
    auto row_at = [](float y) { return y < 0.40f ? -1 : y < 0.50f ? 0 : y < 0.60f ? 1 : 2; };
    e.set_menu_hover([&](float, float y) { const int row = row_at(y); hovered.push_back(row); return row; });
    int confirms = 0;
    e.set_menu_tap([&](float, float y) { ++confirms; return row_at(y) >= 0 ? TapAction::Confirm : TapAction::None; });
    // Press highlights; sliding onto another line highlights it without stick pulses or a confirm.
    e.finger(1, FingerEvent::Down, 0.50f, 0.45f, 0);
    CHECK(!hovered.empty() && hovered.back() == 0);
    e.finger(1, FingerEvent::Move, 0.50f, 0.55f, 100);
    CHECK(hovered.back() == 1);
    for (int i = 0; i < 10; ++i) {
        const Pad p = e.poll(Context::PauseMenu, 101 + i);
        CHECK(p.buttons == 0 && NEAR(p.y, 0.0f));
    }
    e.finger(1, FingerEvent::Up, 0.50f, 0.55f, 200);
    CHECK(confirms == 0);
    // A slow press released on the line it started on selects it.
    e.finger(2, FingerEvent::Down, 0.50f, 0.55f, 1000);
    e.finger(2, FingerEvent::Up, 0.51f, 0.56f, 1800);
    CHECK(confirms == 1);
    CHECK((e.poll(Context::PauseMenu, 1801).buttons & A) != 0);
    for (int i = 0; i < 10; ++i) {
        e.poll(Context::PauseMenu, 1802 + i);
    }
    // A press off every line does nothing.
    e.finger(3, FingerEvent::Down, 0.50f, 0.30f, 3000);
    e.finger(3, FingerEvent::Up, 0.50f, 0.30f, 3040);
    CHECK(confirms == 1);
    CHECK((e.poll(Context::PauseMenu, 3041).buttons & A) == 0);
}

// Counts press edges in one direction over a stretch of 33 ms polls starting at t0.
static int wheel_presses(Engine& e, uint32_t t0, int polls, bool right) {
    int presses = 0;
    bool was = false;
    for (int i = 0; i < polls; ++i) {
        const float x = e.poll(Context::Wheel, t0 + 33u * i).x;
        const bool now = right ? x > 0.9f : x < -0.9f;
        if (now && !was) ++presses;
        was = now;
    }
    return presses;
}

// The passcode wheel spins while the stick is held and snaps to a letter on release, so touch drives it like an analog stick.
// Drag: stick x proportional to the drag distance (full at 0.2), held while the finger is down.
static void test_wheel_drag_is_analog() {
    Engine e = make_engine(Context::Wheel);
    e.finger(1, FingerEvent::Down, 0.50f, 0.57f, 0);
    e.finger(1, FingerEvent::Move, 0.50f + 0.08f / 2.0f, 0.57f, 10);
    const float mid = e.poll(Context::Wheel, 20).x;
    CHECK(mid > 0.2f && mid < 0.6f);
    CHECK(NEAR(e.poll(Context::Wheel, 60).x, mid));   // held, not pulsed
    e.finger(1, FingerEvent::Move, 0.50f + 0.30f / 2.0f, 0.57f, 100);
    CHECK(NEAR(e.poll(Context::Wheel, 110).x, 1.0f));
    e.finger(1, FingerEvent::Move, 0.50f - 0.30f / 2.0f, 0.57f, 200);
    CHECK(NEAR(e.poll(Context::Wheel, 210).x, -1.0f));
    e.finger(1, FingerEvent::Up, 0.35f, 0.57f, 300);
    CHECK(NEAR(e.poll(Context::Wheel, 310).x, 0.0f));
}

// A quick side tap holds full stick that way for kWheelTapPolls (one letter); it never confirms. Center tap = A.
static void test_wheel_side_and_center_taps() {
    Engine e = make_engine(Context::Wheel);
    e.finger(1, FingerEvent::Down, 0.30f, 0.57f, 0);
    e.finger(1, FingerEvent::Up, 0.30f, 0.57f, 40);
    int held = 0;
    int a_polls = 0;
    for (int i = 0; i < 20; ++i) {
        const Pad p = e.poll(Context::Wheel, 41 + 33u * i);
        if (p.x < -0.9f) ++held;
        if (p.buttons & A) ++a_polls;
    }
    CHECK(a_polls == 0);
    CHECK(held >= 6 && held <= 10);
    e.finger(2, FingerEvent::Down, 0.50f, 0.57f, 1000);
    e.finger(2, FingerEvent::Up, 0.50f, 0.57f, 1040);
    CHECK((e.poll(Context::Wheel, 1041).buttons & A) != 0);
}

// Holding a side holds full stick that way until release.
static void test_wheel_side_hold() {
    Engine e = make_engine(Context::Wheel);
    e.finger(1, FingerEvent::Down, 0.75f, 0.57f, 0);
    for (int i = 0; i < 30; ++i) {
        CHECK(e.poll(Context::Wheel, 1 + 33u * i).x > 0.9f);
    }
    e.finger(1, FingerEvent::Up, 0.75f, 0.57f, 1000);
    CHECK(NEAR(e.poll(Context::Wheel, 1001).x, 0.0f));
}

static void test_tap_vs_slow_press() {
    Engine e = make_engine(Context::Carousel);
    e.finger(1, FingerEvent::Down, 0.50f, 0.50f, 0);
    e.finger(1, FingerEvent::Up, 0.505f, 0.50f, 100);
    int a_polls = 0;
    for (int i = 0; i < 5; ++i) {
        if (e.poll(Context::Carousel, 101 + i).buttons & A) ++a_polls;
    }
    CHECK(a_polls == 3);
    e.finger(2, FingerEvent::Down, 0.50f, 0.50f, 1000);
    e.finger(2, FingerEvent::Up, 0.50f, 0.50f, 1400);
    CHECK((e.poll(Context::Carousel, 1401).buttons & A) == 0);
}

static void test_list_tap_calls_menu_and_pulses_a() {
    Engine e = make_engine(Context::ListMenu);
    int calls = 0;
    e.set_menu_tap([&](float, float) { ++calls; return TapAction::Confirm; });
    e.finger(1, FingerEvent::Down, 0.50f, 0.40f, 0);
    e.finger(1, FingerEvent::Up, 0.50f, 0.40f, 50);
    CHECK((e.poll(Context::ListMenu, 51).buttons & A) != 0);
    CHECK(calls == 1);
    e.poll(Context::ListMenu, 52);
    e.poll(Context::ListMenu, 53);
    e.set_menu_tap([&](float, float) { return TapAction::None; });
    e.finger(2, FingerEvent::Down, 0.50f, 0.90f, 500);
    e.finger(2, FingerEvent::Up, 0.50f, 0.90f, 550);
    for (int i = 0; i < 4; ++i) {
        CHECK((e.poll(Context::ListMenu, 551 + i).buttons & A) == 0);
    }
}

// Carousel taps offer the tap to the menu layer (it may select an entry) and always press A.
static void test_carousel_tap_offers_menu_then_a() {
    Engine e = make_engine(Context::Carousel);
    int calls = 0;
    e.set_menu_tap([&](float, float) { ++calls; return TapAction::None; });
    e.finger(1, FingerEvent::Down, 0.50f, 0.80f, 0);
    e.finger(1, FingerEvent::Up, 0.50f, 0.80f, 40);
    CHECK(calls == 1);
    CHECK((e.poll(Context::Carousel, 41).buttons & A) != 0);
}

// A carousel tap the menu layer maps to a direction (e.g. a side save slot) steps that way instead of pressing A.
static void test_carousel_tap_direction() {
    Engine e = make_engine(Context::Carousel);
    e.set_menu_tap([&](float, float) { return TapAction::Left; });
    e.finger(1, FingerEvent::Down, 0.30f, 0.80f, 0);
    e.finger(1, FingerEvent::Up, 0.30f, 0.80f, 40);
    bool saw_left = false;
    for (int i = 0; i < 6; ++i) {
        const Pad p = e.poll(Context::Carousel, 41 + i);
        CHECK((p.buttons & A) == 0);
        if (p.x < -0.9f) saw_left = true;
    }
    CHECK(saw_left);
}

// Host-injected presses last exactly the requested polls (used for 1-frame slider steps).
static void test_inject_one_poll() {
    Engine e = make_engine(Context::ListMenu);
    e.inject(0, 1.0f, 0.0f, 1);
    CHECK(e.poll(Context::ListMenu, 1).x > 0.9f);
    CHECK(NEAR(e.poll(Context::ListMenu, 2).x, 0.0f));
}

// Horizontal fine-drag nudges are single-poll presses (one game frame = one slider unit).
static void test_fine_nudge_is_one_poll() {
    Engine e = make_engine(Context::ListMenu);
    e.finger(1, FingerEvent::Down, 0.30f, 0.50f, 0);
    e.finger(1, FingerEvent::Move, 0.30f + 0.031f / 2.0f, 0.50f, 500);   // past the 0.03 axis lock: 2 nudges
    int held = 0;
    for (int i = 0; i < 8; ++i) {
        if (e.poll(Context::ListMenu, 501 + i).x > 0.9f) ++held;
    }
    CHECK(held == 2);
}

// Taps only count on the game picture: the black side bars (where B and START sit) ignore taps that miss those buttons.
static void test_taps_outside_picture_ignored() {
    for (Context ctx : { Context::Carousel, Context::Cutscene, Context::Wheel }) {
        Engine e = make_engine(ctx);
        e.set_picture(0.17f, 0.83f);
        e.finger(1, FingerEvent::Down, 0.10f, 0.50f, 0);
        e.finger(1, FingerEvent::Up, 0.10f, 0.50f, 40);
        for (int i = 0; i < 12; ++i) {
            const Pad p = e.poll(ctx, 41 + i);
            CHECK(p.buttons == 0 && NEAR(p.x, 0.0f));
        }
        e.finger(2, FingerEvent::Down, 0.50f, 0.40f, 500);
        e.finger(2, FingerEvent::Up, 0.50f, 0.40f, 540);
        CHECK((e.poll(ctx, 541).buttons & (A | START)) != 0);
    }
}

static void test_cutscene_tap_is_start_and_back_is_b() {
    Engine e = make_engine(Context::Cutscene);
    e.finger(1, FingerEvent::Down, 0.50f, 0.50f, 0);
    e.finger(1, FingerEvent::Up, 0.50f, 0.50f, 40);
    CHECK((e.poll(Context::Cutscene, 41).buttons & START) != 0);
    e.back_pressed();
    bool saw_b = false;
    for (int i = 0; i < 10; ++i) {
        const uint16_t b = e.poll(Context::Carousel, 100 + i).buttons;
        if (b & B) saw_b = true;
        CHECK(!((b & B) && (b & START)));   // B follows the START press, never overlaps it
    }
    CHECK(saw_b);
}

static void test_arbitration() {
    Engine e = make_engine();
    e.finger(1, FingerEvent::Down, 0.20f, 0.60f, 0);
    e.finger(1, FingerEvent::Move, 0.20f, 0.60f - 0.7f * e.config().stick_radius, 5);
    Pad t = e.poll(Context::Flight, 6);
    CHECK(e.active());
    CHECK(t.y > 0.3f && t.y < 0.9f);
    uint16_t btn = B;
    float x = 0.95f, y = 0.0f;
    merge(t, &btn, &x, &y);
    CHECK(btn == B);
    CHECK(NEAR(x, 0.95f) && NEAR(y, 0.0f));
    uint16_t btn2 = 0;
    float x2 = 0.1f, y2 = 0.0f;
    merge(t, &btn2, &x2, &y2);
    CHECK(NEAR(x2, 0.0f) && NEAR(y2, t.y));
    e.physical_input();
    CHECK(!e.active());
    CHECK(!e.draw_state(Context::Flight).visible);
    e.finger(2, FingerEvent::Down, 0.20f, 0.60f, 100);
    CHECK(e.active());
}

static void test_disabled_does_nothing() {
    Engine e;
    e.set_screen(2000.0f, 1000.0f);
    e.poll(Context::Cutscene, 0);
    e.finger(1, FingerEvent::Down, 0.5f, 0.5f, 0);
    e.finger(1, FingerEvent::Up, 0.5f, 0.5f, 10);
    Pad p = e.poll(Context::Cutscene, 11);
    CHECK(!p.has_input && p.buttons == 0);
}

static void test_gyro_off_is_neutral() {
    Engine e = make_engine();
    e.tilt(0.5f, true);
    e.gyro_rate(0.0f, 2.0f);
    Pad p = e.poll(Context::Flight, 1);
    CHECK(NEAR(p.x, 0.0f) && NEAR(p.y, 0.0f));
}

static void test_gyro_only_in_flight() {
    Engine e = make_engine(Context::Carousel);
    Config c = e.config();
    c.gyro = true;
    e.set_config(c);
    e.tilt(0.8f, true);
    e.gyro_rate(0.0f, 2.0f);
    CHECK(NEAR(e.poll(Context::Carousel, 1).x, 0.0f));
}

// ---- Menu layer: fake 8 MB RDRAM (native-endian words: bytes at off^3, halves at off^2) ----

static std::vector<uint8_t> g_ram(0x1100000u);   // 8 MB RDRAM plus the start of the mod heap (0x81000000)
static void wb(uint32_t a, uint8_t v) { g_ram[(a - 0x80000000u) ^ 3] = v; }
static void wh(uint32_t a, int16_t v) { std::memcpy(&g_ram[(a - 0x80000000u) ^ 2], &v, 2); }
static void ww(uint32_t a, uint32_t v) { std::memcpy(&g_ram[a - 0x80000000u], &v, 4); }
static void wstr(uint32_t a, const char* s) {
    for (size_t i = 0; i <= std::strlen(s); ++i) wb(a + (uint32_t)i, (uint8_t)s[i]);
}
static uint8_t current_entry() { return g_ram[(0x800CE730 + 0x94 - 0x80000000) ^ 3]; }

static void setup_menu(uint8_t count, uint8_t current, uint16_t active) {
    std::fill(g_ram.begin(), g_ram.end(), 0);
    const uint32_t G = 0x800CE730;
    ww(G, 0x80200000);
    wb(G + 0x04, 2);
    const char* labels[] = { "BIOGRAPHIES", "ELITE ROGUES", "PASSCODES", "BACK" };
    for (int i = 0; i < count; ++i) {
        wstr(0x80300000 + i * 0x40, labels[i]);
        ww(G + 0x08 + 4 * i, 0x80300000 + i * 0x40);
        wh(G + 0x30 + 4 * i, 0);
        wh(G + 0x32 + 4 * i, 0);
        float one = 1.0f;
        uint32_t bits;
        std::memcpy(&bits, &one, 4);
        ww(G + 0x54 + 4 * i, bits);
    }
    wb(G + 0x94, current);
    wb(G + 0x95, count);
    wh(G + 0x96, 40);
    wh(G + 0x98, (int16_t)active);
    wh(0x800CFF50, 2);   // front-end screen: the menu tree
}

static float center_of(const std::vector<Box>& bx, int entry, bool y) {
    for (const Box& b : bx) {
        if (b.entry == entry) return y ? (b.y0 + b.y1) / 2 : (b.x0 + b.x1) / 2;
    }
    return -1.0f;
}

static void test_hit_and_select() {
    setup_menu(4, 0, 0x0F);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(read_menu(g_ram.data(), &s, &bx, 2000, 1000));
    CHECK(bx.size() == 4);
    CHECK(tap_select(g_ram.data(), s, center_of(bx, 2, false), center_of(bx, 2, true), 2000, 1000));
    CHECK(current_entry() == 2);
}

// Stacked entry boxes never overlap: padding is trimmed to the midpoint between neighbours.
static void test_boxes_do_not_overlap() {
    setup_menu(4, 0, 0x0F);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(read_menu(g_ram.data(), &s, &bx, 2340, 1080));
    for (size_t i = 0; i + 1 < bx.size(); ++i) {
        CHECK(bx[i].y1 <= bx[i + 1].y0 + 1e-5f);
    }
}

static void test_miss_and_inactive() {
    setup_menu(4, 0, 0x0B);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(read_menu(g_ram.data(), &s, &bx, 2000, 1000));
    CHECK(!tap_select(g_ram.data(), s, 0.01f, 0.01f, 2000, 1000));
    setup_menu(4, 0, 0x0F);
    std::vector<Box> all;
    read_menu(g_ram.data(), &s, &all, 2000, 1000);
    const float ex = center_of(all, 2, false), ey = center_of(all, 2, true);
    setup_menu(4, 0, 0x0B);
    read_menu(g_ram.data(), &s, &bx, 2000, 1000);
    CHECK(!tap_select(g_ram.data(), s, ex, ey, 2000, 1000));
    CHECK(current_entry() == 0);
}

static void test_bad_menu_rejected() {
    setup_menu(4, 0, 0x0F);
    wb(0x800CE730 + 0x95, 9);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(!read_menu(g_ram.data(), &s, &bx, 2000, 1000));
    setup_menu(4, 0, 0x0F);
    ww(0x800CE730 + 0x08, 0x12345678);
    CHECK(!read_menu(g_ram.data(), &s, &bx, 2000, 1000));
    setup_menu(4, 0, 0x0F);
    ww(0x800CE730, 0x00001234);
    CHECK(!read_menu(g_ram.data(), &s, &bx, 2000, 1000));
}

static void test_no_write_on_menu_change() {
    setup_menu(4, 0, 0x0F);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(read_menu(g_ram.data(), &s, &bx, 2000, 1000));
    wb(0x800CE730 + 0x04, 3);
    CHECK(!tap_select(g_ram.data(), s, center_of(bx, 2, false), center_of(bx, 2, true), 2000, 1000));
    CHECK(current_entry() == 0);
}

static void test_hitbox_widescreen() {
    setup_menu(4, 0, 0x0F);
    MenuSnapshot s{};
    std::vector<Box> narrow, wide;
    read_menu(g_ram.data(), &s, &narrow, 1500, 1000);
    read_menu(g_ram.data(), &s, &wide, 2166, 1000);
    const float nx = (center_of(narrow, 0, false) - 0.5f) * 1500.0f;
    const float wx = (center_of(wide, 0, false) - 0.5f) * 2166.0f;
    CHECK(std::fabs(nx - wx) < 1.0f);
    CHECK(NEAR(center_of(narrow, 0, true), center_of(wide, 0, true)));
}

// The loaded overlay decides flight vs menus; the classifier id only refines menu screens
// (its "mission" rule also matches in the menus after an attract demo left objectives set).
// Measured on the S24+ (2340x1080): main menu START text center at 29.75% x, 83.15% y of the window.
static void test_main_menu_calibration() {
    setup_menu(2, 0, 0x03);
    wb(0x800CE730 + 0x04, 0);
    wh(0x800CE730 + 0x96, 165);
    wh(0x800CE730 + 0x30, -156);
    wh(0x800CE730 + 0x34, -143);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(read_menu(g_ram.data(), &s, &bx, 2340, 1080));
    CHECK(std::fabs(center_of(bx, 0, false) - 0.2975f) < 0.02f);
    CHECK(std::fabs(center_of(bx, 0, true) - 0.8315f) < 0.02f);
}

// Game rule: y = base(+0x96) + 36*i - 18*n + y_off; Controller Settings BACK (base -90, n=1, y_off 283) measured top 216.3 of 240.
static void test_controller_settings_back() {
    setup_menu(1, 0, 0x01);
    wb(0x800CE730 + 0x04, 5);
    wh(0x800CE730 + 0x96, -90);
    wh(0x800CE730 + 0x32, 283);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(read_menu(g_ram.data(), &s, &bx, 2340, 1080));
    const float top = bx.empty() ? -1.0f : bx[0].y0 * 240.0f + g_menu_layout.line_h * 0.2f;
    CHECK(std::fabs(top - 216.3f) < 1.0f);
}

// SELECT GAME: the side save slots (320x240: x 35..92 and 229..287, y 165..215) step left/right; elsewhere no action.
static void test_select_game_slots() {
    float x, y;
    n64_to_window(63.0f, 190.0f, 2340, 1080, &x, &y);
    CHECK(select_game_tap(x, y, 2340, 1080) == TapAction::Left);
    n64_to_window(258.0f, 190.0f, 2340, 1080, &x, &y);
    CHECK(select_game_tap(x, y, 2340, 1080) == TapAction::Right);
    n64_to_window(160.0f, 190.0f, 2340, 1080, &x, &y);
    CHECK(select_game_tap(x, y, 2340, 1080) == TapAction::None);
    // The side slots' "---" name lines step too (320x240 x 108..124 and 196..212, y ~198).
    n64_to_window(116.0f, 198.0f, 2340, 1080, &x, &y);
    CHECK(select_game_tap(x, y, 2340, 1080) == TapAction::Left);
    n64_to_window(204.0f, 198.0f, 2340, 1080, &x, &y);
    CHECK(select_game_tap(x, y, 2340, 1080) == TapAction::Right);
}

// Edge-to-edge carousels (Concert Hall, Showroom): ◁◁◁ / ▷▷▷ in the bottom corners step; elsewhere no action (A).
static void test_carousel_corner_arrows() {
    CHECK(carousel_arrow_tap(0.11f, 0.93f) == TapAction::Left);
    CHECK(carousel_arrow_tap(0.89f, 0.94f) == TapAction::Right);
    CHECK(carousel_arrow_tap(0.50f, 0.93f) == TapAction::None);
    CHECK(carousel_arrow_tap(0.89f, 0.40f) == TapAction::None);
}

// SELECT YOUR CRAFT: top "PRESS C FOR CRAFT DESCRIPTION" line = C; screen edges step crafts; elsewhere no action (A).
static void test_craft_select_taps() {
    CHECK(craft_select_tap(0.50f, 0.14f) == TapAction::CButton);
    CHECK(craft_select_tap(0.10f, 0.50f) == TapAction::Left);
    CHECK(craft_select_tap(0.90f, 0.50f) == TapAction::Right);
    CHECK(craft_select_tap(0.50f, 0.50f) == TapAction::None);
}

// Front-end screen u16 @0x800CFF50 (menu overlay main loop): 2 = menu tree, 1 = SELECT LEVEL, 0 = SELECT YOUR CRAFT.
// SELECT GAME is the menu tree with menu id 1 and entry 0 typed as a save slot (type byte 0x800CE758 == 3).
static void test_account_screen() {
    setup_menu(4, 0, 0x0F);
    wb(0x800CE730 + 0x04, 1);
    wb(0x800CE758, 3);
    wh(0x800CFF50, 2);
    CHECK(account_screen(g_ram.data()) == AccountScreen::SelectGame);
    wb(0x800CE758, 0);
    CHECK(account_screen(g_ram.data()) == AccountScreen::NotAccount);   // ENTER NAME / erase confirm: normal list handling
    wh(0x800CFF50, 1);
    CHECK(account_screen(g_ram.data()) == AccountScreen::Levels);
    wh(0x800CFF50, 0);
    CHECK(account_screen(g_ram.data()) == AccountScreen::Craft);
    wh(0x800CFF50, 2);
    wb(0x800CE758, 3);
    wb(0x800CE730 + 0x04, 2);
    CHECK(account_screen(g_ram.data()) == AccountScreen::NotAccount);
}

// SELECT LEVEL: the arrows sit in the bottom corners (window 18.75% / 81.25% x, 90% y); elsewhere no action (A).
static void test_level_select_arrows() {
    CHECK(level_select_tap(0.19f, 0.90f) == TapAction::Left);
    CHECK(level_select_tap(0.81f, 0.90f) == TapAction::Right);
    CHECK(level_select_tap(0.50f, 0.90f) == TapAction::None);
    CHECK(level_select_tap(0.50f, 0.97f) == TapAction::CButton);
    CHECK(level_select_tap(0.19f, 0.40f) == TapAction::None);
}

// SOUND SETTINGS volume bar: entry sub-types 21/22/23 = music/sfx/speech; bar spans 320x240 x 120..200 under the entry.
static void test_sound_slider_bar() {
    setup_menu(4, 1, 0x0F);
    wb(0x800CE730 + 0x04, 6);
    wb(0x800CE730 + 0x28 + 1, 22);
    SliderBar bar{};
    CHECK(!sound_slider(g_ram.data(), 0, 2340, 1080, &bar));
    CHECK(sound_slider(g_ram.data(), 1, 2340, 1080, &bar));
    CHECK(bar.channel == 1);
    float lx, rx, my, unused;
    n64_to_window(120.0f, 0.0f, 2340, 1080, &lx, &unused);
    n64_to_window(200.0f, 0.0f, 2340, 1080, &rx, &unused);
    my = (bar.y0 + bar.y1) / 2;
    CHECK(slider_value_at(bar, lx) == 0);
    CHECK(slider_value_at(bar, rx) == 128);
    CHECK(std::abs(slider_value_at(bar, (lx + rx) / 2) - 64) <= 1);
    CHECK(slider_value_at(bar, lx - 0.05f) == 0);
    CHECK(bar.y0 < my && my < bar.y1);
    write_volume(g_ram.data(), 1, 77);
    CHECK(read_volume(g_ram.data(), 1) == 77);
    CHECK(g_ram[(0x80130B61 - 0x80000000) ^ 3] == 77);
}

// Pause HUD fixture: slot id -> slot table -> {handleHUD, HUD}; 3 records (title, 2 selectable) drawn as 3 chained elements.
static uint32_t setup_pause(float line1_y, float line2_y) {
    std::fill(g_ram.begin(), g_ram.end(), 0);
    const uint32_t tbl = 0x80300000, obj = 0x80310000, hud = 0x80320000;
    ww(0x8010CA20, 5);
    wh(0x8010BFD0, 1);
    ww(0x80130BB0, tbl);
    ww(tbl + 8, obj);
    ww(obj, 0x800C0084);
    ww(obj + 4, hud);
    wb(hud + 0x258, 3);
    wb(hud + 0xD6A, 2);
    const uint32_t rec = 0x80109E68;
    const uint16_t flags[] = { 0x0000, 0x0001, 0x0001, 0xFFFF };
    for (int i = 0; i < 4; ++i) {
        wh(rec + 6 * i, (int16_t)flags[i]);
    }
    const float ys[] = { -100.0f, line1_y, line2_y };
    for (int i = 0; i < 3; ++i) {
        const uint32_t e = hud + 0x264 + 0x30 * i;
        ww(e, i < 2 ? e + 0x30 : 0x80001234);
        wh(e + 0x08, 8);
        float fx = -50.0f, one = 1.0f;
        uint32_t bits;
        std::memcpy(&bits, &fx, 4);
        ww(e + 0x18, bits);
        std::memcpy(&bits, &ys[i], 4);
        ww(e + 0x1C, bits);
        std::memcpy(&bits, &one, 4);
        ww(e + 0x24, bits);
        ww(e + 0x28, bits);
    }
    return hud;
}

static void test_pause_lines_and_select() {
    const uint32_t hud = setup_pause(0.0f, 20.0f);
    std::vector<PauseLine> lines;
    CHECK(read_pause_lines(g_ram.data(), 2340, 1080, &lines));
    CHECK(lines.size() == 3);
    const PauseLine* second = nullptr;
    for (const PauseLine& l : lines) {
        if (l.selectable == 1) second = &l;
    }
    CHECK(second != nullptr);
    if (!second) return;
    CHECK(pause_tap_select(g_ram.data(), (second->x0 + second->x1) / 2, (second->y0 + second->y1) / 2, 2340, 1080));
    CHECK(g_ram[((hud + 0xD68) & 0x7FFFFF) ^ 2] == 1);
    CHECK(!pause_tap_select(g_ram.data(), 0.02f, 0.02f, 2340, 1080));
}

// Pause rows: pieces of one selectable row merge into one zone; neighbouring zones split at the midpoint (no overlap, no gap).
static void test_pause_rows_merge_and_split() {
    std::vector<PauseLine> lines = {
        { 0, -1, 0.40f, 0.60f, 0.10f, 0.14f },   // title: not a row
        { 1, 0, 0.42f, 0.58f, 0.30f, 0.35f },    // row 0 title
        { 2, 0, 0.38f, 0.62f, 0.34f, 0.40f },    // row 0 slider line (overlaps the next row's top)
        { 3, 1, 0.43f, 0.57f, 0.39f, 0.44f },    // row 1
        { 4, 2, 0.45f, 0.55f, 0.50f, 0.54f },    // row 2 (gap above)
    };
    std::vector<PauseRow> rows = pause_rows(lines);
    CHECK(rows.size() == 3);
    if (rows.size() != 3) return;
    CHECK(rows[0].selectable == 0 && rows[1].selectable == 1 && rows[2].selectable == 2);
    CHECK(NEAR(rows[0].y0, 0.30f));
    CHECK(NEAR(rows[0].y1, rows[1].y0));
    CHECK(NEAR(rows[1].y1, rows[2].y0));
    CHECK(rows[0].x0 < 0.38f && rows[0].x1 > 0.62f);
    CHECK(rows[0].y1 > 0.35f && rows[0].y1 < 0.44f);
}

// Pause text is left-aligned at x with 9.6 game units per glyph; side-by-side rows (YES / NO) keep separate zones.
static void test_pause_side_by_side_rows() {
    std::vector<PauseLine> lines = {
        { 0, 0, 0.35f, 0.45f, 0.70f, 0.74f },   // YES
        { 1, 1, 0.57f, 0.62f, 0.70f, 0.74f },   // NO, same row
    };
    std::vector<PauseRow> rows = pause_rows(lines);
    CHECK(rows.size() == 2);
    if (rows.size() != 2) return;
    CHECK(rows[0].x1 < rows[1].x0);
    CHECK(NEAR(rows[0].y0, 0.70f) && NEAR(rows[1].y0, 0.70f));
}

// A text line alone on its row spans symmetric x..-x (value columns like "STEREO      ON" pad with space);
// two text lines on one row (YES / NO) use their glyph extents.
static void test_pause_line_symmetric_when_alone() {
    setup_pause(0.0f, 20.0f);
    std::vector<PauseLine> lines;
    CHECK(read_pause_lines(g_ram.data(), 2340, 1080, &lines));
    float lx, rx, unused;
    pause_to_window(160.0f - 50.0f * 0.625f, 0.0f, 2340, 1080, &lx, &unused);
    pause_to_window(160.0f + 50.0f * 0.625f, 0.0f, 2340, 1080, &rx, &unused);
    CHECK(lines.size() >= 2 && std::fabs(lines[1].x0 - lx) < 0.002f && std::fabs(lines[1].x1 - rx) < 0.002f);
}

static void test_pause_line_extent() {
    // "YES" (3 glyphs) at x = -80 shares its row with "NO": spans game x -80 .. -51.2.
    setup_pause(0.0f, 0.0f);
    const uint32_t e1 = 0x80320000 + 0x264 + 0x30;
    const uint32_t e2 = e1 + 0x30;
    float fx = -80.0f, fx2 = 61.0f;
    uint32_t bits;
    std::memcpy(&bits, &fx, 4);
    ww(e1 + 0x18, bits);
    wh(e1 + 0x08, 3);
    std::memcpy(&bits, &fx2, 4);
    ww(e2 + 0x18, bits);
    wh(e2 + 0x08, 2);
    std::vector<PauseLine> lines;
    CHECK(read_pause_lines(g_ram.data(), 2340, 1080, &lines));
    float lx, rx, unused;
    pause_to_window(160.0f - 80.0f * 0.625f, 0.0f, 2340, 1080, &lx, &unused);
    pause_to_window(160.0f - 51.2f * 0.625f, 0.0f, 2340, 1080, &rx, &unused);
    CHECK(lines.size() >= 2 && std::fabs(lines[1].x0 - lx) < 0.002f && std::fabs(lines[1].x1 - rx) < 0.002f);
}

// Pause volume bar: fill element scale > 2 spans game x -50..+50; value 0..127 from the finger's x; channel from the record.
static void test_pause_slider_value() {
    PauseLine bar{ 3, 0, 0.40f, 0.60f, 0.40f, 0.44f };
    bar.bar = true;
    bar.channel = 1;
    CHECK(pause_slider_value(bar, 0.40f) == 0);
    CHECK(pause_slider_value(bar, 0.60f) == 127);
    CHECK(std::abs(pause_slider_value(bar, 0.50f) - 64) <= 1);
    CHECK(pause_slider_value(bar, 0.30f) == 0);
}

// Paused for touch/tilt purposes while the pause flag is set or the pause HUD is open or animating, so tilt never drives the pause cursor.
static void test_read_paused_hud_phase() {
    setup_pause(0.0f, 20.0f);
    CHECK(read_paused(g_ram.data()));
    ww(0x8010CA20, 0);
    for (uint8_t phase = 1; phase <= 3; ++phase) {
        wb(0x80320000 + 0x258, phase);
        CHECK(read_paused(g_ram.data()));
    }
    wb(0x80320000 + 0x258, 0);
    CHECK(!read_paused(g_ram.data()));
    ww(0x80310000, 0x80012345);
    wb(0x80320000 + 0x258, 3);
    CHECK(!read_paused(g_ram.data()));
}

static void test_pause_not_ready() {
    setup_pause(0.0f, 20.0f);
    wb(0x80320000 + 0x258, 1);   // still animating in
    std::vector<PauseLine> lines;
    CHECK(!read_pause_lines(g_ram.data(), 2340, 1080, &lines));
    setup_pause(0.0f, 20.0f);
    ww(0x80310000, 0x80012345);  // slot is not the HUD handler
    CHECK(!read_pause_lines(g_ram.data(), 2340, 1080, &lines));
}

// The media hub (menu id 10+) shows the image filled to the window
// width and cropped vertically. Media hub CONCERT HALL (base 40, n=3, scale 0.9): measured text top 0.468 of the window.
static void test_fill_width_screen() {
    setup_menu(3, 0, 0x07);
    wb(0x800CE730 + 0x04, 10);
    wh(0x800CE730 + 0x96, 40);
    wh(0x800CFF50, 10);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(read_menu(g_ram.data(), &s, &bx, 2340, 1080));
    const float pad = g_menu_layout.line_h * 0.2f;
    // Box top includes the 20% padding; convert back to the text top in window fractions.
    const float top = bx.empty() ? -1.0f : bx[0].y0 + pad / 240.0f * (2340.0f / 1.446f) / 1080.0f;
    CHECK(std::fabs(top - 0.468f) < 0.01f);
    const float fill_w = bx.empty() ? 0.0f : bx[0].x1 - bx[0].x0;
    wb(0x800CE730 + 0x04, 2);
    CHECK(read_menu(g_ram.data(), &s, &bx, 2340, 1080));
    const float fit_w = bx.empty() ? 1.0f : bx[0].x1 - bx[0].x0;
    CHECK(std::fabs(fill_w / fit_w - 2.1667f / 1.446f) < 0.02f);
}

// The screen var goes stale outside the pilot flow: after an attract demo the main menu reads 4 and OPTIONS reads 0.
// Menu ids decide instead: fill only for the media hub (id 10+), account screens only under the account menu (id 1).
static void test_stale_screen_var() {
    setup_menu(3, 0, 0x07);
    wb(0x800CE730 + 0x04, 0);
    wh(0x800CFF50, 2);
    MenuSnapshot s{};
    std::vector<Box> fit;
    CHECK(read_menu(g_ram.data(), &s, &fit, 2340, 1080));
    std::vector<Box> stale;
    wh(0x800CFF50, 4);
    CHECK(read_menu(g_ram.data(), &s, &stale, 2340, 1080));
    CHECK(!fit.empty() && !stale.empty() && NEAR(fit[0].x0, stale[0].x0) && NEAR(fit[0].y0, stale[0].y0));
    float x0 = 0.0f, x1 = 1.0f;
    picture_extent(g_ram.data(), 1, 2340, 1080, &x0, &x1);
    CHECK(x0 > 0.1f && x1 < 0.9f);
    wb(0x800CE730 + 0x04, 2);
    wh(0x800CFF50, 0);
    CHECK(account_screen(g_ram.data()) == AccountScreen::NotAccount);
    wh(0x800CFF50, 1);
    CHECK(account_screen(g_ram.data()) == AccountScreen::NotAccount);
    wb(0x800CE730 + 0x04, 10);
    wh(0x800CFF50, 0);
    std::vector<Box> hub;
    CHECK(read_menu(g_ram.data(), &s, &hub, 2340, 1080));
    CHECK(!hub.empty() && (hub[0].x1 - hub[0].x0) > (fit[0].x1 - fit[0].x0) * 1.2f);
}

// Mod-added entries (TOUCH LAYOUT) keep their label on the mod heap above 8 MB; the menu must still yield boxes for all entries.
static void test_heap_label_entry() {
    setup_menu(3, 0, 0x07);
    wb(0x800CE730 + 0x04, 0);
    wstr(0x81000100, "TOUCH LAYOUT");
    ww(0x800CE730 + 0x08 + 4 * 2, 0x81000100);
    MenuSnapshot s{};
    std::vector<Box> bx;
    CHECK(read_menu(g_ram.data(), &s, &bx, 2340, 1080));
    CHECK(bx.size() == 3);
    // A label outside game RAM and the mod heap still means the menu data is garbage.
    ww(0x800CE730 + 0x08 + 4 * 2, 0x00001234);
    CHECK(!read_menu(g_ram.data(), &s, &bx, 2340, 1080));
}

static void test_classify() {
    CHECK(classify(1, "menu.options", false, false) == Context::ListMenu);
    CHECK(classify(1, "menu.main", false, false) == Context::ListMenu);
    CHECK(classify(1, "menu.biographies", false, false) == Context::ListMenu);
    CHECK(classify(1, "menu.elite_rogues", false, false) == Context::ListMenu);
    CHECK(classify(1, "menu.passcodes", false, false) == Context::Wheel);
    CHECK(classify(1, "menu.media", false, false) == Context::ListMenu);
    CHECK(classify(1, "menu.account.level_select", false, false) == Context::Carousel);
    CHECK(classify(1, "mission", false, false) == Context::Carousel);
    CHECK(classify(0, "mission", false, false) == Context::Flight);
    CHECK(classify(0, "mission", true, false) == Context::PauseMenu);
    CHECK(classify(0, "attract.demo", false, true) == Context::Cutscene);
    CHECK(classify(2, "cinematic", false, false) == Context::Cutscene);
    CHECK(classify(-1, "unknown", false, false) == Context::Cutscene);
    CHECK(classify(1, nullptr, false, false) == Context::Carousel);
}

static void test_config_roundtrip() {
    const std::string p = "touch_cfg_test.json";
    std::remove(p.c_str());
    Config c = load_config(p);
    CHECK(NEAR(c.opacity, 0.45f) && !c.gyro);
    c.gyro = true;
    c.steer_full_deg = 30.0f;
    c.pitch_gain = 0.6f;
    c.steer_curve = 1.2f;
    c.rate_assist = 0.4f;
    CHECK(save_config(c, p));
    Config d = load_config(p);
    CHECK(d.gyro && NEAR(d.steer_full_deg, 30.0f) && NEAR(d.pitch_gain, 0.6f) && NEAR(d.steer_curve, 1.2f) && NEAR(d.rate_assist, 0.4f));
    std::remove(p.c_str());
}

int main() {
    test_config_roundtrip();
    test_hit_and_select();
    test_miss_and_inactive();
    test_boxes_do_not_overlap();
    test_bad_menu_rejected();
    test_no_write_on_menu_change();
    test_hitbox_widescreen();
    test_classify();
    test_fill_width_screen();
    test_stale_screen_var();
    test_heap_label_entry();
    test_main_menu_calibration();
    test_controller_settings_back();
    test_select_game_slots();
    test_level_select_arrows();
    test_account_screen();
    test_craft_select_taps();
    test_carousel_corner_arrows();
    test_pause_lines_and_select();
    test_pause_not_ready();
    test_read_paused_hud_phase();
    test_pause_rows_merge_and_split();
    test_pause_side_by_side_rows();
    test_pause_line_extent();
    test_pause_line_symmetric_when_alone();
    test_pause_slider_value();
    test_sound_slider_bar();
    test_gyro_off_is_neutral();
    test_gyro_only_in_flight();
    test_stick_center_deflect_release();
    test_stick_defaults();
    test_menu_back_button();
    test_stick_dead_zone();
    test_two_fingers_stick_and_button();
    test_slide_between_buttons();
    test_stick_finger_never_presses_buttons();
    test_carousel_swipe_one_pulse();
    test_tap_vs_slow_press();
    test_wheel_drag_is_analog();
    test_wheel_side_and_center_taps();
    test_wheel_side_hold();
    test_list_horizontal_fine_drag();
    test_list_vertical_drag_steps();
    test_pause_drag_scrubs_and_tap_confirms();
    test_pause_touch_highlights_directly();
    test_list_tap_calls_menu_and_pulses_a();
    test_carousel_tap_offers_menu_then_a();
    test_taps_outside_picture_ignored();
    test_carousel_tap_direction();
    test_inject_one_poll();
    test_fine_nudge_is_one_poll();
    test_cutscene_tap_is_start_and_back_is_b();
    test_arbitration();
    test_disabled_does_nothing();
    test_menu_corner_buttons_padded();
    test_tilt_angles_from_gravity();
    test_tilt_steers_x();
    test_pitch_follows_motion();
    test_steer_rate_assist();
    test_layout_override_moves_and_scales();
    test_stick_split();
    test_gear_only_in_menus_opens_editor();
    test_edit_drag_moves_button_and_blocks_input();
    test_edit_plus_minus_scale_selected();
    test_edit_pinch_scales();
    test_edit_split_drag();
    test_edit_reset_and_done();
    test_edit_back_finishes();
    test_config_layout_roundtrip();
    if (g_fail) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::puts("PASS");
    return 0;
}
