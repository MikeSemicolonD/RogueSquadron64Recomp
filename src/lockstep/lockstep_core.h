#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

// Game data a mod moved to host RAM (the multiplayer mod's per-player neighbour ranges): original addresses, end-exclusive. Empty unless a mod registers ranges (host API add_moved_range).
struct MovedRange { uint32_t lo; uint32_t hi; uint32_t delta = 0; };
void set_moved_ranges(const std::vector<MovedRange>& ranges);
const std::vector<MovedRange>& moved_ranges();
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

// A frame-hash part a mod adds (host API add_hash_part), appended after the base parts in registration order.
struct HashPart {
    std::string name;
    uint64_t (*fn)(const uint8_t* rdram, void* user);
    void* user;
};

std::vector<std::string> hash_part_names(const std::vector<HashPart>& extra = {});
FrameHash hash_frame(const uint8_t* rdram, const std::vector<HashPart>& extra = {});
std::string format_hash_legend(const std::vector<HashPart>& extra = {});

// Lobby mission select, mirroring another player's screen: the next button edge for this screen (cursor, state: 2 browse, 3 briefing) given theirs and whether they picked.
enum { kFollowNone = 0, kFollowLeft = 1, kFollowRight = 2, kFollowConfirm = 3, kFollowBack = 4 };
int follow_step(int cursor, int state, int target, int target_state, bool picked);
// A cutscene timeline that stopped running (was: gate below its end frame) well before its end was skipped, by whatever button.
bool cutscene_skipped(bool was_running, uint32_t gate, uint32_t end_frame, bool running);
std::string format_hash_line(uint32_t frame, const FrameHash& h);

// One line per byte region ("<name> <hex>"), for diffing two runs at a divergent frame.
std::string dump_regions(const uint8_t* rdram);

// A recording: its header, frames and song events. False, with a log line, when the file can't be read or its header is bad.
bool load_replay(const char* path, SessionHeader* h, std::vector<FrameInput>* frames, std::map<uint32_t, std::vector<uint8_t>>* song);
// An attract demo is playing (gGameSettings flags 0x80130B50 & 0x60).
bool demo_active(const uint8_t* rdram);

// A ROGUESQ_PAUSE_AT target: hold once `clock` reaches `frame`.
struct PauseTarget { std::string clock; uint32_t frame = 0; bool done = false; };

// "clock[:frame],..." (spaces around commas ignored, no frame = 0).
inline std::vector<PauseTarget> parse_pause_targets(const std::string& spec)
{

    std::vector<PauseTarget> out;

    for (size_t at = 0; at <= spec.size();)
    {
        size_t end = spec.find(',', at);

        if (end == std::string::npos) end = spec.size();

        std::string item = spec.substr(at, end - at);

        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);

        const size_t colon = item.rfind(':');

        if (!item.empty()) out.push_back({ item.substr(0, colon), colon == std::string::npos ? 0u : (uint32_t)std::strtoul(item.c_str() + colon + 1, nullptr, 10) });

        at = end + 1;
    }

    return out;
}

// A target names its clock or, at a dot, any of its sub-states (menu covers menu.options).
inline bool pause_clock_matches(const std::string& target, const std::string& clock)
{

    return clock == target || (clock.size() > target.size() && clock.compare(0, target.size(), target) == 0 && clock[target.size()] == '.');
}

}
