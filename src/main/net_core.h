#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rs64::net {

constexpr uint8_t kProtocol = 1;
constexpr size_t kMaxPacket = 1200;

// 1 was the lockstep frame packet and 8 a client objective-write event (both retired); keep them unused.
enum class Msg : uint8_t { Frame = 1, Launch = 2, Bye = 3, State = 4, Result = 5, Presence = 6, Obj = 7, Damage = 9, Life = 10, Mission = 11, Out = 12, Hello = 13, Pick = 14, Ready = 15, Browse = 16, Skip = 17, BriefingDone = 18, Upgrades = 19, Pickup = 20 };
enum class ByeReason : uint8_t { Finished = 0, Desync = 1, Quit = 2, Mismatch = 3 };

std::vector<uint8_t> encode_launch(const std::string& header_text);
bool decode_launch(const uint8_t* data, size_t n, std::string* header_text);
std::vector<uint8_t> encode_bye(ByeReason reason, uint32_t frame);

// What one lobby poll does about the link: connected = a new peer connected and is still up (say HELLO); lost = the previous peer is gone. A connect that dropped before the poll is neither.
struct LinkEdges { bool connected = false; bool lost = false; };
LinkEdges link_poll_edges(bool up, bool was_up, uint32_t connects, uint32_t seen_connects);
bool decode_bye(const uint8_t* data, size_t n, ByeReason* reason, uint32_t* frame);
// StatePacket flags: the sender's player is down (respawning); the receiver hides the puppet until it's back.
constexpr uint8_t kStateDown = 1u << 0;

// Ghost co-op: a player's craft state and pad buttons, sent unreliably every frame; vel is in units per second.
struct StatePacket {
    uint32_t seq = 0;
    // The sender's mission counter: a new mission restarts seq.
    uint8_t epoch = 0;
    float pos[3] = {};
    float fwd[3] = {};
    float down[3] = {};
    float vel[3] = {};
    uint8_t pad[6] = {};
    uint8_t flags = 0;
    // The sender's S-foils, 0 closed .. 250 open (its X-wing's foil state), shown on its puppet.
    uint8_t foil = 250;
    // The sender's steady clock (ms) when it sent this: the receiver's interpolation rebuilds the sender's timeline from it.
    uint32_t sent_ms = 0;
};

std::vector<uint8_t> encode_state(const StatePacket& s);
bool decode_state(const uint8_t* data, size_t n, StatePacket* out);
// Ghost co-op, reliable: the host's mission result (0 abort, 1 success, 2 failed, 3 out of lives) and a player entering (1) or leaving (0) a mission.
std::vector<uint8_t> encode_result(uint8_t epoch, uint8_t result);
bool decode_result(const uint8_t* data, size_t n, uint8_t* epoch, uint8_t* result);
std::vector<uint8_t> encode_presence(uint8_t epoch, uint8_t in_mission);
bool decode_presence(const uint8_t* data, size_t n, uint8_t* epoch, uint8_t* in_mission);
// Ghost co-op objectives: the host's objective booleans, counts, timers and lives (unreliable, newest seq wins).
struct ObjSnapshot {
    uint8_t epoch = 0;
    uint32_t seq = 0;
    uint8_t bools[128] = {};
    int32_t counts[128] = {};
    float timers[8] = {};
    // The shared lives pool (gMissionState+0).
    uint8_t lives = 0;
};

std::vector<uint8_t> encode_obj(const ObjSnapshot& s);
bool decode_obj(const uint8_t* data, size_t n, ObjSnapshot* out);
// Ghost co-op shared deaths: DAT item (index in the level's type-0 list) health drops seen in the sender's world. At most kDamageBatch pairs per packet; encode truncates.
constexpr size_t kDamageBatch = 190;
std::vector<uint8_t> encode_damage(uint8_t epoch, const std::vector<std::pair<uint16_t, int32_t>>& items);
bool decode_damage(const uint8_t* data, size_t n, uint8_t* epoch, std::vector<std::pair<uint16_t, int32_t>>* items);
// Ghost co-op: the host's id for the mission it is starting (sent before LAUNCH); DAMAGE, OBJ and LIFE carry it so traffic from another attempt is dropped.
std::vector<uint8_t> encode_mission(uint8_t id);
bool decode_mission(const uint8_t* data, size_t n, uint8_t* id);
// Ghost co-op: the client lost a life; the host takes it from the shared pool.
std::vector<uint8_t> encode_life_lost(uint8_t epoch);
bool decode_life_lost(const uint8_t* data, size_t n, uint8_t* epoch);
// Lobby mission flow: the host's confirmed mission (level 0..0x12), and a player's confirmed craft (0..8; 0xFF = backed out of craft select).
std::vector<uint8_t> encode_pick(uint8_t level);
bool decode_pick(const uint8_t* data, size_t n, uint8_t* level);
// The host browsing mission select: its cursor (level) and screen state (2 browse, 3 briefing, 4 exiting), sent on change so the client mirrors it live.
std::vector<uint8_t> encode_browse(uint8_t level, uint8_t state);
bool decode_browse(const uint8_t* data, size_t n, uint8_t* level, uint8_t* state);
// A player skipped a cutscene; the other side skips its own.
std::vector<uint8_t> encode_skip();
bool decode_skip(const uint8_t* data, size_t n);
// The client ended the mission briefing; the host confirms its own (the client then follows the host out).
std::vector<uint8_t> encode_briefing_done();
bool decode_briefing_done(const uint8_t* data, size_t n);
std::vector<uint8_t> encode_ready(uint8_t craft);
bool decode_ready(const uint8_t* data, size_t n, uint8_t* craft);
// Ghost co-op, host to client: the client's death took the last shared life, so it is out and spectates until the mission ends.
std::vector<uint8_t> encode_out(uint8_t epoch);
bool decode_out(const uint8_t* data, size_t n, uint8_t* epoch);
// Power-up bits in the settings word 0x80130B4C that npcPowerUpUpdate ORs in (0x200 adv blasters .. 0x10000 adv shields).
constexpr uint32_t kUpgradeMask = 0x0001FE00u;
// Ghost co-op, reliable: upgrades a player picked up this mission; the other side ORs them into its own word.
std::vector<uint8_t> encode_upgrades(uint8_t epoch, uint32_t bits);
bool decode_upgrades(const uint8_t* data, size_t n, uint8_t* epoch, uint32_t* bits);
// Ghost co-op, reliable: a player collected the power-up with this DAT item index; the other side collects its own copy.
std::vector<uint8_t> encode_pickup(uint8_t epoch, uint16_t item);
bool decode_pickup(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* item);
// Network compatibility of this build: bump it whenever two builds can no longer play together. The lobby's HELLO carries it.
constexpr uint32_t kBuildId = 0x52530007u;
// Lobby handshake, sent once by each side on connect. It decodes whatever the sender's protocol byte, so a mismatch can be reported.
struct Hello {
    uint16_t protocol = kProtocol;
    uint32_t build = kBuildId;
    // 0 host, 1 client.
    uint8_t role = 0;
    // The pilot name (3 letters, gGameSettings+7), shown over the other player's ship.
    char name[3] = {0, 0, 0};
};
std::vector<uint8_t> encode_hello(const Hello& h);
bool decode_hello(const uint8_t* data, size_t n, Hello* out);

// The lobby's IPv4 entry: four octets, shown without dots (menu font 5 has no '.'); while editing, the octet under the cursor is bracketed.
// Menu input as direction/button bits, independent of the pad layout.
constexpr uint32_t kPadUp = 1, kPadDown = 2, kPadLeft = 4, kPadRight = 8, kPadB = 16, kPadA = 32;
// LAN discovery, plain UDP on the game port + 1 (outside ENet): a joiner broadcasts a query, a host answers with its game port and pilot name.
constexpr uint16_t kDiscoveryPortOffset = 1;
struct DiscoveryReply {
    uint16_t port = 0;
    uint32_t build = kBuildId;
    char name[3] = {0, 0, 0};
};
std::vector<uint8_t> encode_discovery_query();
bool is_discovery_query(const uint8_t* data, size_t n);
std::vector<uint8_t> encode_discovery_reply(const DiscoveryReply& r);
bool decode_discovery_reply(const uint8_t* data, size_t n, DiscoveryReply* out);

// Lobby lines that mean "still trying" get dots appearing one at a time (kDotPeriodMs each, back to none after three), padded to a fixed width so the centred line stays put.
constexpr uint32_t kDotPeriodMs = 600;
// Pads the missing dots: a blank glyph as wide as the period (the menu font's Ö slot, made at runtime with the period; see ensure_menu_period).
constexpr char kDotPad = '`';
// The typing caret: a thin italic bar (the menu font's Ä slot, made at runtime from the stem of '!'; see ensure_menu_period).
constexpr char kCaret = '|';
bool waiting_status(const std::string& status);
std::string status_dots(const std::string& status, uint32_t now_ms);

// Internet hosting through UPnP: whether the router opened the game port, and the host page line for it (menu font: uppercase, digits, spaces, periods).
enum class PortMapState { Idle, Working, Open, NoRouter, Refused, SharedAddress };
// An address the internet can reach: not private, carrier-grade NAT (100.64/10), loopback or link-local. Host byte order.
bool public_ipv4(uint32_t ip);
std::string online_label(PortMapState state);
// The host's address line: the internet address once the router opened the port, else the LAN one (lan_ip empty: no network). Addresses are dotted (the menu font's period is added at runtime, ensure_menu_period).
std::string host_address_label(PortMapState state, const std::string& lan_ip, const std::string& ext_ip, uint16_t port = 0);

// HOST GAME's address: the row's IPv4 when it is one of this machine's or loopback (bind = shown = row), else bind any (0.0.0.0 asks for that) and show the fallback (0: none).
struct HostAddress {
    uint32_t bind = 0;
    uint32_t shown = 0;
};
HostAddress choose_host_address(std::optional<uint32_t> row, const std::vector<uint32_t>& local, std::optional<uint32_t> fallback);
std::string ipv4_text(uint32_t ip);

struct AddressEditor {
    uint8_t octet[4] = {192, 168, 1, 2};
    // A typed ":port" (0: none, the configured port applies).
    uint16_t port = 0;
    int cursor = 0;
    bool editing = false;
    void move(int dir);
    void step(int delta);
    // One menu frame while editing: up/down step the octet (held: every 3 frames after 12, by 10 from 45), left/right move, A or B end editing. True = the input was taken and the menu must not see it (every frame while editing).
    bool input(uint32_t pressed, uint32_t held);
    int held_frames = 0;
    // A saved address the octets cannot show (a hostname, IPv6) is kept and joined as-is until the player edits the octets.
    std::string saved;
    bool touched = false;
    void load(const std::string& s);
    // Keyboard entry (the phone's on-screen keyboard, or a desktop keyboard): digits, '.' and ':', at most 21 (an address and port), shown with a caret (kCaret); the pad only ends it (A or B).
    bool typing = false;
    std::string typed;
    void begin_typing();
    void type_text(const char* s);
    void type_backspace();
    // Ends typing and editing; a typed address that parses replaces the octets (true), anything else leaves them.
    bool end_typing();
    // Ends typing and editing without applying the typed text (the page closed mid-entry).
    void cancel_typing();
    // The address JOIN connects to, without the port.
    std::string target() const;
    // target() plus ":port" when one was typed (what roguesq_net.json keeps).
    std::string entry() const;
    // The row's octets, unless it holds a saved address they cannot show.
    std::optional<uint32_t> ipv4() const;
    std::string label() const;
    std::string text() const;
    // "a.b.c.d" or "a.b.c.d:port" (port 1-65535).
    static bool parse(const std::string& s, AddressEditor* out);
};

// The join code a relay transport's host shows (mp.transport host_label): kDigits digits. Pad: up/down step the digit under the cursor (wrapping), left/right move, A or B end editing. Keyboard: digits only; a complete code replaces it.
struct CodeEditor {
    static constexpr int kDigits = 6;
    char digit[kDigits] = {'0', '0', '0', '0', '0', '0'};
    int cursor = 0;
    bool editing = false;
    bool typing = false;
    std::string typed;
    bool input(uint32_t pressed, uint32_t held);
    void begin_typing();
    void type_text(const char* s);
    void type_backspace();
    // Ends typing and editing; true when the typed text was a full code and replaced it.
    bool end_typing();
    void cancel_typing();
    void load(const std::string& s);
    std::string code() const;
    std::string label() const;
    static bool valid(const std::string& s);
};

// Lobby progress for one side: join gives up after kJoinTimeoutMs, a connected peer must say HELLO within kHelloTimeoutMs, and a HELLO from another build fails both sides. A host whose player leaves goes back to waiting.
enum class LobbyState { Idle, Hosting, Joining, Connected, Failed };
class Lobby {
public:
    static constexpr uint32_t kJoinTimeoutMs = 15000;
    static constexpr uint32_t kHelloTimeoutMs = 5000;
    static constexpr uint32_t kLeftNoticeMs = 5000;
    void host();
    void join();
    void cancel();
    // The link could not start (the host's port is taken, no network).
    void on_start_failed();
    void on_connected(uint32_t now_ms);
    void on_hello(const Hello& h);
    void on_lost();
    void tick(uint32_t now_ms);
    LobbyState state() const { return state_; }
    bool hosting() const { return host_; }
    // Menu-font-safe status line: uppercase, digits, spaces.
    std::string status() const;
    bool ready() const { return state_ == LobbyState::Connected; }
    // The build this side compares HELLOs against and sends (tests override it to force a mismatch).
    void set_local_build(uint32_t b) { build_ = b; }
    // ROGUESQ_MP=join waits as long as the scripted path always did.
    void set_join_timeout(uint32_t ms) { join_timeout_ = ms; }
    uint32_t local_build() const { return build_; }
private:
    void reset(LobbyState s, bool host);
    LobbyState state_ = LobbyState::Idle;
    bool host_ = false;
    bool peer_ = false;
    int64_t started_ms_ = -1;
    int64_t connected_ms_ = -1;
    bool left_ = false;
    int64_t left_ms_ = -1;
    std::string fail_;
    uint32_t build_ = kBuildId;
    uint32_t join_timeout_ = kJoinTimeoutMs;
};

// The Msg of a packet with our protocol byte (any protocol byte for a HELLO), or 0.
uint8_t message_type(const uint8_t* data, size_t n);

}
