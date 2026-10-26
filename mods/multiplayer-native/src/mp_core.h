#pragma once
// Multiplayer-only logic (local and online co-op): the imposter craft and its flight model, the ghost puppet, and the lobby, HUD, lives, skip and trigger rules. Built into the multiplayer code only; base code uses lockstep_core.h.
#include "lockstep/lockstep_core.h"

namespace rs64::ls {

// Port 1's pad record from a recorded port-0 frame: START masked (player 2 cannot pause), connected byte set; nullptr = idle pad.
void pad2_bytes(const FrameInput* f, uint8_t out[6]);

// Co-op imposter (player 2 as an allied NPC craft), flown by a port of the player X-wing's handling
// (updatePlayerCraftPhysics 0x800B1BE8 / updateXwingFlightControls 0x800B4588). World Y points down.
struct Vec3 { float x = 0, y = 0, z = 0; };
// fwd and down are the craft's body forward/down axes (the wingman block's +0x0C/+0x18); right = down x fwd.
struct ImposterCraft {
    Vec3 pos, fwd, down;
    float speed = 2.625f;
    float target = 2.625f;
    float boost_hold = 0.0f;
    bool level_latch = false;
    // S-foils: position 0 (open) .. 1 (closed, attack position) and the closed flag; closed = 1.5x speed, stiffer turns.
    float foil_t = 0.0f;
    bool foil_closed = false;
};
// Raw port-1 stick plus the control-map buttons: fire (B), boost (A), brake (Z), freeze rotation (C-up), S-foil toggle (C-right).
struct ImposterPad { int8_t stick_x = 0; int8_t stick_y = 0; bool fire = false; bool boost = false; bool brake = false; bool freeze = false; bool foil = false; };
// settings = the flags word (bit0 auto-level, bit1 auto-roll, bit2 invert pitch, bit6 linear stick); ground/ceiling in world Y.
struct ImposterWorld { uint32_t settings = 0x3; uint32_t level = 0; float ground_y = 1e9f; float ceiling_y = -1e9f; };
constexpr uint32_t kImposterRec = 0x80B40000u;

// The per-player neighbour data instruction_patches.json moves to host RAM so index 1 fits; original addresses, end-exclusive. R4 (0x80138930, a per-view 4 x 0xF8 block) has its own delta so its index-1 entry clears camera node[1]. tools/gen_relocation.py repeats these as MOVED.
constexpr MovedRange kMovedRanges[] = {{0x80138058u, 0x80138268u, 0x00A00000u}, {0x80138268u, 0x80138838u, 0x00A00000u}, {0x80138930u, 0x80138D10u, 0x00A20000u}, {0x80138E5Cu, 0x80139020u, 0x00A00000u}};

// Polynomial sin/cos (identical on every platform); accurate for the small per-frame angles the flight model uses.
float poly_sin(float x);
float poly_cos(float x);
// The game's asin polynomial (0x8001C400).
float asin_a(float x);
void imposter_fly(ImposterCraft& c, const ImposterPad& in, const ImposterWorld& w, float dt);
bool imposter_fire(bool fire, uint16_t* cooldown);
// The fake subtype-0x27 DAT record the imposter wingman spawns from, at kImposterRec.
void build_imposter_record(uint8_t* rdram, const Vec3& pos);
void clear_imposter_record(uint8_t* rdram);
// 1.5 units along player 1's right axis, at player 1's height.
Vec3 imposter_spawn_pos(const Vec3& p1, const Vec3& right);
// The NPC slot table (0x80130BB0) has kNpcSlots 8-byte entries (fake_func_8003E55C allocates 0x4000 bytes); allocateNpcSlot returns 0xFFFF when none is free.
constexpr uint32_t kNpcSlots = 0x800;
// Room for one more NPC: a free context (stack top s32 0x80130BC8 >= 0) and a free slot (free-list head u16 0x80130BB4 != 0xFFFF).
bool npc_pool_room(const uint8_t* rdram);
// spawnNpcOfType can fail (no free slot), and a level without the wingman model leaves the mesh instance NULL.
bool imposter_spawn_ok(uint32_t slot, uint32_t npc, uint32_t blk, uint32_t mesh);
// Whether the level loaded a mesh by name: walkMeshdef0List's lookup (name byte sum % 25 bucket at 0x801394B0, 12-byte chain entries at 0x80139020) without its instance allocation.
bool mesh_loaded(const uint8_t* rdram, const char* name);
// First wingman model the level loaded, as npcWingmanUpdate's record +0x8C index (0 wmxwng .. 7 t16), or -1.
int wingman_model(const uint8_t* rdram);
// The mesh name of a wingman model index (0 wmxwng .. 7 t16), or nullptr.
const char* wingman_model_name(int model);

// Ghost co-op: the remote player's craft, dead-reckoned from the last received state; corrections blend in over kPuppetBlendS, jumps past kPuppetSnap snap.
struct PuppetState { Vec3 pos, fwd, down, vel; };
constexpr double kPuppetCoastS = 0.5;
constexpr double kPuppetBlendS = 0.1;
constexpr float kPuppetSnap = 20.0f;
// With set_interp(delay) > 0 the puppet is shown `delay` seconds in the past on the sender's timeline (entity interpolation): states carry their send time, the clock offset is the smallest (arrival - send) of the last few seconds,
// and the pose is interpolated between buffered samples around now - offset - delay, extrapolated from the newest (capped at kPuppetCoastS) when the buffer runs dry.
class Puppet {
public:
    void reset(const PuppetState& at);
    void set_interp(double delay_s) { interp_s_ = delay_s; }
    void on_state(const PuppetState& s, double now_s, double sent_s = -1.0);
    bool has_target() const { return has_target_; }
    PuppetState step(double now_s, float dt);
    // The interpolated pose at now_s, before any smoothing (interpolation mode).
    PuppetState sample(double now_s) const;
    // Show the latest target at once (a respawned puppet appears where the player is, not sweeping from its old spot).
    void snap() { shown_ = interp_s_ > 0.0 && !buf_.empty() ? buf_.back().s : target_; }
private:
    struct Sample { double sent; PuppetState s; };
    PuppetState shown_{}, target_{};
    double received_s_ = 0.0;
    bool has_target_ = false;
    double interp_s_ = 0.0;
    std::deque<Sample> buf_;
    // (arrival time, arrival - send) of recent states, for the clock offset.
    std::deque<std::pair<double, double>> offsets_;
    double offset_ = 0.0;
};
// Ghost co-op shared lives: a death spends one while two or more are left, the last one puts that player out (spectating), and a death with none left (someone is already out) ends the mission. An out player's death costs nothing.
enum class LifeOutcome { Spend, Out, GameOver, Ignore };
LifeOutcome shared_life(uint8_t pool, bool player_out);
// An out player's pad: its own START (byte 0 bit 0x10) and the connected byte stay, plus the survivor's held fire buttons, so the craft fires with them but never steers.
void spectate_pad(uint8_t pad[6], uint16_t survivor_held, uint16_t fire_mask);
// The mission-select state the host reports: state 4 with the result 0xFFFFFE is a back-out (B), reported as browsing (2) so the client never confirms on it.
int browse_report(int state, uint32_t result);
// The in-mission co-op message line: SPECTATING while this player is out, then WINGMATE LEFT, WINGMATE OUT (down with no lives left) or WINGMATE DOWN (respawning); "" for none.
const char* hud_message(bool out, bool peer_left, bool peer_down, int lives);
// Shared cutscene skip: a local skip is sent to the other player, and their skip pulses START here once this side plays that scene (they may be ahead, still loading). A skip within kEchoS of our own pulses is theirs, not sent back.
// A scene is phase << 8 | skippable scenes entered in that phase; both sides start a phase at the same points (craft select, mission init), so a skip lands on the scene it was made in.
class SkipShare {
public:
    static constexpr double kPulseS = 0.1;
    static constexpr double kEchoS = 1.0;
    // True = send SKIP.
    bool on_local(bool skipped, double now_s) const { return skipped && (last_press_s_ < 0.0 || now_s - last_press_s_ > kEchoS); }
    void on_remote(uint16_t scene);
    // Whether this poll should hold START; scene = this side's current one, cinematic = it is playing. A skip for a later scene waits, one for an earlier scene is dropped.
    bool press(double now_s, bool cinematic, uint16_t scene);
private:
    bool have_ = false;
    uint16_t scene_ = 0;
    double pressing_since_s_ = -1.0;
    double last_press_s_ = -1.0;
};
// Craft select barrier: nobody leaves until both players picked; a player backing out (-1) re-arms it.
struct CraftBarrier {
    int local = -1;
    int remote = -1;
    void pick(int craft) { local = craft; }
    void back() { local = -1; }
    void on_remote(int craft) { remote = craft; }
    bool released() const { return local >= 0 && remote >= 0; }
};
// Shared player trigger volumes: both players count as one team, so an effect runs on the first enter and on the last exit; repeats and exits without an enter run nothing. Player 0 = this machine's craft, 1 = the other player.
class TeamTriggers {
public:
    bool on_edge(int player, uint16_t event, bool enter);
    // That player left: its insides are dropped without running any exit.
    void forget(int player);
    void reset() { inside_.clear(); }
private:
    std::map<uint16_t, uint8_t> inside_;
};
// The wingman model (record +0x8C) showing a remote player's craft: its own model for crafts 0-5 and 7, else -1 (TIE interceptor and Naboo have none).
int puppet_model(int craft);
// The "imposter" frame-hash part (the record at kImposterRec plus its wingman block), registered with add_hash_part while coop_imposter is set.
uint64_t hash_imposter(const uint8_t* rdram);

}
