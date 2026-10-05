#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rs64::ls {

struct SessionHeader {
    uint32_t seed = 1;
    uint32_t level = 0;
    uint32_t craft = 0;
    uint32_t difficulty = 0;
    uint32_t flags0 = 0;
    uint32_t controller = 0;
    uint32_t expansion = 0;
    uint32_t cheats0 = 0;
    uint32_t cheats1 = 0;
    uint32_t naboo = 0;
    uint32_t secondary = 0;
    uint32_t cruise_bits = 0;
    // gMissionState 0x80130B10..0x80130B3F before initMission: rank and per-level progress carried from the pilot.
    std::array<uint8_t, 0x30> pilot = {};
    std::string build;
};

std::string format_header(const SessionHeader& h);
bool parse_header(const std::string& text, SessionHeader* out);

// Session settings as the game holds them at the initMission hook (level from gGameSettings, which initMission copies to 0x80130B70 later).
SessionHeader read_session(const uint8_t* rdram, uint32_t seed);
// Writes everything read_session captures except level and craft, which the menus or nav sequencer set.
void apply_session(uint8_t* rdram, const SessionHeader& h);

struct FrameInput {
    uint32_t frame = 0;
    uint8_t pad[6] = {};
    uint8_t throttle = 0xFF;
};

std::string format_frame(const FrameInput& f);
bool parse_frame(const char* line, FrameInput* out);

// "S <frame> <0|1>": the audio thread's answer to isSongHandleActive in tickSongFadeTimer on that frame (a sim input).
std::string format_song_event(uint32_t frame, bool active);
bool parse_song_event(const char* line, uint32_t* frame, bool* active);

// ROGUESQ_LS_SEED text (decimal or 0x hex) to a seed; 0 for null, junk or zero (the caller keeps its default).
uint32_t parse_seed(const char* s);
// ROGUESQ_LS_INPUT_DELAY clamped to 1..60 frames.
int clamp_delay(long frames);

uint8_t quantize_throttle(float p);
float dequantize_throttle(uint8_t q);

class DelayLine {
public:
    explicit DelayLine(int frames);
    void push_pop(uint8_t pad[6]);
private:
    std::vector<uint8_t> ring_;
    size_t idx_ = 0;
    size_t frames_ = 0;
};

constexpr size_t kRdramSize = 0x800000;

// Per-player neighbour data moved to host RAM (tools/coop/coop_relocation.toml); original addresses, end-exclusive.
struct MovedRange { uint32_t lo; uint32_t hi; uint32_t delta = 0; };
constexpr uint32_t kRelocDelta = 0x00A00000u;
// R4 (0x80138930, a per-view 4 x 0xF8 block) has its own delta so its index-1 entry clears camera node[1].
constexpr MovedRange kMovedRanges[] = {{0x80138058u, 0x80138268u, 0x00A00000u}, {0x80138268u, 0x80138838u, 0x00A00000u}, {0x80138930u, 0x80138D10u, 0x00A20000u}, {0x80138E5Cu, 0x80139020u, 0x00A00000u}};
constexpr size_t kRdramSpan = 0x01000000;
constexpr uint8_t kRelocPoison = 0xA5;

uint32_t relocated(uint32_t kseg0);
void poison_moved(uint8_t* rdram);
uint32_t first_unpoisoned(const uint8_t* rdram);

uint64_t fnv1a(const uint8_t* data, size_t n, uint64_t h = 0xcbf29ce484222325ull);

struct FrameHash {
    uint64_t combined = 0;
    std::vector<uint64_t> parts;
};

std::vector<std::string> hash_part_names(bool imposter = false);
FrameHash hash_frame(const uint8_t* rdram, bool imposter = false);
std::string format_hash_legend(bool imposter = false);

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
// Lobby mission select, mirroring another player's screen: the next button edge for this screen (cursor, state: 2 browse, 3 briefing) given theirs and whether they picked.
enum { kFollowNone = 0, kFollowLeft = 1, kFollowRight = 2, kFollowConfirm = 3, kFollowBack = 4 };
int follow_step(int cursor, int state, int target, int target_state, bool picked);
// The mission-select state the host reports: state 4 with the result 0xFFFFFE is a back-out (B), reported as browsing (2) so the client never confirms on it.
int browse_report(int state, uint32_t result);
// The in-mission co-op message line: SPECTATING while this player is out, then WINGMATE LEFT, WINGMATE OUT (down with no lives left) or WINGMATE DOWN (respawning); "" for none.
const char* hud_message(bool out, bool peer_left, bool peer_down, int lives);
// A cutscene timeline that stopped running (was: gate below its end frame) well before its end was skipped, by whatever button.
bool cutscene_skipped(bool was_running, uint32_t gate, uint32_t end_frame, bool running);
// Shared cutscene skip: a local skip is sent to the other player, and their skip pulses START here once a cutscene plays (they may be ahead, still loading), for up to kWaitS. A skip within kEchoS of our own pulses is theirs, not sent back.
class SkipShare {
public:
    static constexpr double kWaitS = 10.0;
    static constexpr double kPulseS = 0.1;
    static constexpr double kEchoS = 1.0;
    // True = send SKIP.
    bool on_local(bool skipped, double now_s) const { return skipped && (last_press_s_ < 0.0 || now_s - last_press_s_ > kEchoS); }
    void on_remote(double now_s);
    // Whether this poll should hold START.
    bool press(double now_s, bool cinematic);
    // One skippable scene gave way to another: a skip already pressing is used up, one still waiting stays.
    void end_scene() {
        if (pressing_since_s_ >= 0.0) {
            got_s_ = -1.0;
            pressing_since_s_ = -1.0;
        }
    }
private:
    double got_s_ = -1.0;
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
std::string format_hash_line(uint32_t frame, const FrameHash& h);

// One line per byte region ("<name> <hex>"), for diffing two runs at a divergent frame.
std::string dump_regions(const uint8_t* rdram);

}
