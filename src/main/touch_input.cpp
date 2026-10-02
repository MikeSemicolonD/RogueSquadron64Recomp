#include "touch_input.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace rs64::touch {
    namespace {
        constexpr uint32_t kTapMs = 250;
        constexpr float kTapMove = 0.03f;
        constexpr uint32_t kSwipeMs = 400;
        constexpr float kSwipeMove = 0.08f;
        constexpr float kScrubStep = 0.05f;
        constexpr float kFineStep = 0.015f;
        constexpr size_t kMaxPending = 8;
        // Passcode wheel: band of the letters (window y), center tap zone half-width, repeat timing.
        constexpr float kWheelTop = 0.50f;
        constexpr float kWheelBottom = 0.65f;
        constexpr float kWheelCenterHalf = 0.08f;
        constexpr float kWheelFullDrag = 0.20f;
        constexpr int kWheelTapPolls = 8;
        constexpr float kDeadZone = 0.08f;
        constexpr float kCornerPad = 2.0f;
        // Tilt steering needs at least this share of gravity in the screen plane (about 17 degrees off flat).
        constexpr float kWheelMinPlane = 0.3f;
        // Gyro rates under this (rad/s) are sensor noise, not aim.
        constexpr float kRateFloor = 0.03f;
        // Layout editor limits.
        constexpr float kMinScale = 0.5f;
        constexpr float kMaxScale = 2.0f;
        constexpr float kScaleStep = 1.1f;
        constexpr float kSplitGrab = 0.02f;
        constexpr float kMinSplit = 0.2f;
        constexpr float kMaxSplit = 0.8f;
    }

    void Engine::set_config(const Config& c) {
        cfg_ = c;
    }

    const Config& Engine::config() const {
        return cfg_;
    }

    void Engine::set_screen(float w, float h) {
        if (w > 0.0f && h > 0.0f) {
            aspect_ = w / h;
        }
    }

    void Engine::set_picture(float x0, float x1) {
        if (x1 > x0) {
            pic_x0_ = x0;
            pic_x1_ = x1;
        }
    }

    float Engine::dist(float dx, float dy) const {
        const float ax = dx * aspect_;
        return std::sqrt(ax * ax + dy * dy);
    }

    // Flight: right-hand arc, moved/resized by config layout; menus: B, Start and the layout-editor gear. Radii scale with button_scale.
    std::vector<ButtonDef> Engine::layout(Context ctx) const {
        const float s = cfg_.button_scale;
        if (ctx == Context::Flight || editing_) {
            std::vector<ButtonDef> v = {
                { A, 0.86f, 0.70f, 0.085f * s, "A", "A" },
                { B, 0.76f, 0.83f, 0.065f * s, "B", "B" },
                { Z, 0.73f, 0.60f, 0.060f * s, "Z", "Z" },
                { R, 0.93f, 0.28f, 0.060f * s, "R", "R" },
                { CU, 0.84f, 0.36f, 0.040f * s, "C^", "CU" },
                { CD, 0.84f, 0.50f, 0.040f * s, "Cv", "CD" },
                { CL, 0.80f, 0.43f, 0.040f * s, "C<", "CL" },
                { CR, 0.88f, 0.43f, 0.040f * s, "C>", "CR" },
                { START, 0.50f, 0.07f, 0.045f * s, "START", "START" },
            };
            for (ButtonDef& b : v) {
                auto it = cfg_.layout.find(b.id);
                if (it != cfg_.layout.end()) {
                    b.cx = it->second.cx;
                    b.cy = it->second.cy;
                    b.r *= it->second.scale;
                }
            }
            return v;
        }
        if (ctx == Context::ListMenu || ctx == Context::Carousel || ctx == Context::PauseMenu || ctx == Context::Wheel) {
            return {
                { B, 0.06f, 0.08f, 0.045f * s, "B", "MENU_B", kCornerPad },
                { START, 0.94f, 0.08f, 0.045f * s, "START", "MENU_START", kCornerPad },
                { 0, 0.06f, 0.92f, 0.040f * s, "", "GEAR", kCornerPad },
            };
        }
        return {};
    }

    std::vector<ButtonDef> Engine::tools() const {
        return {
            { 0, 0.34f, 0.92f, 0.045f, "-", "MINUS" },
            { 0, 0.43f, 0.92f, 0.045f, "+", "PLUS" },
            { 0, 0.57f, 0.92f, 0.045f, "RESET", "RESET" },
            { 0, 0.66f, 0.92f, 0.045f, "DONE", "DONE" },
        };
    }

    bool Engine::hit_button(Context ctx, float x, float y, uint16_t* mask) const {
        for (const ButtonDef& b : layout(ctx)) {
            if (b.mask != 0 && dist(x - b.cx, y - b.cy) <= b.r * b.hit) {
                *mask = b.mask;
                return true;
            }
        }
        return false;
    }

    // Pulses queued while others are pending start after them plus a gap, so the game sees separate presses.
    void Engine::queue(uint16_t buttons, float x, float y, int polls, int gap) {
        int busy = 0;
        for (const Pulse& q : pulses_) {
            busy = std::max(busy, q.delay + q.polls);
        }
        pulses_.push_back({ buttons, x, y, polls, busy ? busy + gap : 0 });
    }

    void Engine::inject(uint16_t buttons, float x, float y, int polls, int gap) {
        queue(buttons, x, y, polls, gap);
    }

    void Engine::apply_tap_action(TapAction a) {
        if (a == TapAction::Left) {
            queue(0, -1.0f, 0.0f);
        } else if (a == TapAction::Right) {
            queue(0, 1.0f, 0.0f);
        } else if (a == TapAction::Confirm) {
            queue(A, 0.0f, 0.0f);
        } else if (a == TapAction::CButton) {
            queue(CD, 0.0f, 0.0f);
        }
    }

    void Engine::finger(int64_t id, FingerEvent ev, float x, float y, uint32_t ms) {
        if (!cfg_.enabled) {
            return;
        }
        active_ = true;
        if (editing_) {
            edit_finger(id, ev, x, y, ms);
            return;
        }
        const Context ctx = last_ctx_;
        if (ev == FingerEvent::Down) {
            Finger f{ Finger::Role::Gesture, x, y, x, y, ms, 0, false, 0, 0 };
            uint16_t mask = 0;
            bool gear = false;
            for (const ButtonDef& b : layout(ctx)) {
                gear = gear || (b.id && std::string_view(b.id) == "GEAR" && dist(x - b.cx, y - b.cy) <= b.r * b.hit);
            }
            if (gear) {
                begin_edit();
                return;
            }
            if (hit_button(ctx, x, y, &mask)) {
                f.role = Finger::Role::Button;
                f.button = mask;
            } else if (ctx != Context::Flight && (x < pic_x0_ || x > pic_x1_)) {
                // Side bars beside the game picture: a missed B/START press must not confirm or swipe anything.
                f.role = Finger::Role::Ignored;
            } else if (ctx == Context::Flight && x < cfg_.stick_split && stick_id_ < 0) {
                f.role = Finger::Role::Stick;
                stick_id_ = id;
            } else if (ctx == Context::Wheel && y >= kWheelTop && y <= kWheelBottom && (x < 0.5f - kWheelCenterHalf || x > 0.5f + kWheelCenterHalf)) {
                // A touch on a wheel side holds the stick that way while down; a quick tap turns one letter (see Up). Never confirms.
                f.side = (x < 0.5f) ? -1 : 1;
                f.gesture_fired = true;
            } else if (ctx == Context::PauseMenu && menu_hover_) {
                f.row0 = menu_hover_(x, y);
            }
            fingers_[id] = f;
            return;
        }
        auto it = fingers_.find(id);
        if (it == fingers_.end()) {
            return;
        }
        Finger& f = it->second;
        if (f.role == Finger::Role::Ignored) {
            if (ev == FingerEvent::Up) {
                fingers_.erase(it);
            }
            return;
        }
        f.x = x;
        f.y = y;
        if (ev == FingerEvent::Move) {
            if (f.role == Finger::Role::Button) {
                uint16_t mask = 0;
                f.button = hit_button(ctx, x, y, &mask) ? mask : 0;
            } else if (f.role == Finger::Role::Gesture && ctx == Context::Wheel) {
                // Passcode wheel: a horizontal drag becomes a held joystick; wheel_repeat() steps it from poll().
                const float dx = (x - f.x0) * aspect_;
                const float dy = y - f.y0;
                if (f.axis == 0 && std::fabs(dx) > kTapMove && std::fabs(dx) >= 2.0f * std::fabs(dy)) {
                    f.axis = 1;
                    f.side = 0;
                    f.gesture_fired = true;
                }
            } else if (f.role == Finger::Role::Gesture && (ctx == Context::ListMenu || ctx == Context::PauseMenu)) {
                // Horizontal: fine, two-way slider nudges (one step per kFineStep of drag). Vertical: coarse steps between entries.
                const float dx = (x - f.x0) * aspect_;
                const float dy = y - f.y0;
                const bool hover = ctx == Context::PauseMenu && menu_hover_;
                if (f.axis == 0) {
                    if (std::fabs(dx) > kTapMove && std::fabs(dx) >= 2.0f * std::fabs(dy)) {
                        f.axis = 1;
                    } else if (!hover && std::fabs(dy) > kTapMove && std::fabs(dy) >= 2.0f * std::fabs(dx)) {
                        f.axis = 2;
                    }
                }
                if (hover && f.axis != 1) {
                    // Pause lines follow the finger directly; stick pulses here were dropped or doubled by the game's repeat timing.
                    menu_hover_(x, y);
                    return;
                }
                if (f.axis == 1) {
                    const int want = (int)(dx / kFineStep);
                    while (f.steps != want && pulses_.size() < kMaxPending) {
                        const int dir = (want > f.steps) ? 1 : -1;
                        queue(0, (float)dir, 0.0f, 1, 1);
                        f.steps += dir;
                    }
                    f.gesture_fired = true;
                } else if (f.axis == 2) {
                    const int want = (std::fabs(dy) > kSwipeMove) ? 1 + (int)((std::fabs(dy) - kSwipeMove) / kScrubStep) : 0;
                    while (f.steps < want && pulses_.size() < kMaxPending) {
                        queue(0, 0.0f, dy < 0.0f ? 1.0f : -1.0f);
                        ++f.steps;
                    }
                    f.gesture_fired = true;
                }
            } else if (f.role == Finger::Role::Gesture && !f.gesture_fired && (ms - f.t0) <= kSwipeMs) {
                const float dx = (x - f.x0) * aspect_;
                const float dy = y - f.y0;
                if (std::fabs(dx) > kSwipeMove && std::fabs(dx) >= 2.0f * std::fabs(dy)) {
                    queue(0, dx > 0.0f ? 1.0f : -1.0f, 0.0f);
                    f.gesture_fired = true;
                } else if (std::fabs(dy) > kSwipeMove && std::fabs(dy) >= 2.0f * std::fabs(dx)) {
                    queue(0, 0.0f, dy < 0.0f ? 1.0f : -1.0f);
                    f.gesture_fired = true;
                }
            }
            return;
        }
        // A quick side tap on the wheel holds the stick long enough to turn one letter.
        if (f.side != 0 && f.axis == 0 && (ms - f.t0) < kTapMs) {
            queue(0, (float)f.side, 0.0f, kWheelTapPolls, 1);
        }
        const bool tap = f.role == Finger::Role::Gesture && !f.gesture_fired && (ms - f.t0) < kTapMs && dist(x - f.x0, y - f.y0) < kTapMove;
        if (last_ctx_ == Context::PauseMenu && menu_hover_ && f.role == Finger::Role::Gesture) {
            // Release on the line the press started on selects it, however long the press; a slide to another line only highlights.
            if (f.axis != 1 && f.row0 >= 0 && menu_hover_(x, y) == f.row0) {
                apply_tap_action(menu_tap_ ? menu_tap_(x, y) : TapAction::None);
            }
        } else if (tap) {
            if (ctx == Context::Cutscene) {
                queue(START, 0.0f, 0.0f);
            } else if (ctx == Context::ListMenu) {
                apply_tap_action(menu_tap_ ? menu_tap_(x, y) : TapAction::None);
            } else if (ctx == Context::Wheel) {
                // A letter tap queues its own steps (Handled); ENTER CODE/BACK select; anything else enters the center letter.
                const TapAction a = menu_tap_ ? menu_tap_(x, y) : TapAction::None;
                apply_tap_action(a == TapAction::None ? TapAction::Confirm : a);
            } else if (ctx == Context::Carousel) {
                // The menu layer may select a tapped entry or map it to a step (side save slot); otherwise A.
                const TapAction a = menu_tap_ ? menu_tap_(x, y) : TapAction::None;
                apply_tap_action(a == TapAction::None ? TapAction::Confirm : a);
            } else if (ctx == Context::PauseMenu) {
                // Only a tap on a pause line confirms; a miss must not confirm the highlighted line (CONTINUE unpauses).
                apply_tap_action(menu_tap_ ? menu_tap_(x, y) : TapAction::None);
            }
        }
        if (id == stick_id_) {
            stick_id_ = -1;
        }
        fingers_.erase(it);
    }

    void Engine::physical_input() {
        active_ = false;
    }

    void Engine::back_pressed() {
        if (editing_) {
            end_edit();
            return;
        }
        if (cfg_.enabled) {
            queue(B, 0.0f, 0.0f);
        }
    }


    void Engine::tilt(float roll, bool valid) {
        roll_ = roll;
        roll_valid_ = valid;
    }

    void Engine::gyro_rate(float roll_rate, float pitch_rate) {
        roll_rate_ = roll_rate;
        pitch_rate_ = pitch_rate;
    }

    bool tilt_angles(float ax, float ay, float az, bool flipped, float* roll) {
        // Landscape: screen up = device +x and screen right = device -y; flipped landscape negates both.
        const float s = flipped ? -1.0f : 1.0f;
        const float up = s * ax;
        const float right = -s * ay;
        const float in_plane = std::sqrt(up * up + right * right);
        const float total = std::sqrt(ax * ax + ay * ay + az * az);
        *roll = std::atan2(-right, up);
        return total > 0.0f && in_plane >= kWheelMinPlane * total;
    }

    void Engine::set_menu_tap(MenuTapFn fn) {
        menu_tap_ = std::move(fn);
    }

    void Engine::set_menu_hover(MenuHoverFn fn) {
        menu_hover_ = std::move(fn);
    }

    bool Engine::active() const {
        return cfg_.enabled && active_;
    }

    // Passcode wheel: the game spins it while the stick is held and snaps to a letter on release, so a drag holds the stick
    // proportionally to its distance from the start (full at kWheelFullDrag) and a held side holds it fully.
    float Engine::wheel_stick() const {
        float best = 0.0f;
        for (const auto& [id, f] : fingers_) {
            if (f.role != Finger::Role::Gesture) {
                continue;
            }
            float v = 0.0f;
            if (f.axis == 1) {
                const float dx = (f.x - f.x0) * aspect_;
                const float t = std::clamp((std::fabs(dx) - kTapMove) / (kWheelFullDrag - kTapMove), 0.0f, 1.0f);
                v = (dx > 0.0f) ? t : -t;
            } else if (f.side != 0) {
                v = (float)f.side;
            }
            if (std::fabs(v) > std::fabs(best)) {
                best = v;
            }
        }
        return best;
    }

    Pad Engine::poll(Context ctx, uint32_t ms) {
        last_ctx_ = ctx;
        Pad p;
        if (!cfg_.enabled || editing_) {
            return p;
        }
        if (ctx == Context::Wheel) {
            p.x = wheel_stick();
        }
        for (const auto& [id, f] : fingers_) {
            if (f.role == Finger::Role::Button) {
                p.buttons |= f.button;
            }
        }
        if (ctx == Context::Flight && stick_id_ >= 0) {
            const Finger& f = fingers_.at(stick_id_);
            float sx = (f.x - f.x0) * aspect_ / cfg_.stick_radius;
            float sy = (f.y0 - f.y) / cfg_.stick_radius;
            // Radial dead zone, then rescale the rest to 0..1 and apply the response curve.
            const float m = std::sqrt(sx * sx + sy * sy);
            if (m < kDeadZone) {
                sx = 0.0f;
                sy = 0.0f;
            } else {
                const float t = std::min((m - kDeadZone) / (1.0f - kDeadZone), 1.0f);
                const float out = std::pow(t, std::max(cfg_.stick_curve, 0.1f));
                sx = sx / m * out;
                sy = sy / m * out;
            }
            p.x = sx;
            p.y = sy;
        }
        if (ctx == Context::Flight && cfg_.gyro) {
            // Steering: held roll sets the stick (dead zone, curve), and roll motion adds an immediate nudge.
            float gx = 0.0f;
            if (roll_valid_) {
                const float deg = std::fabs(roll_) * 180.0f / 3.14159265f;
                const float span = std::max(cfg_.steer_full_deg - cfg_.steer_dead_deg, 1.0f);
                const float t = std::pow(std::clamp((deg - cfg_.steer_dead_deg) / span, 0.0f, 1.0f), std::max(cfg_.steer_curve, 0.1f));
                gx = (roll_ < 0.0f) ? -t : t;
            }
            if (std::fabs(roll_rate_) >= kRateFloor) {
                gx += roll_rate_ * cfg_.rate_assist;
            }
            // Up/down is mouse-like: the stick follows pitch motion and centers when the phone stops, so a held angle never overshoots.
            float gy = 0.0f;
            if (std::fabs(pitch_rate_) >= kRateFloor) {
                const float out = cfg_.pitch_min + cfg_.pitch_gain * std::fabs(pitch_rate_);
                gy = (pitch_rate_ < 0.0f) ? -out : out;
            }
            gx = std::clamp(gx, -1.0f, 1.0f);
            gy = std::clamp(gy, -1.0f, 1.0f);
            if ((gx * gx + gy * gy) > (p.x * p.x + p.y * p.y)) {
                p.x = gx;
                p.y = gy;
            }
        }
        for (Pulse& q : pulses_) {
            if (q.delay > 0) {
                --q.delay;
                continue;
            }
            p.buttons |= q.buttons;
            if (std::fabs(q.x) > std::fabs(p.x)) p.x = q.x;
            if (std::fabs(q.y) > std::fabs(p.y)) p.y = q.y;
            --q.polls;
        }
        pulses_.erase(std::remove_if(pulses_.begin(), pulses_.end(), [](const Pulse& q) { return q.polls <= 0; }), pulses_.end());
        p.has_input = active_ || p.buttons != 0 || p.x != 0.0f || p.y != 0.0f;
        return p;
    }

    OverlayState Engine::draw_state(Context ctx) const {
        OverlayState d;
        d.stick_split = cfg_.stick_split;
        if (editing_) {
            d.visible = true;
            d.editing = true;
            d.selected = selected_;
            d.buttons = layout(Context::Flight);
            d.tools = tools();
            d.stick_r = cfg_.stick_radius;
            return d;
        }
        d.visible = active() && ctx != Context::None && ctx != Context::Cutscene;
        d.buttons = layout(ctx);
        d.stick_r = cfg_.stick_radius;
        for (const auto& [id, f] : fingers_) {
            if (f.role == Finger::Role::Button) {
                d.pressed |= f.button;
            }
        }
        if (ctx == Context::Flight && stick_id_ >= 0) {
            const Finger& f = fingers_.at(stick_id_);
            d.stick_down = true;
            d.base_x = f.x0;
            d.base_y = f.y0;
            d.knob_x = f.x;
            d.knob_y = f.y;
        }
        return d;
    }

    void Engine::begin_edit() {
        editing_ = true;
        active_ = true;
        selected_ = 0;
        fingers_.clear();
        edit_fingers_.clear();
        pulses_.clear();
        stick_id_ = -1;
    }

    bool Engine::editing() const {
        return editing_;
    }

    bool Engine::take_layout_saved() {
        const bool s = layout_saved_;
        layout_saved_ = false;
        return s;
    }

    void Engine::end_edit() {
        editing_ = false;
        layout_saved_ = true;
        selected_ = 0;
        edit_fingers_.clear();
    }

    // Current placement of a flight button (its override, else its default with scale 1).
    bool Engine::placement(uint16_t mask, std::string* id, Placement* p) const {
        for (const ButtonDef& b : layout(Context::Flight)) {
            if (b.mask == mask) {
                *id = b.id;
                auto it = cfg_.layout.find(b.id);
                *p = { b.cx, b.cy, it != cfg_.layout.end() ? it->second.scale : 1.0f };
                return true;
            }
        }
        return false;
    }

    void Engine::set_scale(uint16_t mask, float scale) {
        std::string id;
        Placement p;
        if (placement(mask, &id, &p)) {
            p.scale = std::clamp(scale, kMinScale, kMaxScale);
            cfg_.layout[id] = p;
        }
    }

    void Engine::edit_finger(int64_t id, FingerEvent ev, float x, float y, uint32_t ms) {
        if (ev == FingerEvent::Down) {
            EditFinger f{ x, y, x, y, ms, 0, 0.0f, 0.0f, false };
            // A second finger while one drags a button pinches that button.
            for (const auto& [oid, o] : edit_fingers_) {
                if (o.target != 0) {
                    std::string bid;
                    Placement p;
                    placement(o.target, &bid, &p);
                    pinch_d0_ = std::max(dist(x - o.x, y - o.y), 0.01f);
                    pinch_s0_ = p.scale;
                    edit_fingers_[id] = f;
                    return;
                }
            }
            for (const ButtonDef& t : tools()) {
                if (dist(x - t.cx, y - t.cy) <= t.r) {
                    edit_fingers_[id] = f;
                    return;
                }
            }
            uint16_t mask = 0;
            if (hit_button(Context::Flight, x, y, &mask)) {
                std::string bid;
                Placement p;
                placement(mask, &bid, &p);
                f.target = mask;
                f.cx0 = p.cx;
                f.cy0 = p.cy;
                selected_ = mask;
            } else if (std::fabs(x - cfg_.stick_split) < kSplitGrab) {
                f.split = true;
            } else {
                selected_ = 0;
            }
            edit_fingers_[id] = f;
            return;
        }
        auto it = edit_fingers_.find(id);
        if (it == edit_fingers_.end()) {
            return;
        }
        EditFinger& f = it->second;
        f.x = x;
        f.y = y;
        if (ev == FingerEvent::Move) {
            const EditFinger* dragger = nullptr;
            const EditFinger* other = nullptr;
            for (const auto& [oid, o] : edit_fingers_) {
                if (o.target != 0 && !dragger) {
                    dragger = &o;
                } else {
                    other = &o;
                }
            }
            if (dragger && other) {
                set_scale(dragger->target, pinch_s0_ * dist(other->x - dragger->x, other->y - dragger->y) / pinch_d0_);
            } else if (f.target != 0) {
                std::string bid;
                Placement p;
                placement(f.target, &bid, &p);
                p.cx = std::clamp(f.cx0 + (x - f.x0), 0.0f, 1.0f);
                p.cy = std::clamp(f.cy0 + (y - f.y0), 0.0f, 1.0f);
                cfg_.layout[bid] = p;
            } else if (f.split) {
                cfg_.stick_split = std::clamp(x, kMinSplit, kMaxSplit);
            }
            return;
        }
        const bool tap = (ms - f.t0) < kTapMs * 2 && dist(x - f.x0, y - f.y0) < kTapMove;
        const float tx = f.x0;
        const float ty = f.y0;
        edit_fingers_.erase(it);
        if (!tap) {
            return;
        }
        for (const ButtonDef& t : tools()) {
            if (dist(tx - t.cx, ty - t.cy) > t.r) {
                continue;
            }
            const std::string_view tid = t.id;
            std::string bid;
            Placement p;
            if (tid == "DONE") {
                end_edit();
            } else if (tid == "RESET") {
                cfg_.layout.clear();
                cfg_.stick_split = 0.5f;
            } else if (selected_ != 0 && placement(selected_, &bid, &p)) {
                set_scale(selected_, p.scale * (tid == "PLUS" ? kScaleStep : 1.0f / kScaleStep));
            }
            return;
        }
    }

    void merge(const Pad& t, uint16_t* buttons, float* x, float* y) {
        *buttons |= t.buttons;
        if ((t.x * t.x + t.y * t.y) > ((*x) * (*x) + (*y) * (*y))) {
            *x = t.x;
            *y = t.y;
        }
    }
}
