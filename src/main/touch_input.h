#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Touch engine: finger/gyro/back events in, one N64 pad state per game poll out. No SDL/ImGui/RDRAM here.
// Coordinates are 0..1 of the window; distances and radii are in units of screen height.
namespace rs64::touch {
    enum class Context { None, Flight, ListMenu, Carousel, PauseMenu, Wheel, Cutscene };
    enum class FingerEvent { Down, Move, Up };

    // A moved/resized flight button; scale multiplies its default radius.
    struct Placement { float cx, cy, scale; };

    struct Config {
        bool enabled = false;
        float opacity = 0.45f;
        float button_scale = 1.0f;
        float stick_radius = 0.18f;   // drag distance (screen heights) to full deflection
        float stick_curve = 1.5f;     // response exponent; >1 is gentler near center
        bool gyro = false;              // tilt flying (GYRO STEERING toggle)
        float steer_dead_deg = 5.0f;    // roll under this flies straight
        float steer_full_deg = 40.0f;   // roll for a full left/right stick
        float steer_curve = 1.5f;       // response exponent; >1 is gentler near center
        float rate_assist = 0.3f;       // steering: stick added per rad/s of roll motion, so corrections answer at once
        float pitch_gain = 0.8f;        // up/down (mouse-like): stick per rad/s of pitch motion
        float pitch_min = 0.08f;        // up/down: smallest output once moving, to clear the game's stick dead zone
        bool debug_hitboxes = false;
        std::map<std::string, Placement> layout;   // flight button id -> placement; missing ids keep their default
        float stick_split = 0.5f;                  // flight: touches left of this window x drive the stick
    };

    struct Pad {
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
        bool has_input = false;
    };

    // hit scales r for touch testing only (padding around small corner buttons).
    struct ButtonDef { uint16_t mask; float cx, cy, r; const char* label; const char* id = nullptr; float hit = 1.0f; };

    struct OverlayState {
        bool visible = false;
        bool stick_down = false;
        float base_x = 0, base_y = 0, knob_x = 0, knob_y = 0, stick_r = 0;
        std::vector<ButtonDef> buttons;
        uint16_t pressed = 0;
        bool editing = false;
        uint16_t selected = 0;           // layout editor: mask of the selected button
        std::vector<ButtonDef> tools;    // layout editor toolbar (ids MINUS, PLUS, RESET, DONE)
        float stick_split = 0.5f;
    };

    // What a menu tap means: Confirm = an entry was selected (press A), Left/Right = step that way, None = no hit.
    // Handled = the menu layer already queued its own presses (e.g. several wheel steps).
    enum class TapAction { None, Confirm, Left, Right, CButton, Handled };
    // Called on ListMenu and Carousel taps with normalized window coords.
    using MenuTapFn = std::function<TapAction(float x, float y)>;
    // Pause menu: highlights the line under a finger and returns its row (-1 = none).
    using MenuHoverFn = std::function<int(float x, float y)>;

    class Engine {
    public:
        void set_config(const Config& c);
        const Config& config() const;
        void set_screen(float width_px, float height_px);
        // Horizontal extent of the game picture (window fractions). Outside it (the side bars) only overlay buttons respond.
        void set_picture(float x0, float x1);
        void finger(int64_t id, FingerEvent ev, float x, float y, uint32_t ms);
        void physical_input();
        void back_pressed();
        // Steering roll from tilt_angles() (radians, + = clockwise); !valid (phone too flat) holds the steering centered.
        void tilt(float roll, bool valid);
        // Gyro rates (rad/s): clockwise turn (steering assist) and top edge tipping away (mouse-like up/down).
        void gyro_rate(float roll_rate, float pitch_rate);
        void set_menu_tap(MenuTapFn fn);
        // With a hover handler, pause-menu fingers highlight lines directly; release on the starting line selects it.
        void set_menu_hover(MenuHoverFn fn);
        // Queues a host-driven press lasting `polls` game polls, after any pending presses.
        void inject(uint16_t buttons, float x, float y, int polls, int gap = 1);
        Pad poll(Context ctx, uint32_t ms);
        bool active() const;
        OverlayState draw_state(Context ctx) const;
        // Layout editor: flight buttons are dragged/pinched instead of pressed; game input is suppressed until Done/Back.
        void begin_edit();
        bool editing() const;
        // True once after the editor closes; the host then persists config().layout.
        bool take_layout_saved();

    private:
        struct Finger {
            enum class Role { Stick, Button, Gesture, Ignored } role;
            float x0, y0, x, y;
            uint32_t t0;
            uint16_t button;
            bool gesture_fired;
            int axis;    // scrub axis once locked: 0 none, 1 x, 2 y
            int steps;   // scrub steps already emitted
            int side = 0;   // passcode wheel: -1/+1 while holding a side, 0 otherwise
            int row0 = -1;  // pause menu: line under the finger at Down
        };
        float wheel_stick() const;
        struct EditFinger { float x0, y0, x, y; uint32_t t0; uint16_t target; float cx0, cy0; bool split; };
        void edit_finger(int64_t id, FingerEvent ev, float x, float y, uint32_t ms);
        void end_edit();
        std::vector<ButtonDef> tools() const;
        void set_scale(uint16_t mask, float scale);
        bool placement(uint16_t mask, std::string* id, Placement* p) const;
        struct Pulse { uint16_t buttons; float x, y; int polls; int delay; };

        std::vector<ButtonDef> layout(Context ctx) const;
        bool hit_button(Context ctx, float x, float y, uint16_t* mask) const;
        float dist(float dx, float dy) const;
        void queue(uint16_t buttons, float x, float y, int polls = 3, int gap = 2);
        void apply_tap_action(TapAction a);

        Config cfg_;
        float aspect_ = 16.0f / 9.0f;
        float pic_x0_ = 0.0f;
        float pic_x1_ = 1.0f;
        bool active_ = false;
        Context last_ctx_ = Context::None;
        std::unordered_map<int64_t, Finger> fingers_;
        std::vector<Pulse> pulses_;
        int64_t stick_id_ = -1;
        float roll_ = 0.0f;
        bool roll_valid_ = false;
        float roll_rate_ = 0.0f;
        float pitch_rate_ = 0.0f;
        MenuTapFn menu_tap_;
        MenuHoverFn menu_hover_;
        bool editing_ = false;
        bool layout_saved_ = false;
        uint16_t selected_ = 0;
        std::unordered_map<int64_t, EditFinger> edit_fingers_;
        float pinch_d0_ = 0.0f;
        float pinch_s0_ = 1.0f;
    };

    // Roll of a landscape screen from the accelerometer (device axes, portrait-natural): turn in the screen plane, + = clockwise.
    // Returns false when the phone lies too flat to read it.
    bool tilt_angles(float ax, float ay, float az, bool flipped, float* roll);

    // Merges touch into a physical result: buttons OR, stick = larger magnitude.
    void merge(const Pad& touch, uint16_t* buttons, float* x, float* y);

    // N64 button masks (mirror of main.cpp).
    constexpr uint16_t A = 0x8000, B = 0x4000, Z = 0x2000, START = 0x1000;
    constexpr uint16_t DU = 0x0800, DD = 0x0400, DL = 0x0200, DR = 0x0100;
    constexpr uint16_t L = 0x0020, R = 0x0010, CU = 0x0008, CD = 0x0004, CL = 0x0002, CR = 0x0001;
}
